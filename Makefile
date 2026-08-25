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

# .kapp applications travel INSIDE the images: on the ISO they live as
# ordinary files under /boot/apps (grub.cfg loads them as Multiboot
# modules); in the hard-disk installer payload (hdboot.img) they are
# embedded into core.img outright. Either way burning the ISO is enough
# to get the apps on any machine, with no disk and no network.
# They are taken from release/apps.
APPS       := $(wildcard release/apps/*.kapp)
APP_GRAFT  := $(foreach a,$(APPS),"boot/apps/$(notdir $(a))=$(a)")

C_SRC    := $(wildcard kernel/*.c)
# direct.asm is the GRUB-free boot stub: it is assembled separately by
# tools/mkdirect.py and must never end up inside the kernel image.
ASM_SRC  := $(filter-out boot/direct.asm,$(wildcard boot/*.asm))
OBJ      := $(patsubst kernel/%.c,build/obj/%.o,$(C_SRC)) \
            $(patsubst boot/%.asm,build/obj/%.o,$(ASM_SRC))

.PHONY: all apps iso floppy run run-curses clean font debug release release-inner

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
	    --install-modules="biosdisk part_msdos multiboot normal echo configfile test true sleep vbe vga minicmd reboot halt" \
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
	@mkdir -p build/isodir/boot/grub build/isodir/boot/apps
	@cp $(KERNEL) build/isodir/boot/kvant.bin
	@cp grub/grub.cfg build/isodir/boot/grub/grub.cfg
	@cp build/hdboot.img build/isodir/boot/hdboot.img
	@for a in $(APPS); do cp $$a build/isodir/boot/apps/; done
	@# core.img stays MONOLITHIC in GRUB modules: every module the menu
	@# needs is sewn into it (--install-modules), so GRUB never reads
	@# the 276 separate .mod files / 2.4 MB font off the disc - on worn
	@# DVDs of old laptops that hangs right after "Welcome to GRUB!".
	@# The kernel and the .kapp files, however, live on the ISO as
	@# ordinary files: embedding them too overflows the 0x78000-byte
	@# core image limit of i386-pc ("core image is too big").
	@grub-mkimage \
	    --format=i386-pc \
	    --output=build/core.img \
	    --prefix=/boot/grub \
	    --compress=xz \
	    biosdisk iso9660 part_msdos multiboot normal echo test true sleep configfile search search_fs_file vbe vga minicmd reboot halt
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
	@# down from sector 2 would overwrite the FAT. Instead the kernel
	@# and the menu are embedded INSIDE core.img (memdisk) - no drive
	@# and no filesystem are needed. The .kapp applications do NOT fit
	@# into the 0x78000-byte core image limit together with the kernel
	@# and GRUB, so the floppy boots the bare system (the apps come
	@# from the ISO or are installed onto a disk later).
	@printf 'set timeout=5\nset default=0\n' > build/fd.cfg
	@printf 'menuentry "KvantOS - graphics 1024x768" { multiboot /boot/kvant.bin ; boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - VGA text 80x25" { multiboot /boot/kvant.bin text ; boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - safe mode" { multiboot /boot/kvant.bin text safe ; boot }\n' >> build/fd.cfg
	@grub-mkstandalone --format=i386-pc --output=build/fd_core.img \
	    --install-modules="biosdisk multiboot normal echo configfile test true sleep vbe vga minicmd reboot halt" \
	    --modules="biosdisk multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" --compress=xz \
	    "boot/grub/grub.cfg=build/fd.cfg" "boot/kvant.bin=$(KERNEL)"
	@cat /usr/lib/grub/i386-pc/boot.img build/fd_core.img > build/kvantos.img
	@truncate -s 1474560 build/kvantos.img
	@mkdir -p release && cp build/kvantos.img release/kvantos-floppy.img
	@echo "  DONE: release/kvantos-floppy.img ($$(du -h build/kvantos.img | cut -f1))"

# --- GRUB-free bootable ISO (fallback when GRUB tools are unavailable) ---
# A tiny stub (boot/direct.asm) becomes the El Torito boot image: it
# sets VBE 1024x768x32 itself, copies the kernel to 1 MiB, lays out the
# .kapp modules at 2 MiB and hands over a full multiboot_info. Built
# with nasm + pycdlib only (tools/mkdirect.py vendors the latter).
iso-direct: apps
	@$(MAKE) --no-print-directory build/kvantos-direct.iso

build/kvantos-direct.iso: $(KERNEL) tools/mkdirect.py boot/direct.asm
	@python3 tools/mkdirect.py $(KERNEL) release/apps $@
	@mkdir -p release && cp $@ release/kvantos-direct.iso
	@echo "  DONE: release/kvantos-direct.iso"

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -serial stdio

run-curses: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -display curses

debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -s -S -serial stdio

# The full distribution set: ISO, floppy, kernel, applications and an
# empty disk for files. This is the archive attached to a GitHub
# release - the repository itself carries no binaries.
#
# On CI the whole build is logged; when it fails, the tail of the log
# is pushed back to the repository as the annotated tag "ci-diag", so
# the failure reason can be inspected even without Actions log access.
release:
	@mkdir -p build
	@if $(MAKE) --no-print-directory release-inner >build/release.log 2>&1; then \
	    cat build/release.log; \
	else \
	    rc=$$?; \
	    cat build/release.log; \
	    echo; \
	    if [ "$$GITHUB_ACTIONS" = "true" ]; then \
	        tail -c 1500 build/release.log 2>/dev/null | tr '\n\r' '  ' > build/diag.tail; \
	        echo "::error::release build failed rc=$$rc tail: $$(cat build/diag.tail)"; \
	        if [ -d .git ]; then \
	            git config user.email ci@kvantos.local 2>/dev/null || true; \
	            git config user.name "KvantOS CI" 2>/dev/null || true; \
	            tail -c 12000 build/release.log > build/diag.msg 2>/dev/null || true; \
	            if git tag -f ci-diag -F build/diag.msg >/dev/null 2>&1; then \
	                if git push -f origin refs/tags/ci-diag >/dev/null 2>build/diag.pusherr; then \
	                    echo "DIAG: pushed tag ci-diag with the build log"; \
	                else \
	                    echo "::error::DIAG tag push failed: $$(tr '\n\r' '  ' < build/diag.pusherr)"; \
	                fi; \
	            else \
	                echo "::error::DIAG git tag creation failed"; \
	            fi; \
	        fi; \
	    fi; \
	    exit $$rc; \
	fi

release-inner: iso floppy
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
