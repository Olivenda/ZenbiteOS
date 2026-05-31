<h1 align="center">Zenbite</h1>
<p align="center">
  <em>A from-scratch 32-bit retro operating system, in the spirit of MS-DOS / FreeDOS.</em>
</p>

<p align="center">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-blue?style=for-the-badge"></a>
  <a href="https://github.com/olivenda/zenbite-/releases"><img alt="Release" src="https://img.shields.io/badge/release-v0.3-darkorange?style=for-the-badge"></a>
  <a href="https://github.com/olivenda/zenbite-/actions"><img alt="Build" src="https://img.shields.io/badge/build-passing-success?style=for-the-badge"></a>
  <a href="https://github.com/olivenda/zenbite-/stargazers"><img alt="Stars" src="https://img.shields.io/badge/stars-%E2%98%85-yellow?style=for-the-badge"></a>
  <a href="https://github.com/olivenda/zenbite-/issues"><img alt="Good first issues" src="https://img.shields.io/badge/good%20first%20issue-welcome-brightgreen?style=for-the-badge"></a>
</p>

<p align="center">
  <strong>Boots from a 1.44 MiB floppy &middot; runs in 4 MiB of RAM &middot; ships with a real network stack &middot; installs from a wizard &middot; opens a windowed Desktop with Files, Web and Terminal apps.</strong>
</p>

---

```
   ____             _     _ _
  |_  / ___ _ __  | |__ (_) |_ ___    .   ✦    ✧
   / / / _ \ '_ \ | '_ \| | __/ _ \    ✦   .   ✧
  / /_|  __/ | | || |_) | | ||  __/   ✧ ✦   .
 /____\___|_| |_||_.__/|_|\__\___|     .  ✦ ✧

Zenbite v0.3
(c) 2026 Zenbite contributors -- MIT License

CPU   : AuthenticAMD  64-bit capable -- running in 32-bit compatibility mode
Memory: 15998 KiB total, 834 KiB used (kernel), 14524 KiB free
net   : MAC 52:54:00:12:34:56  IP 10.0.2.16

C:\>
```

## Features

| Subsystem      | What you get                                                                       |
| -------------- | ---------------------------------------------------------------------------------- |
| Bootloader     | Custom two-stage MBR + stage2 (FAT12 floppy, FAT16/FAT32 HDD, BPB-aware + LBA)     |
| Kernel         | 32-bit PM, GDT/IDT/PIC/PIT, identity-paged 4 GiB                                   |
| Display        | VGA text mode (80×25, 16 colours), CP437 font                                      |
| Input          | PS/2 keyboard with US + DE (QWERTZ) layouts, PS/2 mouse, USB HID keyboard          |
| Storage        | ATA PIO, AHCI, floppy controller (FDC), FAT12 / FAT16 / FAT32 up to ~4 GiB         |
| Networking     | Intel e1000 + NE2000 drivers, ARP, IPv4, ICMP, UDP, TCP, `wget` and `ping`         |
| Time           | CMOS RTC; `date` and `time` builtins                                               |
| Shell          | DOS-style REPL with `?`, `ver`, `cls`, `dir`, `cd`, `type`, `copy`, `del`, `cc` …  |
| Installer      | MS-DOS-Setup-style wizard: pick disk → format → bootloader → user → samples        |
| Desktop        | Windowed UI with Files browser, Web (real HTTP) and Terminal                        |

## Quick start

```sh
sudo apt install build-essential gcc-multilib nasm qemu-system-x86 mtools dosfstools
make            # builds the boot floppy, setup floppy, install media, blank target
make run        # boots all five disks in QEMU
make test       # headless boot for CI -- scrapes serial for ZENBITE READY
make clean
```

Inside the OS:

```
C:\> desktop          # launches the windowed Desktop (F1=Files, F2=Web, F3=Terminal)
C:\> ping 10.0.2.2    # ICMP echo, 4 packets
C:\> wget http://10.0.2.2/index.html -o INDEX.HTM
C:\> ifconfig         # MAC + IP + gateway
C:\> keymap de        # switch to QWERTZ
C:\> date && time     # real wall clock from CMOS
```

To write to a real floppy / USB stick:

```sh
sudo dd if=zenbite.img of=/dev/sdX bs=512 conv=fsync
```

## Disk layout

```
zenbite.img            1.44 MiB FAT12 boot floppy (stage1+stage2+kernel; self-installable)
zenbite_setup.img      1.44 MiB FAT12 setup floppy (SYSTEM/, BOOT/, SAMPLES/)
zenbite_install1.img   8 MiB FAT16 setup disk 1 (system files)
zenbite_install2.img   8 MiB FAT16 setup disk 2 (samples)
zenbite_target.img     64 MiB blank ATA target (formatted by the installer)
```

The installer creates a home folder for the user it asks you to name:

```
C:\HOME\<USER>\
  DESKTOP\
  DOCUMENTS\
  DOWNLOADS\
  PICTURES\
  PROFILE.TXT     # username, created-at, machine name
```

The primary user has unrestricted access to every drive and folder — Zenbite's permission model is intentionally Linux-root-equivalent for the main account.

## Roadmap

- **v0.1 — It boots and types** ✓ bootloader + 32-bit kernel + VGA + PS/2 + FAT12 + shell
- **v0.2 — It remembers things** ✓ FAT16 write, editor, ZAS assembler, mini C compiler
- **v0.3 — It connects and clicks** ✓ TCP/IP, e1000, AHCI, FAT32 ≤ 4 GiB, mouse, Desktop
- **v0.4** ring-3 userland + syscall ABI; sound (PC speaker / SB16); long filenames

## Contributing

Pull requests welcome from absolutely anyone. Zenbite is MIT — do whatever you want with
it, just keep the credit line. Start with [`CONTRIBUTING.md`](CONTRIBUTING.md). Good first
issues live on the [issue tracker](https://github.com/olivenda/zenbite-/issues) with the
`good-first-issue` label.

## Maintainers

- **Oliver Petz** — project lead and primary maintainer ([@olivenda](https://github.com/olivenda))

Past and present contributors are recorded in [`AUTHORS`](AUTHORS) and in the git history
(`git shortlog -sne`).

## License

[MIT](LICENSE) — © 2026 Oliver Petz and Zenbite contributors.
