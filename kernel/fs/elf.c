/* ELF32 executable loader + INT 0x80 syscall handler.
 * Supports a Linux i386 ABI subset: exit, read(stdin), write(stdout/stderr).
 * ELF programs are loaded into their requested virtual addresses
 * (64 MB identity-mapped by paging_init).
 */
#include "elf.h"
#include "kernel.h"
#include "kio.h"
#include "string.h"
#include "fs.h"
#include "types.h"

/* Syscall numbers (Linux i386 ABI subset) */
#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6

/* Static load buffer and stack */
static u8  elf_load_buf[65536];
static u8  elf_stack[16384];

/* ELF execution state */
static volatile int g_elf_running;
static volatile int g_elf_exit_code;
/* saved[0]=ebp, [1]=ebx, [2]=esi, [3]=edi, [4]=esp, [5]=return eip */
static u32 g_elf_saved[8];

void elf_syscall(u32 eax, u32 ebx, u32 ecx, u32 edx) {
    switch (eax) {
    case SYS_EXIT:
        g_elf_exit_code = (int)ebx;
        g_elf_running   = 0;
        __asm__ volatile(
            "mov %0, %%ebp\n\t"
            "mov %1, %%ebx\n\t"
            "mov %2, %%esi\n\t"
            "mov %3, %%edi\n\t"
            "mov %4, %%esp\n\t"
            "jmp *%5\n\t"
            : : "m"(g_elf_saved[0]), "m"(g_elf_saved[1]),
                "m"(g_elf_saved[2]), "m"(g_elf_saved[3]),
                "m"(g_elf_saved[4]), "m"(g_elf_saved[5])
            : "memory"
        );
        /* unreachable */
        break;
    case SYS_WRITE:
        if (ebx == 1 || ebx == 2) {
            const char *buf = (const char *)ecx;
            u32 len = edx;
            for (u32 i = 0; i < len; i++) kputc(buf[i]);
        }
        break;
    case SYS_READ:
        if (ebx == 0) {
            char *buf = (char *)ecx;
            u32 len = edx;
            if (len > 0) { buf[0] = (char)kb_getc(); }
        }
        break;
    default:
        break;
    }
}

int elf_exec(const char *path) {
    /* 1. Open and read the ELF file */
    int h = fs_open(path);
    if (h < 0) {
        kprintf("elf: cannot open %s\n", path);
        return -1;
    }
    int fsize = fs_size(h);
    if (fsize < 0 || (u32)fsize > sizeof(elf_load_buf)) {
        kprintf("elf: file too large (%d bytes, max %u)\n", fsize, (u32)sizeof(elf_load_buf));
        fs_close(h);
        return -1;
    }
    memset(elf_load_buf, 0, sizeof(elf_load_buf));
    int n = fs_read(h, elf_load_buf, (size_t)fsize);
    fs_close(h);
    if (n < 52) {
        kprintf("elf: file too small\n");
        return -1;
    }

    /* 2. Parse and validate ELF header */
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)elf_load_buf;
    if (ehdr->e_ident[0] != ELF_MAGIC0 ||
        ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 ||
        ehdr->e_ident[3] != ELF_MAGIC3) {
        kprintf("elf: bad magic\n");
        return -1;
    }
    if (ehdr->e_machine != EM_386) {
        kprintf("elf: not i386\n");
        return -1;
    }
    if (ehdr->e_type != ET_EXEC) {
        kprintf("elf: not an executable\n");
        return -1;
    }

    /* 3. Load PT_LOAD segments */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        u32 off = ehdr->e_phoff + (u32)i * (u32)ehdr->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > (u32)fsize) break;
        Elf32_Phdr *ph = (Elf32_Phdr *)(elf_load_buf + off);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_filesz > 0)
            memcpy((void *)ph->p_vaddr, elf_load_buf + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((void *)(ph->p_vaddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
    }

    /* 4. Set up initial stack */
    u32 *sp = (u32 *)(elf_stack + sizeof(elf_stack) - 16);
    *--sp = 0;   /* end of argv */
    *--sp = 0;   /* argv */
    *--sp = 0;   /* argc */
    u32 esp_init = (u32)sp;
    u32 entry    = ehdr->e_entry;

    /* 5. Save kernel context, switch to ELF stack, jump to entry */
    g_elf_running   = 1;
    g_elf_exit_code = 0;

    __asm__ volatile(
        "mov %%ebp, %0\n\t"
        "mov %%ebx, %1\n\t"
        "mov %%esi, %2\n\t"
        "mov %%edi, %3\n\t"
        "mov %%esp, %4\n\t"
        "lea 0f, %%eax\n\t"
        "mov %%eax, %5\n\t"
        "mov %6, %%esp\n\t"
        "push $0\n\t"
        "jmp *%7\n\t"
        "0:\n\t"
        : "=m"(g_elf_saved[0]), "=m"(g_elf_saved[1]),
          "=m"(g_elf_saved[2]), "=m"(g_elf_saved[3]),
          "=m"(g_elf_saved[4]), "=m"(g_elf_saved[5])
        : "r"(esp_init), "r"(entry)
        : "eax", "ecx", "edx", "memory"
    );

    return g_elf_exit_code;
}
