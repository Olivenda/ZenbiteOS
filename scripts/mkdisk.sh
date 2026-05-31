#!/usr/bin/env bash
# Builds the Zenbite distribution disks:
#
#   zenbite.img          -- 1.44 MiB FAT12 boot floppy  (stage1+2+kernel, minimal)
#   zenbite_setup.img    -- 1.44 MiB FAT12 setup floppy (SYSTEM/ BOOT/ SAMPLES/)
#                          Connect as -fdb in QEMU.  On real hw: swap after boot.
#   zenbite_install1.img -- 8 MiB FAT16 ATA "Setup Disk 1" (same content, larger)
#   zenbite_install2.img -- 8 MiB FAT16 ATA "Setup Disk 2" (extra samples)
#   zenbite_target.img   -- 16 MiB blank ATA target disk
set -euo pipefail

BUILD="${1:-build}"
IMG="${2:-zenbite.img}"
SETUP="${3:-zenbite_setup.img}"
INST1="${4:-zenbite_install1.img}"
INST2="${5:-zenbite_install2.img}"
TARGET="${6:-zenbite_target.img}"

STAGE1="$BUILD/boot/stage1.bin"
STAGE2="$BUILD/boot/stage2.bin"
KERNEL="$BUILD/kernel.bin"

for f in "$STAGE1" "$STAGE2" "$KERNEL"; do
    [[ -f "$f" ]] || { echo "missing $f" >&2; exit 1; }
done

# --- 1) boot floppy -------------------------------------------------------
dd if=/dev/zero of="$IMG" bs=512 count=2880 status=none
mformat -i "$IMG" -f 1440 ::
python3 - "$IMG" "$STAGE1" <<'PY'
import sys
img, stage1 = sys.argv[1], sys.argv[2]
with open(img, "r+b") as f:
    bpb = f.read(512)[3:62]
    f.seek(0)
    s1 = open(stage1, "rb").read()
    assert len(s1) == 512
    sector = bytearray(s1)
    sector[3:62] = bpb
    sector[510:512] = b"\x55\xaa"
    f.write(sector)
PY
mcopy -i "$IMG" "$STAGE2" ::/STAGE2.BIN
mcopy -i "$IMG" "$KERNEL" ::/KERNEL.BIN
# Single-floppy installs: the boot floppy doubles as setup disk 1 so the
# user can install without ever swapping disks. Adds ~90 KiB to the image.
mmd -i "$IMG" ::/SYSTEM
mcopy -i "$IMG" "$KERNEL" ::/SYSTEM/KERNEL.BIN
mcopy -i "$IMG" "$STAGE2" ::/SYSTEM/STAGE2.BIN
printf 'Zenbite v0.2\n' | mcopy -i "$IMG" - ::/SYSTEM/ZENBITE.SYS
mmd -i "$IMG" ::/BOOT
mcopy -i "$IMG" "$STAGE1" ::/BOOT/STAGE1.BIN
mcopy -i "$IMG" "$STAGE2" ::/BOOT/STAGE2.BIN
printf '1' | mcopy -i "$IMG" - ::/INSTALL.TAG
echo "built $IMG (bootable, self-installable)"

# --- 2) setup floppy: 1.44 MiB FAT12, same content as install disk 1 -----
# This is the "swap-in" floppy for single-drive real-hardware installs.
# It must fit in 1.44 MiB; KERNEL.BIN dominates — if it ever exceeds ~1.3 MiB
# the floppy target below should be increased (use 2.88 MiB or an HDD image).
SETUP_KB=$(( $(stat -c%s "$KERNEL") / 1024 + 64 ))
if (( SETUP_KB > 1300 )); then
    echo "WARNING: kernel is ${SETUP_KB} KiB; setup floppy may be too small." >&2
fi
dd if=/dev/zero of="$SETUP" bs=512 count=2880 status=none
mformat -i "$SETUP" -f 1440 ::
STMP="$(mktemp -d)"
mkdir -p "$STMP/SYSTEM" "$STMP/BOOT" "$STMP/BIN" "$STMP/SAMPLES"
cp "$KERNEL" "$STMP/SYSTEM/KERNEL.BIN"
cp "$STAGE2" "$STMP/SYSTEM/STAGE2.BIN"
printf 'Zenbite v0.2\n' > "$STMP/SYSTEM/ZENBITE.SYS"
cp "$STAGE1" "$STMP/BOOT/STAGE1.BIN"
cp "$STAGE2" "$STMP/BOOT/STAGE2.BIN"
printf '1' > "$STMP/INSTALL.TAG"
cat > "$STMP/SAMPLES/HELLO.C" <<'C'
int main() { puts("Hello, Zenbite!"); return 0; }
C
mmd -i "$SETUP" ::/SYSTEM
for f in "$STMP"/SYSTEM/*; do mcopy -i "$SETUP" "$f" "::/SYSTEM/$(basename "$f")"; done
mmd -i "$SETUP" ::/BOOT
for f in "$STMP"/BOOT/*; do mcopy -i "$SETUP" "$f" "::/BOOT/$(basename "$f")"; done
mmd -i "$SETUP" ::/SAMPLES
for f in "$STMP"/SAMPLES/*; do mcopy -i "$SETUP" "$f" "::/SAMPLES/$(basename "$f")"; done
mmd -i "$SETUP" ::/BIN
mcopy -i "$SETUP" "$STMP/INSTALL.TAG" ::/INSTALL.TAG
rm -rf "$STMP"
echo "built $SETUP (1.44 MiB setup floppy)"

# --- 4) install disk 1: system files (ATA, 8 MiB FAT16 version) ----------
dd if=/dev/zero of="$INST1" bs=1M count=8 status=none
mkfs.fat -F 16 -s 2 -n "ZBE-INST1" "$INST1" >/dev/null
SYS1="$(mktemp -d)"
mkdir -p "$SYS1/SYSTEM"
mkdir -p "$SYS1/BOOT"
mkdir -p "$SYS1/BIN"
cp "$KERNEL" "$SYS1/SYSTEM/KERNEL.BIN"
cp "$STAGE2" "$SYS1/SYSTEM/STAGE2.BIN"
cp "$STAGE1" "$SYS1/BOOT/STAGE1.BIN"
cp "$STAGE2" "$SYS1/BOOT/STAGE2.BIN"
printf "Zenbite v0.2\n" > "$SYS1/SYSTEM/ZENBITE.SYS"
printf "1" > "$SYS1/INSTALL.TAG"
cat > "$SYS1/SYSTEM/README.TXT" <<'TXT'
Zenbite v0.2 System Files

KERNEL.BIN    - Main kernel binary
STAGE2.BIN    - Second-stage bootloader
ZENBITE.SYS   - System version marker

To reinstall bootloader on a drive: see BOOT\STAGE1.BIN
TXT
cat > "$SYS1/README.TXT" <<'TXT'
Zenbite Setup Disk 1.

This disk holds the system files (kernel, loader, version tag) under
\SYSTEM\. During setup they are copied to your target drive.

Do not modify files on this disk.
TXT
mmd -i "$INST1" ::/SYSTEM
for f in "$SYS1"/SYSTEM/*; do mcopy -i "$INST1" "$f" "::/SYSTEM/$(basename "$f")"; done
mmd -i "$INST1" ::/BOOT
for f in "$SYS1"/BOOT/*; do mcopy -i "$INST1" "$f" "::/BOOT/$(basename "$f")"; done
mmd -i "$INST1" ::/BIN
mcopy -i "$INST1" "$SYS1/INSTALL.TAG" ::/INSTALL.TAG
mcopy -i "$INST1" "$SYS1/README.TXT"  ::/README.TXT
rm -rf "$SYS1"
echo "built $INST1 (Setup Disk 1)"

# --- 5) install disk 2: samples ------------------------------------------
dd if=/dev/zero of="$INST2" bs=1M count=8 status=none
mkfs.fat -F 16 -s 2 -n "ZBE-INST2" "$INST2" >/dev/null
SYS2="$(mktemp -d)"
mkdir -p "$SYS2/SAMPLES"
cat > "$SYS2/SAMPLES/HELLO.C" <<'C'
// Hello world in Zenbite C.
int main() {
    puts("Hello, Zenbite!");
    return 0;
}
C
cat > "$SYS2/SAMPLES/FIB.C" <<'C'
// First 12 Fibonacci numbers.
int main() {
    int a = 0; int b = 1; int i = 0;
    while (i < 12) {
        print(a);
        int t = a + b; a = b; b = t;
        i = i + 1;
    }
    return 0;
}
C
cat > "$SYS2/SAMPLES/FIZZBUZZ.C" <<'C'
// Classic FizzBuzz.
int main() {
    int i = 1;
    while (i <= 30) {
        if (i % 15 == 0)     puts("FizzBuzz");
        else if (i % 3 == 0) puts("Fizz");
        else if (i % 5 == 0) puts("Buzz");
        else                 print(i);
        i = i + 1;
    }
    return 0;
}
C
cat > "$SYS2/SAMPLES/INDEX.TXT" <<'TXT'
Zenbite sample programs:
  HELLO.C      hello world
  FIB.C        Fibonacci numbers
  FIZZBUZZ.C   classic warmup

Try:   cc HELLO.C
TXT
printf "2" > "$SYS2/INSTALL.TAG"
mmd -i "$INST2" ::/SAMPLES
for f in "$SYS2"/SAMPLES/*; do mcopy -i "$INST2" "$f" "::/SAMPLES/$(basename "$f")"; done
mcopy -i "$INST2" "$SYS2/INSTALL.TAG" ::/INSTALL.TAG
rm -rf "$SYS2"
echo "built $INST2 (Setup Disk 2)"

# --- 6) combined install "CD": everything on one FAT16 disk -----------
# A single source disk carrying SYSTEM/, BOOT/ and SAMPLES/ together --
# the "one CD" scenario. The installer's find_source_dir() locates each
# directory wherever it lives, so this disk alone is enough to install.
CD="zenbite_install_cd.img"
dd if=/dev/zero of="$CD" bs=1M count=16 status=none
mkfs.fat -F 16 -s 2 -n "ZENBITE-CD" "$CD" >/dev/null
CDT="$(mktemp -d)"
mkdir -p "$CDT/SYSTEM" "$CDT/BOOT" "$CDT/SAMPLES"
cp "$KERNEL" "$CDT/SYSTEM/KERNEL.BIN"
cp "$STAGE2" "$CDT/SYSTEM/STAGE2.BIN"
printf 'Zenbite v0.3\n' > "$CDT/SYSTEM/ZENBITE.SYS"
cp "$STAGE1" "$CDT/BOOT/STAGE1.BIN"
cp "$STAGE2" "$CDT/BOOT/STAGE2.BIN"
cat > "$CDT/SAMPLES/HELLO.C" <<'C'
int main() { puts("Hello, Zenbite!"); return 0; }
C
printf '1' > "$CDT/INSTALL.TAG"
mmd -i "$CD" ::/SYSTEM
for f in "$CDT"/SYSTEM/*;  do mcopy -i "$CD" "$f" "::/SYSTEM/$(basename "$f")";  done
mmd -i "$CD" ::/BOOT
for f in "$CDT"/BOOT/*;    do mcopy -i "$CD" "$f" "::/BOOT/$(basename "$f")";    done
mmd -i "$CD" ::/SAMPLES
for f in "$CDT"/SAMPLES/*; do mcopy -i "$CD" "$f" "::/SAMPLES/$(basename "$f")"; done
mcopy -i "$CD" "$CDT/INSTALL.TAG" ::/INSTALL.TAG
rm -rf "$CDT"
echo "built $CD (combined install CD, 16 MiB)"

# --- 7) USB image: partitioned, MBR-bootable --------------------------
# Modern BIOSes refuse to boot a USB stick that lacks an MBR partition
# table. We build a 32 MiB image with:
#   sector 0          : Zenbite chainloader MBR + partition table
#   sectors 2048..N   : one active FAT16 partition holding the install CD
# The chainloader reads the partition's first sector (a normal Zenbite
# FAT16 boot sector, stage1_hdd) and jumps to it.
USB="zenbite_usb.img"
MBR="$BUILD/boot/mbr.bin"
STAGE1_HDD="$BUILD/boot/stage1_hdd.bin"
STAGE2_HDD="$BUILD/boot/stage2_hdd.bin"
[[ -f "$MBR" ]]        || { echo "missing $MBR" >&2; exit 1; }
[[ -f "$STAGE1_HDD" ]] || { echo "missing $STAGE1_HDD" >&2; exit 1; }
[[ -f "$STAGE2_HDD" ]] || { echo "missing $STAGE2_HDD" >&2; exit 1; }

USB_SECTORS=65536              # 32 MiB
PART_START=2048                # 1 MiB alignment
PART_SECTORS=$((USB_SECTORS - PART_START))

# 1. Empty 32 MiB image.
dd if=/dev/zero of="$USB" bs=512 count=$USB_SECTORS status=none

# 2. Create the FAT16 partition image at the correct offset.
PART_IMG="$(mktemp)"
dd if=/dev/zero of="$PART_IMG" bs=512 count=$PART_SECTORS status=none
mkfs.fat -F 16 -s 4 -n "ZENBITE-USB" "$PART_IMG" >/dev/null

# 3. Populate the partition with the install content.
UTMP="$(mktemp -d)"
mkdir -p "$UTMP/SYSTEM" "$UTMP/BOOT" "$UTMP/SAMPLES"
cp "$KERNEL"  "$UTMP/SYSTEM/KERNEL.BIN"
cp "$STAGE2"  "$UTMP/SYSTEM/STAGE2.BIN"
printf 'Zenbite v0.3\n' > "$UTMP/SYSTEM/ZENBITE.SYS"
cp "$STAGE1_HDD" "$UTMP/BOOT/STAGE1.BIN"
cp "$STAGE2"     "$UTMP/BOOT/STAGE2.BIN"
cat > "$UTMP/SAMPLES/HELLO.C" <<'C'
int main() { puts("Hello, Zenbite!"); return 0; }
C
printf '1' > "$UTMP/INSTALL.TAG"
mmd -i "$PART_IMG" ::/SYSTEM
for f in "$UTMP"/SYSTEM/*;  do mcopy -i "$PART_IMG" "$f" "::/SYSTEM/$(basename "$f")";  done
mmd -i "$PART_IMG" ::/BOOT
for f in "$UTMP"/BOOT/*;    do mcopy -i "$PART_IMG" "$f" "::/BOOT/$(basename "$f")";    done
mmd -i "$PART_IMG" ::/SAMPLES
for f in "$UTMP"/SAMPLES/*; do mcopy -i "$PART_IMG" "$f" "::/SAMPLES/$(basename "$f")"; done
mcopy -i "$PART_IMG" "$UTMP/INSTALL.TAG" ::/INSTALL.TAG
# stage1_hdd loads "STAGE2  BIN" from the root directory; stage2_hdd
# then loads "KERNEL  BIN" the same way. Use the HDD/FAT16 variants
# (floppy stage2 only understands FAT12).
mcopy -i "$PART_IMG" "$STAGE2_HDD" ::/STAGE2.BIN
mcopy -i "$PART_IMG" "$KERNEL" ::/KERNEL.BIN
rm -rf "$UTMP"

# 4. Overwrite the partition's first sector with stage1_hdd while
#    preserving the FAT16 BPB that mkfs.fat just wrote. Patch the
#    BPB's hidden_sectors (offset 0x1C, 4 bytes LE) to the partition's
#    start LBA so stage1_hdd/stage2_hdd compute absolute disk LBAs
#    instead of partition-relative ones.
python3 - "$PART_IMG" "$STAGE1_HDD" "$PART_START" <<'PY'
import struct, sys
img, stage1, part_start = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(img, "r+b") as f:
    bpb = bytearray(f.read(512)[3:62])
    # hidden_sectors lives at boot-sector offset 0x1C, i.e. bpb index
    # 0x1C - 3 = 0x19 .. 0x1C.
    bpb[0x19:0x1D] = struct.pack("<I", part_start)
    s1 = open(stage1, "rb").read()
    assert len(s1) == 512, len(s1)
    sector = bytearray(s1)
    sector[3:62]    = bytes(bpb)
    sector[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(sector)
PY

# 5. Splice partition image into the USB image at PART_START.
dd if="$PART_IMG" of="$USB" bs=512 seek=$PART_START count=$PART_SECTORS \
    conv=notrunc status=none
rm -f "$PART_IMG"

# 6. Write the MBR boot code (bytes 0..445) into sector 0 of the USB.
#    Then build the partition table by hand at offset 0x1BE.
dd if="$MBR" of="$USB" bs=1 count=446 conv=notrunc status=none

python3 - "$USB" "$PART_START" "$PART_SECTORS" <<'PY'
import struct, sys
img, start, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
# Partition table entry, 16 bytes:
#   0x00  boot flag (0x80 = active)
#   0x01  start CHS  (head, sector/cyl_hi, cyl_lo) -- legacy, we use FE FF FF
#   0x04  type (0x0E = FAT16 LBA)
#   0x05  end CHS    (legacy) -- also FE FF FF
#   0x08  start LBA (LE u32)
#   0x0C  sector count (LE u32)
entry = bytearray(16)
entry[0]   = 0x80
entry[1:4] = b"\xFE\xFF\xFF"
entry[4]   = 0x0E
entry[5:8] = b"\xFE\xFF\xFF"
entry[8:12]  = struct.pack("<I", start)
entry[12:16] = struct.pack("<I", count)
with open(img, "r+b") as f:
    f.seek(0x1BE)
    f.write(entry)
    # Three empty partition slots.
    f.write(b"\x00" * 48)
    # Boot signature.
    f.seek(0x1FE)
    f.write(b"\x55\xAA")
PY

echo "built $USB (USB-bootable, MBR + FAT16 partition, 32 MiB)"

# --- 8) target: blank --------------------------------------------------
dd if=/dev/zero of="$TARGET" bs=1M count=64 status=none
echo "built $TARGET (blank target, 64 MiB)"
