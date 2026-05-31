/* PS/2 keyboard driver: scancode set 1 -> ASCII, blocking ring-buffer queue.
 * Falls back to direct port polling if IRQ1 is not arriving (e.g. some VMs).
 * Also drains the USB HID keyboard via usb_kbd_trygetc().
 */
#include "io.h"
#include "kernel.h"
#include "vga.h"
#include "../include/usb.h"

extern void pic_unmask(u8 irq);

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define BUF_SIZE 64
static volatile char buf[BUF_SIZE];
static volatile u32  head, tail;

static int shift_down, ctrl_down, caps_lock, altgr_down;

/* Sticky Ctrl+C flag. Cleared by kb_intr_consume(); set whenever the
 * scancode for ^C is processed. The shell polls this between commands
 * (and inside long-running builtins) to bail out. */
volatile int g_intr;
int kb_intr_pending(void) { return g_intr; }
void kb_intr_consume(void) { g_intr = 0; }
static int ext_flag = 0;

/* Keyboard layout. 0 = US, 1 = DE (QWERTZ). The two layouts differ a lot
 * on the symbol rows, the Y/Z swap, and the umlaut keys; AltGr unlocks
 * @ { [ ] } \ ~ on DE which are otherwise unreachable. CP437 codes are
 * used for the umlauts and ess-zett so they render correctly in VGA text
 * mode without a font change. */
static int layout = 0;

static const char us_base[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ',
};

static const char us_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0, ' ',
};

/* DE QWERTZ. CP437 bytes: 0x81=u-umlaut, 0x84=a-umlaut, 0x94=o-umlaut,
 * 0x8E=A-umlaut, 0x99=O-umlaut, 0x9A=U-umlaut, 0xE1=eszett, 0xF8=degree. */
#define UM_a  (char)0x84
#define UM_o  (char)0x94
#define UM_u  (char)0x81
#define UM_A  (char)0x8E
#define UM_O  (char)0x99
#define UM_U  (char)0x9A
#define SS    (char)0xE1
#define DEG   (char)0xF8

static const char de_base[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]=SS,[0x0D]='\'',
    [0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='z',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]=UM_u,[0x1B]='+',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',
    [0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
    [0x27]=UM_o,[0x28]=UM_a,[0x29]='^',[0x2B]='#',
    [0x2C]='y',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='-',
    [0x39]=' ',
};

static const char de_shift[128] = {
    [0x02]='!',[0x03]='"',[0x04]='\x15',[0x05]='$',[0x06]='%',[0x07]='&',
    [0x08]='/',[0x09]='(',[0x0A]=')',[0x0B]='=',[0x0C]='?',[0x0D]='`',
    [0x0E]='\b',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Z',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
    [0x1A]=UM_U,[0x1B]='*',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',
    [0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',
    [0x27]=UM_O,[0x28]=UM_A,[0x29]=DEG,[0x2B]='\'',
    [0x2C]='Y',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',
    [0x33]=';',[0x34]=':',[0x35]='_',
    [0x39]=' ',
};

static const char de_altgr[128] = {
    [0x08]='{', [0x09]='[', [0x0A]=']', [0x0B]='}', [0x0C]='\\',
    [0x10]='@', [0x1B]='~', [0x2B]='|',
};

void kb_set_layout(const char *name) {
    if (!name) return;
    if (name[0] == 'd' || name[0] == 'D') layout = 1;
    else                                   layout = 0;
}

const char *kb_get_layout(void) { return layout ? "de" : "us"; }

static void enqueue(char c) {
    u32 next = (head + 1) % BUF_SIZE;
    if (next == tail) return;       /* drop on overflow */
    buf[head] = c;
    head = next;
}

static void on_irq1(void) {
    u8 sc = inb(KBD_DATA);

    /* Extended key prefix */
    if (sc == 0xE0) { ext_flag = 1; return; }

    if (ext_flag) {
        ext_flag = 0;
        /* AltGr (right Alt) = E0 38 down, E0 B8 up. Critical on DE
         * layouts where @ { } [ ] \ etc. are only reachable via AltGr. */
        if (sc == 0x38) { altgr_down = 1; return; }
        if (sc == 0xB8) { altgr_down = 0; return; }
        /* Other key-release for extended keys: ignore */
        if (sc & 0x80) return;
        char ext_c = 0;
        switch (sc) {
            case 0x48: ext_c = (char)KB_UP;    break;
            case 0x50: ext_c = (char)KB_DOWN;  break;
            case 0x4B: ext_c = (char)KB_LEFT;  break;
            case 0x4D: ext_c = (char)KB_RIGHT; break;
            case 0x53: ext_c = (char)KB_DEL;   break;
            case 0x47: ext_c = (char)KB_HOME;  break;
            case 0x4F: ext_c = (char)KB_END;   break;
            case 0x49: ext_c = (char)KB_PGUP;  break;
            case 0x51: ext_c = (char)KB_PGDN;  break;
            default:   break;
        }
        if (ext_c) enqueue(ext_c);
        return;
    }

    if (sc & 0x80) {
        u8 down = sc & 0x7F;
        if (down == 0x2A || down == 0x36) shift_down = 0;
        if (down == 0x1D) ctrl_down = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0x1D)               { ctrl_down  = 1; return; }
    if (sc == 0x3A)               { caps_lock ^= 1; return; }

    /* F-keys (no E0 prefix) */
    char fkey = 0;
    switch (sc) {
        case 0x3B: fkey = (char)KB_F1; break;
        case 0x3C: fkey = (char)KB_F2; break;
        case 0x3D: fkey = (char)KB_F3; break;
        case 0x3E: fkey = (char)KB_F4; break;
        case 0x3F: fkey = (char)KB_F5; break;
        case 0x40: fkey = (char)KB_F6; break;
        case 0x41: fkey = (char)KB_F7; break;
        case 0x42: fkey = (char)KB_F8; break;
        default:   break;
    }
    if (fkey) { enqueue(fkey); return; }

    char c = 0;
    if (layout == 1) {
        if (altgr_down)        c = de_altgr[sc];
        if (!c)                c = (shift_down ? de_shift : de_base)[sc];
    } else {
        c = (shift_down ? us_shift : us_base)[sc];
    }
    if (!c) return;
    if (caps_lock && c >= 'a' && c <= 'z') c -= 32;
    else if (caps_lock && c >= 'A' && c <= 'Z' && !shift_down) c += 32;
    /* Ctrl+letter -> control character (0x03 = Ctrl+C, used to interrupt
     * the running shell command). Latched via g_intr so a long-running
     * builtin can poll for the flag between iterations. */
    if (ctrl_down && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl_down && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    if (c == 0x03) g_intr = 1;
    enqueue(c);
}

void keyboard_init(void) {
    /* Flush any stale bytes sitting in the i8042 output buffer. */
    while (inb(KBD_STATUS) & 0x01) inb(KBD_DATA);

    /* Re-enable the PS/2 keyboard port in case BIOS handed it off disabled. */
    while (inb(KBD_STATUS) & 0x02); /* wait for input buffer empty */
    outb(KBD_STATUS, 0xAE);         /* enable first PS/2 port */
    io_wait();

    head = tail = 0;
    irq_install_handler(1, on_irq1);
    pic_unmask(1);
}

int kb_trygetc(void) {
    /* PS/2 polling fallback: if a KEYBOARD byte is queued in the i8042
     * output buffer (bit 0 set, bit 5 CLEAR) and IRQ1 hasn't drained it
     * yet, do it now. Critically, we must NOT read when bit 5 is set --
     * that's a mouse byte. Reading it here would interpret e.g. a Y
     * delta of 0x01 as the ESC scancode (and "exit" full-screen apps
     * the moment the user moves the mouse). */
    if (head == tail) {
        u8 st = inb(KBD_STATUS);
        if ((st & 0x21) == 0x01)
            on_irq1();
    }

    /* USB HID keyboard */
    {
        int c = usb_kbd_trygetc();
        if (c >= 0) return c;
    }

    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return (u8)c;
}

int kb_getc(void) {
    /* Make sure anything drawn since the last present is visible before
     * we go to sleep waiting for input -- otherwise the user is staring
     * at a stale frame. */
    vga_present();
    int c;
    for (;;) {
        c = kb_trygetc();
        if (c >= 0) return c;
        __asm__ volatile("hlt");
    }
}
