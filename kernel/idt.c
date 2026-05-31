#include "types.h"
#include "kernel.h"
#include "kio.h"
#include "io.h"

extern void elf_syscall(u32 eax, u32 ebx, u32 ecx, u32 edx);

struct idt_entry {
    u16 base_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   ip;

static void set(int n, u32 handler, u16 sel, u8 flags) {
    idt[n].base_lo = handler & 0xFFFF;
    idt[n].base_hi = (handler >> 16) & 0xFFFF;
    idt[n].sel = sel;
    idt[n].zero = 0;
    idt[n].flags = flags;
}

/* Stubs defined in isr.asm. */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void isr128(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static void (*irq_handlers[16])(void);

void irq_install_handler(u8 irq, void (*fn)(void)) {
    if (irq < 16) irq_handlers[irq] = fn;
}
void irq_uninstall_handler(u8 irq) {
    if (irq < 16) irq_handlers[irq] = NULL;
}

/* Called from isr.asm common stubs. */
struct iregs {
    u32 ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
};

static const char *exception_names[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound range", "Invalid opcode", "Device n/a",
    "Double fault", "Coproc segment", "Invalid TSS", "Segment not present",
    "Stack-segment", "GP fault", "Page fault", "Reserved",
    "x87 FP", "Alignment check", "Machine check", "SIMD FP",
    "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved",
};

void isr_handler(struct iregs *r) {
    if (r->int_no == 128) {
        elf_syscall(r->eax, r->ebx, r->ecx, r->edx);
        return;
    }
    const char *n = (r->int_no < 32) ? exception_names[r->int_no] : "Unknown";
    panic("Exception #%u (%s) err=0x%x eip=0x%x", r->int_no, n, r->err_code, r->eip);
}

void irq_handler(struct iregs *r) {
    u32 irq = r->int_no - 32;
    if (irq >= 8) outb(0xA0, 0x20);     /* slave EOI */
    outb(0x20, 0x20);                   /* master EOI */
    if (irq < 16 && irq_handlers[irq]) irq_handlers[irq]();
}

void idt_init(void) {
    ip.limit = sizeof(idt) - 1;
    ip.base  = (u32)&idt;
    for (int i = 0; i < 256; i++) set(i, 0, 0, 0);

    set(0,  (u32)isr0,  0x08, 0x8E);
    set(1,  (u32)isr1,  0x08, 0x8E);
    set(2,  (u32)isr2,  0x08, 0x8E);
    set(3,  (u32)isr3,  0x08, 0x8E);
    set(4,  (u32)isr4,  0x08, 0x8E);
    set(5,  (u32)isr5,  0x08, 0x8E);
    set(6,  (u32)isr6,  0x08, 0x8E);
    set(7,  (u32)isr7,  0x08, 0x8E);
    set(8,  (u32)isr8,  0x08, 0x8E);
    set(9,  (u32)isr9,  0x08, 0x8E);
    set(10, (u32)isr10, 0x08, 0x8E);
    set(11, (u32)isr11, 0x08, 0x8E);
    set(12, (u32)isr12, 0x08, 0x8E);
    set(13, (u32)isr13, 0x08, 0x8E);
    set(14, (u32)isr14, 0x08, 0x8E);
    set(15, (u32)isr15, 0x08, 0x8E);
    set(16, (u32)isr16, 0x08, 0x8E);
    set(17, (u32)isr17, 0x08, 0x8E);
    set(18, (u32)isr18, 0x08, 0x8E);
    set(19, (u32)isr19, 0x08, 0x8E);
    set(20, (u32)isr20, 0x08, 0x8E);
    set(21, (u32)isr21, 0x08, 0x8E);
    set(22, (u32)isr22, 0x08, 0x8E);
    set(23, (u32)isr23, 0x08, 0x8E);
    set(24, (u32)isr24, 0x08, 0x8E);
    set(25, (u32)isr25, 0x08, 0x8E);
    set(26, (u32)isr26, 0x08, 0x8E);
    set(27, (u32)isr27, 0x08, 0x8E);
    set(28, (u32)isr28, 0x08, 0x8E);
    set(29, (u32)isr29, 0x08, 0x8E);
    set(30, (u32)isr30, 0x08, 0x8E);
    set(31, (u32)isr31, 0x08, 0x8E);

    set(32, (u32)irq0,  0x08, 0x8E);
    set(33, (u32)irq1,  0x08, 0x8E);
    set(34, (u32)irq2,  0x08, 0x8E);
    set(35, (u32)irq3,  0x08, 0x8E);
    set(36, (u32)irq4,  0x08, 0x8E);
    set(37, (u32)irq5,  0x08, 0x8E);
    set(38, (u32)irq6,  0x08, 0x8E);
    set(39, (u32)irq7,  0x08, 0x8E);
    set(40, (u32)irq8,  0x08, 0x8E);
    set(41, (u32)irq9,  0x08, 0x8E);
    set(42, (u32)irq10, 0x08, 0x8E);
    set(43, (u32)irq11, 0x08, 0x8E);
    set(44, (u32)irq12, 0x08, 0x8E);
    set(45, (u32)irq13, 0x08, 0x8E);
    set(46, (u32)irq14, 0x08, 0x8E);
    set(47, (u32)irq15, 0x08, 0x8E);

    set(0x80, (u32)isr128, 0x08, 0x8E);

    __asm__ volatile("lidt (%0)" :: "r"(&ip));
}
