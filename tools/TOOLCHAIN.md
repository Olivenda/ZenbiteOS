# Toolchain

Zenbite builds with **your host gcc** in 32-bit multilib mode. No cross-compiler
required. The kernel still runs in 32-bit protected mode on the target — at
runtime CPUID detects 64-bit hosts and prints an info banner.

## Debian / Ubuntu

```sh
sudo apt install build-essential gcc-multilib nasm qemu-system-x86 mtools dosfstools
make
make run
```

On WSL / Debian this is everything. No downloads beyond apt.

## macOS

`gcc -m32` doesn't ship on modern macOS. Two options:

1. Build a cross-compiler — `tools/build-toolchain.sh` does this, then
   `make CROSS=i686-elf-` uses it. (See bottom of this file.)
2. Run the build inside a Docker container based on `debian:stable-slim`
   with the apt line above.

## Optional: dedicated i686-elf cross-compiler

If you'd rather use a freestanding cross-compiler (cleaner, but a one-time
big download/build), `tools/build-toolchain.sh` builds it into
`~/opt/i686-elf`. Then:

```sh
export PATH="$HOME/opt/i686-elf/bin:$PATH"
make CROSS=i686-elf-
```

Both paths produce identical binaries because the kernel is freestanding and
never links against any libc.
