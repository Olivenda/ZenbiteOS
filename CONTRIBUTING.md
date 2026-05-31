# Contributing to Zenbite

Thanks for wanting to hack on Zenbite. This document is the short version of how to do that
successfully.

## Ground rules

- Be kind. The [Code of Conduct](CODE_OF_CONDUCT.md) applies everywhere — issues, PRs,
  discussions, commit messages.
- Zenbite is MIT-licensed. By contributing you agree your contribution may be redistributed
  under the same terms.
- Sign off every commit with `Signed-off-by: Your Name <you@example.com>` (this is the
  Developer Certificate of Origin — `git commit -s` does it automatically). We do not
  require a CLA.

## Setting up your toolchain

On Debian / Ubuntu / WSL the entire dependency list is one apt line:

```sh
sudo apt install build-essential gcc-multilib nasm qemu-system-x86 mtools dosfstools
make
make run     # boots in QEMU
```

We use the host `gcc -m32 -ffreestanding -nostdlib` directly — no cross-
compiler download required. The `-ffreestanding -nostdlib -fno-builtin`
flags guarantee the kernel doesn't accidentally link against host libc.

If you prefer a dedicated `i686-elf` cross-compiler anyway, see
[`tools/TOOLCHAIN.md`](tools/TOOLCHAIN.md) — pass `make CROSS=i686-elf-`.

If `make run` shows the `A:\>` prompt, you are ready to hack.

## Coding style

Kernel and shell code follow a small, boring style — readability beats cleverness.

- **C:** K&R-ish braces; 4-space indent; `snake_case` for functions, variables, and file
  names; `UPPER_SNAKE_CASE` for macros and enum members; one declaration per line; pointers
  bind to the variable (`char *p`, not `char* p`).
- **Assembly:** NASM, Intel syntax; lower-case mnemonics; labels in `snake_case`; comments
  with `;` aligned to column 40 where it improves scannability.
- No `malloc` / dynamic allocation in interrupt context. Allocate up front.
- Every public function gets a one-line comment above it stating *why* it exists, not
  *what* it does. The code says what.
- Width: aim for 100 columns, hard wrap at 120.

We do not (yet) enforce a formatter. Match the file you are editing.

## Project layout

See [the plan](#layout-cheatsheet) at the bottom of this file for the full tree.

- `boot/` — Stage1 and Stage2 bootloader (NASM).
- `kernel/` — 32-bit kernel: entry, GDT/IDT, drivers, memory, FS.
- `shell/` — DOS-style command interpreter.
- `libk/` — freestanding libc subset (`memcpy`, `printf`, `string`).
- `include/` — public headers shared between kernel and shell.
- `scripts/` — image build, QEMU launcher.
- `tools/` — toolchain notes (no checked-in binaries).
- `.github/` — CI, issue/PR templates.

## Pull request checklist

Before you open a PR:

- [ ] `make` builds cleanly with no new warnings (`-Wall -Wextra` is on).
- [ ] `make test` passes (CI will run this too).
- [ ] You added or updated tests where it makes sense (kernel unit tests live in
      `kernel/tests/` and run on the host with `make check`).
- [ ] You ran the change in QEMU and the shell still works.
- [ ] You added your name to `AUTHORS` if this is your first contribution.
- [ ] Commits are signed off (`git commit -s`).
- [ ] Commit messages explain *why* the change exists.

Open the PR against `main`. CI must be green before review.

## What to work on

- Bugs and `good-first-issue`-labelled tickets are the friendliest entry points.
- For larger changes (new drivers, new FS, the GUI), please open an issue first so we can
  agree on the approach before you spend a weekend on it.
- The roadmap in [`README.md`](README.md#roadmap) lists the next big buckets.

## Layout cheatsheet

```
boot/      stage1.asm, stage2.asm, boot.ld
kernel/    entry.asm, kmain.c, gdt.c, idt.c, isr.asm, irq.c, pic.c, pit.c,
           keyboard.c, vga.c, cpuid.c, panic.c
kernel/mm/ pmm.c, paging.c, kheap.c
kernel/fs/ fat12.c, vfs.c
kernel/drv/ata.c, floppy.c, serial.c
shell/     shell.c, parser.c, builtins.c, history.c
libk/      string.c, printf.c, ctype.c, memcpy.c
include/   public headers
```
