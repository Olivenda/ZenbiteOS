# Zenbite — notes for future Claude sessions

This file captures hard-won lessons from building Zenbite (a 32-bit x86
retro OS) so the next session doesn't repeat the same investigations.
Don't restate things you can read from the code; only record what bit
us, where it hurt, and the fix.

## Project shape

* Branch: `claude/retro-os-design-plan-kWoZ6` — develop here, push when
  done. Never push to `main`.
* Build: `make` (gcc i686 freestanding + NASM). Output disk images live
  at repo root: `zenbite.img` (floppy), `zenbite_usb.img` (MBR + FAT16
  partition, USB-bootable), `zenbite_target.img` (blank ATA target),
  `zenbite_install_cd.img` (FAT16 install CD).
* Boot test: `qemu-system-i386 -m 32 -drive file=zenbite.img,if=floppy,format=raw -boot a -display none -serial file:/tmp/log`.
* `make` re-runs `scripts/mkdisk.sh`; tree must be clean of build
  artifacts before commit.

## Quick-jump map

| Area               | File                                  |
|--------------------|---------------------------------------|
| Stage1 / Stage2    | `boot/stage1*.asm`, `boot/stage2*.asm`, `boot/mbr.asm` |
| Kernel entry       | `kernel/entry.asm` -> `kernel/kmain.c`|
| VGA text + shadow  | `kernel/vga.c`                        |
| Keyboard (PS/2)    | `kernel/keyboard.c`                   |
| Mouse (PS/2 + USB) | `kernel/drv/mouse.c`, `kernel/drv/usb.c` |
| ATA / disks        | `kernel/drv/ata.c`, `kernel/drv/disk.c` |
| MBR partitions     | `kernel/drv/mbr.c`, `include/mbr.h`   |
| FAT12/16/32        | `kernel/fs/fat.c`                     |
| Net + DNS + HTTP   | `kernel/net/net.c`                    |
| Shell (REPL)       | `shell/shell.c`, `shell/builtins.c`   |
| Desktop / WM       | `shell/desktop.c`                     |
| Editor / Installer | `shell/edit.c`, `shell/install.c`     |
| Mini-C / Asm       | `shell/zbc.c`, `shell/zas.c`          |

## Bugs that took multiple commits to fix

### "Text flickers on/off in steady rhythm" -- VGA hardware blink
BIOS leaves the Attribute Controller in **blink mode**: attribute
bit 7 means "foreground blinks at ~2 Hz" instead of "bright BG".
Every window background in this OS is bright (white=15 etc.), so
the VGA card was literally toggling cells on/off twice a second.
**Fix in `vga_init()`:** toggle Mode Control bit 3 via 0x3C0/0x3C1.
Don't bother chasing flicker in the renderer until you're sure
blink mode is off.

### "Cursor leaves a magenta trail"
`show_menu` painted the cursor every frame via `cursor_draw()` but
never called `cursor_restore()` between iterations -- every
position became a permanent block. Pattern: track `prev_mc/prev_mr`,
restore-then-draw on motion. Same trap in any custom polling loop.

### "Window teleports on drag release"
The drag branch in `desktop_main` did `continue` BEFORE the
end-of-frame `vga_present()`, so position only flushed to VGA on
release. **Always `vga_present()` inside the drag loop**, before
the `continue`.

### "Drag doesn't release on USB-mouse machines"
`mouse_get()` only updated `ms_buttons` from USB when the reported
value was nonzero -- release (ubtn=0) never cleared. Fix: when
`usb_mouse_present()`, the USB report is authoritative for both
press AND release.

### "Desktop exits when the mouse moves"
`kb_trygetc()` polled the i8042 status port and called `on_irq1()`
for any byte present. **Mouse Y-delta of 0x01 == ESC scancode** ->
desktop ESC handler fires. Fix: only drain a keyboard byte (bit 0
set, bit 5 CLEAR). Bit 5 set means mouse byte.

### "HDD boot hangs at 'Zenbite/HDD ok'"
Stage2 code past 0x200 was garbage because `read_sectors_lba`
looped on CX, but `INT 13h` clobbered CX. Only 1 sector loaded per
call instead of N. **Always `push eax/ecx/bx/si` around INT 13h**
in real-mode disk readers (stage1_hdd.asm + stage2_hdd.asm).

### "USB stick doesn't boot ('no operating system')"
The old `zenbite_install_cd.img` was a superfloppy (FAT16 BPB at
sector 0, no partition table). Modern BIOSes refuse to boot a USB
without an MBR partition table. Fix: build a real partitioned
image (`zenbite_usb.img`): chainloader MBR + active FAT16 partition
at LBA 2048 + stage1_hdd as the partition's boot sector. The MBR
must keep DL in a register across the self-relocation (see
`boot/mbr.asm`); writing it to `[drive]` before the copy gets
overwritten by the placeholder byte in the source.

### "Stage1_hdd reads wrong sectors on partitioned disks"
Stage1 / Stage2 computed LBAs relative to the partition (BPB-
relative) but `INT 13h AH=42h` wants disk-absolute LBAs. **Always
add `[BPB_HIDDEN_SECTORS]`** (offset 0x1C, 4 bytes) before issuing
a disk read. Zero on superfloppies, so this is harmless there.

### "Installed MBR-partition disk failed standalone boot"
The kernel's FAT formatter wrote `hidden_sectors = 0` on every
volume, including installs onto a partition view-disk. Stage1_hdd
then computed absolute LBAs from 0 instead of the partition's start
LBA -- the first `disk_read(STAGE2.BIN)` landed on the MBR and the
chain died with "ZB: no boot sig". **Fix:** in `fat.c`'s formatters,
look up the partition view's `offset_lba` via `mbr_view_info()` and
stamp it into `boot[28..31]`. Verified end-to-end with a 64 MiB
target installed via the partitioned wizard.

### "Kernel parsed a BPB out of an MBR sector"
After adding MBR view-disks, `do_mount_all` still walked every
present slot 0..N. On a partitioned raw disk it'd try `fs_mount`
on the raw disk, parse sector 0 (the MBR) as a FAT BPB, and print
the noisy "disk X present but unformatted" line. **Fix:** in
`vfs.c::do_mount_all`, skip raw disks that have a valid MBR
(`mbr_has_table`) -- the partition view-disk handles the real mount
a few iterations later.

### "Can't read drives over 4 GB / actually 128 GB"
The ATA driver was LBA28 only. Any sector past 2^28 silently
wrapped. Fix: check `ident[83]` bit 10 for LBA48; if set, read the
48-bit sector count from `ident[100..103]` and issue READ/WRITE
SECTORS EXT (0x24 / 0x34) plus CACHE FLUSH EXT (0xEA) with the
two-pass register fill (high bytes then low). `ata: <name> LBA48`
in dmesg confirms.

### "Differential present caused flicker on VBox/VMware/real CRT"
Two problems compound: writing to VGA memory during display scan
tears, and a full 2000-cell tight-loop copy gets caught mid-flight
by hypervisor polling. Fix has 3 layers:
  1. **Shadow buffer**: every draw goes to `shadow_buf`; only
     `vga_present()` touches `VGA_BUF` (0xB8000).
  2. **Differential write**: compare each cell to `prev_buf`; write
     only the changed ones. Idle frames write zero cells.
  3. **Vblank sync**: poll `inb(0x3DA) & 0x08` to start writes
     during retrace.
And of course: **disable blink mode** (see top of this section)
before chasing any flicker.

### "USB GET_DESCRIPTOR(8) times out at boot"
UHCI's frame list is 1024 entries x 1 ms each. The original
ctrl_transfer hooked the control QH into `frame[0]` only, so the
HC processed it at most once per ~1024 ms -- and the 500k-iteration
`td_wait` timed out long before then. Symptoms: every USB control
transfer to a newly-reset device fails. (HID kbd/mouse happened to
work because their QHs already lived in every frame slot once
submitted.) **Fix:** while a control or bulk transfer is in flight,
hook the QH into ALL 1024 frame slots; restore them to
`LP_TERMINATE` on completion. The bulk pump in `usb.c` does the
same.

### "FAT16 root-dir read fails on a USB stick"
`fs_mount` reads `root_size_sec` (32 sectors = 16 KiB) in one
`disk_read` call. With UHCI `maxp=64` that's 256 bulk packets =
256 TDs in one `bulk_transfer`, but our static pool is
`BULK_MAX_TDS = 64`. Symptoms: stick enumerates
("usb-msc: usb0 ..."), mount fails
("disk usb0 present but unformatted"). **Fix:** cap each SCSI
READ/WRITE (10) at `MSC_IO_CHUNK_SECTORS = 4` in
`msc_disk_read`/`write` and loop at the disk layer.

### "USB MSC disk registered but doesn't mount"
`usb_init()` runs at kmain ordering #4, before `fs_init()` --
which begins with `disk_init()` wiping all 14 slots. USB MSC's
registration during enumeration gets erased before `do_mount_all`
ever sees it. **Fix:** keep enumeration in `usb_init`, but defer
disk-table registration to a separate `usb_msc_register_disks()`
called from `fs_init` after ATA/AHCI/floppy have populated slots
0..9.

### "Mem shows the same in shell and desktop"
The kheap usage wasn't tracked. Added `kheap_used_kib()` /
`kheap_total_kib()` in `kernel/mm/kheap.c`; both shell `mem` and
the System Monitor widget read those. If a number looks suspicious
identical between two callers, suspect a stale static cache before
suspecting the readers.

### "Inverted-block cursor erased text underneath"
0xDB full-block cursor replaces the cell glyph completely. When
the PS/2 mouse jittered by a cell, the underlying character
flashed in and out -- which reads as text flicker. Fix: cursor
keeps the underlying glyph and just swaps fg/bg attributes. The
character stays visible.

## Architecture invariants

* **VGA text mode 80x25**, 0xB8000, 16-bit cells. Don't reach for
  graphics modes -- the entire WM assumes text mode.
* **Shadow buffer is THE source of truth** during `tui_init()` /
  desktop. Direct `kputs()` writes still go to VGA via the redirect
  path (used by the Terminal widget for `vga_redirect`).
* **`vga_present()` is the only place VGA memory is touched** in
  shadow mode. Never inline VGA writes; they'll race the diff.
* **PS/2 mouse + USB mouse share the same cursor state** via
  `mouse_get()`. Don't add a third input source without consolidating
  there.
* **Partition view-disks live in slots 16..31** (DISK_RAW_MAX..DISK_MAX).
  Read/write delegate to the parent raw disk + a fixed LBA offset, so
  the FS layer doesn't need to know about partitions at all -- sector
  0 of a view-disk IS the FAT BPB. Format a partition? Format the
  view-disk. Mount a partition? Mount the view-disk. The MBR scanner
  re-runs in `fs_rescan()`, so editing the table from `parts/mkpart`
  or the installer keeps the view-disks in sync.
* **`tui_init()` / `tui_end()`** manage shadow-mode enable + the
  hardware cursor visibility. Modal full-screen apps (Editor) call
  `tui_end()` to drop back to direct VGA writes, then `tui_init()`
  on return.
* **Widgets live in `widgets[8]`** with a z-order via `next_z`.
  Slot 0 is reserved for Welcome. Singleton kinds raise the existing
  one in `spawn_widget()`; multi-instance ones stack.
* **The desktop main loop is HLT-idle** -- waits for an IRQ (timer
  ~100 Hz, mouse, keyboard) rather than busy-spinning. Don't add
  busy loops; they wreck the idle CPU usage and the diff-present
  timing.

## Things that look like bugs but aren't

* **Cursor disappears for one frame** when a live widget re-renders
  over it. It comes back next frame on mouse motion. Acceptable;
  fixing it requires drawing cursor LAST in every render path,
  which is more invasive than the value provides.
* **Setup wizard launches on every boot** if `install_system_installed()`
  returns 0. This is intentional -- it checks for a drive with
  `SYSTEM\ZENBITE.SYS` and no `INSTALL.TAG`. Setup disks intentionally
  carry both files so they don't self-recognize as "installed".
* **`make` rebuilds the disk images even on no source change.**
  That's just `mkdisk.sh` being unconditional. Don't try to make it
  smart -- the boot blobs depend on `$(KERNEL)` which depends on
  every kernel object.

## Workflow notes

* When debugging the desktop, **temporarily patch `kmain.c`** to call
  `desktop_main(0, NULL)` before the install check, so QEMU drops
  straight into the desktop. Revert before committing -- and double-
  check `git diff` for the hack.
* Use the QEMU monitor over TCP (`-monitor tcp:127.0.0.1:PORT,server,nowait`)
  to dump VGA text memory: `x/2000b 0xB8000`. The first byte of each
  pair is the glyph, the second is the attribute. Quick smoke-test
  for "did the desktop render?" without a graphical display.
* `git push -u origin claude/retro-os-design-plan-kWoZ6` is the
  canonical push. Retry up to 4 times with 2/4/8/16s backoff on
  network errors. Never `--force` to a shared branch.
* Don't create a PR unless the user explicitly asks.

## Scope decisions still standing

* **EXT2/EXT4 read support** is genuinely several hundred lines and
  hasn't been done -- the FAT32 path + LBA48 covers most USB sticks
  and partitions. If the user asks again, do EXT2 first (read-only),
  EXT4 extents second.
* **Real cooperative WM for Editor / Web / Terminal**: Editor uses
  tui_end()-style fullscreen text mode and can't be windowed cleanly
  without rewriting its rendering. Web and Terminal are now widgets
  but their long-blocking operations (`http_get`, `shell_run_line`)
  still block the whole desktop while they run.
* **USB Mass Storage**: BBB/SCSI pipeline is live (`kernel/drv/usb.c`,
  slots 10..13). Tested in QEMU with `-device piix3-usb-uhci -device
  usb-storage,...`. Known limits: one USB device per controller
  (everything uses USB address 1), and 4-sector chunks at the SCSI
  layer. EHCI / multiple sticks / a real BBB scheduler are still
  open.
