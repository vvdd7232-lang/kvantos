# ============================================================
#  KvantOS - build system
# ============================================================
NAME     := kvantos
KERNEL   := build/kvant.bin
ISO      := build/kvantos.iso

CC       := gcc
AS       := nasm
LD       := ld

# GCC freestanding headers (stdint.h, stddef.h, stdarg.h) needed with -nostdinc
GCC_INC  := $(shell $(CC) -m32 -print-file-name=include)

CFLAGS   := -m32 -march=i586 -mtune=generic -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-pic -fno-pie -nostdlib -nostdinc -Wall -Wextra -O2 \
            -Iinclude -isystem $(GCC_INC) -Wno-unused-parameter
ASFLAGS  := -f elf32
LDFLAGS  := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

# .kapp applications are embedded INSIDE the boot image: that way they
# reach the system on any machine, even with no disk and no network -
# burning the ISO is enough. The kernel picks them up as Multiboot
# modules. They are taken from release/apps.
APPS       := $(wildcard release/apps/*.kapp)
APP_GRAFT  := $(foreach a,$(APPS),"boot/apps/$(notdir $(a))=$(a)")
FD_MODULES := $(foreach a,$(APPS),module /boot/apps/$(notdir $(a)) $(notdir $(a)) ;)

C_SRC    := $(wildcard kernel/*.c)
ASM_SRC  := $(wildcard boot/*.asm)
OBJ      := $(patsubst kernel/%.c,build/obj/%.o,$(C_SRC)) \
            $(patsubst boot/%.asm,build/obj/%.o,$(ASM_SRC))

.PHONY: all apps iso floppy run run-curses clean font debug release

all: $(KERNEL)

# Applications are built by the separate SDK and land in release/apps.
# The APPS list is expanded while the Makefile is read, so targets that
# need ready .kapp files re-invoke make recursively - only then does the
# wildcard see the freshly built files.
apps:
	@$(MAKE) --no-print-directory -C sdk

build/obj:
	@mkdir -p build/obj

build/obj/%.o: kernel/%.c | build/obj
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

build/obj/%.o: boot/%.asm | build/obj
	@echo "  AS   $<"
	@$(AS) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJ) linker.ld
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@grub-file --is-x86-multiboot $@ && echo "  OK   Multiboot header is valid"
	@size $@ 2>/dev/null || true

# --- building the bootable ISO with GRUB ---
# The bootloader used to install onto a hard disk: MBR + the GRUB body.
# Built by the same grub-mkstandalone but with biosdisk, so that after
# installation the system starts from the hard disk itself.
build/hdboot.img: $(KERNEL) grub/grub.cfg | build/obj
	@echo "  HD   bootloader for disk installation"
	@grub-mkstandalone --format=i386-pc --output=build/hd_core.img \
	    --install-modules="biosdisk part_msdos multiboot normal echo configfile test true sleep all_video vbe vga video_bochs video_cirrus minicmd reboot halt" \
	    --modules="biosdisk multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" --compress=xz \
	    "boot/grub/grub.cfg=grub/grub.cfg" "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT)
	@cat /usr/lib/grub/i386-pc/boot.img build/hd_core.img > $@

iso: apps
	@$(MAKE) --no-print-directory $(ISO)

$(ISO): $(KERNEL) grub/grub.cfg build/hdboot.img
	@echo "  ISO  $@"
	@rm -rf build/isodir
	@mkdir -p build/isodir/boot/grub
	@cp $(KERNEL) build/isodir/boot/kvant.bin
	@cp grub/grub.cfg build/isodir/boot/grub/grub.cfg
	@cp build/hdboot.img build/isodir/boot/hdboot.img
	@# A MONOLITHIC eltorito image is built: every required module and
	@# grub.cfg itself are sewn into core.img. Otherwise GRUB would read
	@# 276 separate .mod files and a 2.4 MB font from the drive - on the
	@# worn DVDs of old laptops that hangs right after "Welcome to GRUB!".
	@grub-mkstandalone \
	    --format=i386-pc \
	    --output=build/core.img \
	    --install-modules="biosdisk iso9660 part_msdos multiboot normal echo test true sleep configfile search search_fs_file all_video vbe vga video_bochs video_cirrus minicmd reboot halt" \
	    --modules="biosdisk iso9660 part_msdos multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" \
	    --compress=xz \
	    "boot/grub/grub.cfg=grub/grub.cfg" \
	    "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT)
	@cat /usr/lib/grub/i386-pc/cdboot.img build/core.img > build/eltorito.img
	@mkdir -p build/isodir/boot/grub/i386-pc
	@cp build/eltorito.img build/isodir/boot/grub/i386-pc/eltorito.img
	@xorriso -as mkisofs \
	    -graft-points \
	    -b boot/grub/i386-pc/eltorito.img \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --grub2-boot-info \
	    -iso-level 3 -r -J -joliet-long \
	    -V KVANTOS \
	    -o $@ build/isodir 2>/dev/null
	@isohybrid $@ 2>/dev/null || true
	@mkdir -p release
	@cp $@ release/kvantos.iso
	@cp $(KERNEL) release/kvant.bin
	@echo "  DONE: release/kvantos.iso"

floppy: apps
	@$(MAKE) --no-print-directory build/kvantos.img

build/kvantos.img: $(KERNEL)
	@echo "  FLOPPY  build/kvantos.img (fallback for machines without a DVD)"
	@# There is deliberately NO filesystem on the floppy: core.img laid
	@# down from sector 2 would overwrite the FAT. Instead the kernel and
	@# grub.cfg are embedded INSIDE core.img (memdisk) - no drive and no
	@# filesystem are needed.
	@printf 'set timeout=5\nset default=0\n' > build/fd.cfg
	@printf 'menuentry "KvantOS - graphics 1024x768" { multiboot /boot/kvant.bin ; $(FD_MODULES) boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - VGA text 80x25" { multiboot /boot/kvant.bin text ; $(FD_MODULES) boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - safe mode" { multiboot /boot/kvant.bin text safe ; boot }\n' >> build/fd.cfg
	@grub-mkstandalone --format=i386-pc --output=build/fd_core.img \
	    --install-modules="biosdisk multiboot normal echo configfile test true sleep all_video vbe vga video_bochs video_cirrus minicmd reboot halt" \
	    --modules="biosdisk multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" --compress=xz \
	    "boot/grub/grub.cfg=build/fd.cfg" "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT)
	@cat /usr/lib/grub/i386-pc/boot.img build/fd_core.img > build/kvantos.img
	@truncate -s 1474560 build/kvantos.img
	@mkdir -p release && cp build/kvantos.img release/kvantos-floppy.img
	@echo "  DONE: release/kvantos-floppy.img ($$(du -h build/kvantos.img | cut -f1))"

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -serial stdio

run-curses: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -display curses

debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -s -S -serial stdio

# The full distribution set: ISO, floppy, kernel, applications and an
# empty disk for files. This is the archive attached to a GitHub
# release - the repository itself carries no binaries.
release: iso floppy
	@mkdir -p release
	@test -f release/kvantos-disk.img || python3 sdk/mkdisk.py release/kvantos-disk.img 16 >/dev/null
	@rm -f kvantos-0.1.0-photon.tar.gz
	@tar czf kvantos-0.1.0-photon.tar.gz -C release \
	    kvantos.iso kvantos-floppy.img kvant.bin kvantos-disk.img apps
	@echo "  DONE: kvantos-0.1.0-photon.tar.gz ($$(du -h kvantos-0.1.0-photon.tar.gz | cut -f1))"

font:
	@python3 tools/mkfont.py

clean:
	@rm -rf build release
	@echo "  cleaned"
