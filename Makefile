# Zenbite top-level Makefile.
#
# Builds: boot/stage1.bin, boot/stage2.bin, kernel/kernel.bin, and four images:
#   zenbite.img          - 1.44 MB FAT12 boot floppy (stage1+2+kernel only)
#   zenbite_setup.img    - 1.44 MB FAT12 setup floppy (SYSTEM/ BOOT/ SAMPLES/)
#   zenbite_install1.img - 8 MB FAT16 ATA setup disk 1 (same content as setup floppy)
#   zenbite_install2.img - 8 MB FAT16 ATA setup disk 2 (additional samples)
#   zenbite_target.img   - 64 MB blank ATA target disk
#
# Disk layout:
#   QEMU -fda = boot floppy     (FDC drive A:, disk slot 8)
#   QEMU -fdb = setup floppy    (FDC drive B:, disk slot 9)
#   QEMU -hda = ATA setup disk1 (ATA slot 0, drive A: if floppy absent)
#   QEMU -hdb = ATA setup disk2 (ATA slot 1)
#   QEMU -hdc = blank target    (ATA slot 2)
#
# Single-floppy real hardware: boot from zenbite.img; installer prompts to
# swap to setup floppy (or use F5 to rescan after inserting the data disk).
#
# Requires: nasm, gcc-multilib, mtools (mformat/mcopy/mmd), qemu-system-x86_64.
# The kernel runs in 32-bit protected mode.

# Use the host toolchain by default. Override with CROSS= if you have a
# dedicated i686-elf cross-compiler (the build also works with that).
CROSS    ?=
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy
NASM     ?= nasm
# Use the x86_64 QEMU so the kernel's CPUID-based 64-bit detection actually
# sees the long-mode bit (qemu-system-i386 strips it). The kernel itself is
# 32-bit; either QEMU binary will boot it.
QEMU     ?= qemu-system-x86_64

BUILD    := build
KERNEL   := $(BUILD)/kernel.bin
IMG      := zenbite.img
SETUP    := zenbite_setup.img
INST1    := zenbite_install1.img
INST2    := zenbite_install2.img
TARGET   := zenbite_target.img

CFLAGS   := -std=gnu11 -ffreestanding -nostdlib -m32 \
            -fno-stack-protector -fno-pic -fno-pie -mno-red-zone \
            -fno-builtin -fno-omit-frame-pointer \
            -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
            -Wall -Wextra -Wno-unused-parameter -O2 \
            -MMD -MP -Iinclude

LDFLAGS  := -m elf_i386 -nostdlib -T kernel/kernel.ld

NASMFLAGS_BOOT := -f bin
NASMFLAGS_OBJ  := -f elf32

# Source globs.
KERNEL_C_SRC := $(wildcard kernel/*.c) \
                $(wildcard kernel/mm/*.c) \
                $(wildcard kernel/fs/*.c) \
                $(wildcard kernel/drv/*.c) \
                $(wildcard kernel/net/*.c) \
                $(wildcard shell/*.c) \
                $(wildcard libk/*.c)

# New driver directories picked up automatically by the wildcards above.

KERNEL_S_SRC := kernel/entry.asm kernel/isr.asm kernel/boot_blobs.asm

KERNEL_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_C_SRC)) \
              $(patsubst %.asm,$(BUILD)/%.o,$(KERNEL_S_SRC))

.PHONY: all run test clean dirs

all: $(IMG) $(SETUP)

dirs:
	@mkdir -p $(BUILD) $(BUILD)/boot $(BUILD)/kernel $(BUILD)/kernel/mm \
	          $(BUILD)/kernel/fs $(BUILD)/kernel/drv $(BUILD)/kernel/net \
	          $(BUILD)/shell $(BUILD)/libk

# --- Stage 1 / Stage 2 -----------------------------------------------------
$(BUILD)/boot/stage1.bin: boot/stage1.asm | dirs
	$(NASM) $(NASMFLAGS_BOOT) $< -o $@

$(BUILD)/boot/stage2.bin: boot/stage2.asm | dirs
	$(NASM) $(NASMFLAGS_BOOT) $< -o $@

# HDD/FAT16-aware variants used by the installer for ATA/AHCI targets.
$(BUILD)/boot/stage1_hdd.bin: boot/stage1_hdd.asm | dirs
	$(NASM) $(NASMFLAGS_BOOT) $< -o $@

$(BUILD)/boot/stage2_hdd.bin: boot/stage2_hdd.asm | dirs
	$(NASM) $(NASMFLAGS_BOOT) $< -o $@

# Chainloader MBR for USB / partitioned-HDD images.
$(BUILD)/boot/mbr.bin: boot/mbr.asm | dirs
	$(NASM) $(NASMFLAGS_BOOT) $< -o $@

# --- Kernel ----------------------------------------------------------------
$(BUILD)/%.o: %.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm | dirs
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS_OBJ) $< -o $@

# boot_blobs.asm uses incbin to embed the stage binaries -- declare the dep
# so it rebuilds when stage1/stage2 change.
$(BUILD)/kernel/boot_blobs.o: $(BUILD)/boot/stage1.bin $(BUILD)/boot/stage2.bin \
                              $(BUILD)/boot/stage1_hdd.bin $(BUILD)/boot/stage2_hdd.bin \
                              $(BUILD)/boot/mbr.bin

$(KERNEL): $(KERNEL_OBJ) kernel/kernel.ld
	$(LD) $(LDFLAGS) -o $(BUILD)/kernel.elf $(KERNEL_OBJ)
	$(OBJCOPY) -O binary $(BUILD)/kernel.elf $@

# --- Distribution images ---------------------------------------------------
$(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET): $(BUILD)/boot/stage1.bin \
                                              $(BUILD)/boot/stage2.bin \
                                              $(BUILD)/boot/stage1_hdd.bin \
                                              $(BUILD)/boot/mbr.bin $(KERNEL) \
                                              scripts/mkdisk.sh
	scripts/mkdisk.sh $(BUILD) $(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET)

# --- Runtime ---------------------------------------------------------------
# Full run: boot floppy + setup floppy + ATA disks + blank target.
# On first boot the kernel auto-launches the installer.
# To wipe and start over: rm zenbite_target.img && make
run: $(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET)
	$(QEMU) -fda $(IMG) -fdb $(SETUP) \
	        -hda $(INST1) -hdb $(INST2) -hdc $(TARGET) -boot a \
	        -serial stdio -no-reboot -no-shutdown

# Minimal run: just boot floppy + setup floppy + target.
# Skips the redundant ATA install disks.
run-min: $(IMG) $(SETUP) $(TARGET)
	$(QEMU) -fda $(IMG) -fdb $(SETUP) -hda $(TARGET) -boot a \
	        -serial stdio -no-reboot -no-shutdown

# Boot an already-installed system (no setup disks needed).
run-installed: $(IMG) $(TARGET)
	$(QEMU) -fda $(IMG) -hda $(TARGET) -boot a \
	        -serial stdio -no-reboot -no-shutdown

# Single-CD install: boot floppy + one combined install disk + target.
# The installer copies SYSTEM/BOOT/SAMPLES from the single CD.
run-cd: all
	$(QEMU) -fda $(IMG) -hdb zenbite_install_cd.img -hda $(TARGET) -boot a \
	        -serial stdio -no-reboot -no-shutdown

# Headless boot used by CI.
test: $(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET)
	scripts/qemu-run.sh $(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET)

clean:
	rm -rf $(BUILD) $(IMG) $(SETUP) $(INST1) $(INST2) $(TARGET) \
	       zenbite_install_cd.img zenbite_data.img zenbite_blank.img

# Pull in auto-generated header dependencies (-MMD) so editing a header
# rebuilds every .o that includes it. Without this, changing kernel.h
# (e.g. ZENBITE_VERSION) left stale object files -- the shell printed an
# old version while the kernel banner showed the new one.
-include $(KERNEL_OBJ:.o=.d)
