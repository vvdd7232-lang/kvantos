.DEFAULT_GOAL := all
# ============================================================
#  KvantOS - build system
# ============================================================
NAME     := kvantos
VERSION  := $(shell awk '$$2 == "KV_VERSION" {gsub(/"/, "", $$3); print $$3}' include/kernel.h)
KERNEL   := build/kvant.bin
ISO      := build/kvantos.iso

CC       := gcc
AS       := nasm
LD       := ld
OBJCOPY  := objcopy

# GCC freestanding headers (stdint.h, stddef.h, stdarg.h) needed with -nostdinc
GCC_INC  := $(shell $(CC) -m32 -print-file-name=include)
GCC_INC64:= $(shell $(CC) -m64 -print-file-name=include)

CFLAGS   := -m32 -march=i586 -mtune=generic -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-pic -fno-pie -nostdlib -nostdinc -Wall -Wextra -O2 \
            -Iinclude -isystem $(GCC_INC) -Wno-unused-parameter
ASFLAGS  := -f elf32
LDFLAGS  := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

# ---- KvantOS 3.0: the 64-bit kernel + UEFI stub ----
# kvant64.bin is a plain ELF64 executable loaded by the UEFI stub
# (boot/kvantefi.c). The red zone is off because interrupts may fire
# at any moment; SSE is off because the kernel never initializes it.
CFLAGS64 := -m64 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
            -fno-pic -fno-pie -nostdlib -nostdinc -Wall -Wextra -O2 \
            -Iinclude -isystem $(GCC_INC64) -Wno-unused-parameter
ASFLAGS64:= -f elf64
LDFLAGS64:= -m elf_x86_64 -T linker64.ld -nostdlib -z noexecstack
KERNEL64 := build/kvant64.bin

# UEFI applications use the Microsoft x86-64 calling convention and are
# position independent; the PE/COFF conversion is done by objcopy.
CFLAGS_EFI := -m64 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fpic -fshort-wchar -mno-red-zone -nostdlib -nostdinc -Wall -Wextra -O2 \
            -Iinclude -isystem $(GCC_INC64)
EFI      := build/kvantefi.efi

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
# boot64.asm / isr64.asm belong to the 64-bit kernel (kvant64.bin).
ASM_SRC  := $(filter-out boot/direct.asm boot/boot64.asm boot/isr64.asm boot/user64.asm,$(wildcard boot/*.asm))
OBJ      := $(patsubst kernel/%.c,build/obj/%.o,$(C_SRC)) \
            $(patsubst boot/%.asm,build/obj/%.o,$(ASM_SRC))

ASM64_SRC:= boot/boot64.asm boot/isr64.asm boot/user64.asm
OBJ64    := $(patsubst kernel/%.c,build/obj64/%.o,$(C_SRC)) \
            $(patsubst boot/%.asm,build/obj64/%.o,$(ASM64_SRC))

# Laboratory-only write diagnostics must never leak through stale object files.
STORAGE_LAB ?= 0
CFLAGS += -include build/storage_config.h
CFLAGS64 += -include build/storage_config.h
.PHONY: storage-config-force
build/storage_config.h: storage-config-force
	@mkdir -p build
	@tmp=$@.$$$$.tmp; printf '#define KV_STORAGE_LAB %s\n' '$(STORAGE_LAB)' > $$tmp; \
	    cmp -s $$tmp $@ || mv $$tmp $@; rm -f $$tmp
$(filter %.o,$(OBJ) $(OBJ64)): build/storage_config.h
build/obj/usb.o build/obj64/usb.o build/obj/usb_bot.o build/obj64/usb_bot.o: include/usb_bot.h
build/obj/fat.o build/obj64/fat.o build/obj/sector_tx.o build/obj64/sector_tx.o: include/sector_tx.h include/vfs.h

ESPIMG   := build/esp.img

.PHONY: all apps iso floppy run run-curses clean font debug release release-inner kv64 efi esp

all: $(KERNEL) $(KERNEL64) $(EFI)

# Applications are built by the separate SDK and land in release/apps.
# The APPS list is expanded while the Makefile is read, so targets that
# need ready .kapp files re-invoke make recursively - only then does the
# wildcard see the freshly built files.
apps:
	@$(MAKE) --no-print-directory -C sdk
	@mkdir -p release/apps release/apps64
	@cp sdk/build/*.kapp release/apps/ 2>/dev/null || true
	@cp sdk/build64/*.kapp release/apps64/ 2>/dev/null || true

build/obj:
	@mkdir -p build/obj

build/mcc_user_blob.h: sdk/isolated/mcc_runtime.c sdk/isolated/mcc_runtime.lds sdk/isolated/runtime_pack.py sdk/isolated/kvuser.h include/kvapp.h sdk/isolated/Makefile
	$(MAKE) -C sdk/isolated CC="$(CC)" runtime
build/obj/mcc.o build/obj64/mcc.o: build/mcc_user_blob.h include/mcc.h

build/obj/mcc.o build/obj/gui.o build/obj/textedit.o build/obj/mouse.o build/obj/keyboard.o build/obj/usb.o build/obj/fat.o build/obj/sector_tx.o build/obj/ntfs.o: CFLAGS += -Os
build/obj64/mcc.o build/obj64/gui.o build/obj64/textedit.o build/obj64/mouse.o build/obj64/keyboard.o build/obj64/usb.o build/obj64/fat.o build/obj64/sector_tx.o build/obj64/ntfs.o: CFLAGS64 += -Os

build/obj/usb.o build/obj64/usb.o: kernel/usb_uhci.h kernel/usb_ehci.h kernel/usb_ohci.h kernel/usb_hub.h
build/obj/fat.o build/obj64/fat.o: kernel/fat_wal.h kernel/fat_meta.h include/sector_tx.h include/vfs.h
build/obj/sector_tx.o build/obj64/sector_tx.o: include/sector_tx.h

build/obj/gui.o build/obj64/gui.o: $(wildcard kernel/gui_editor_*.h) include/vfs.h include/textedit.h

build/obj/%.o: kernel/%.c include/kernel.h include/keymap.h | build/obj
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

build/obj/%.o: boot/%.asm | build/obj
	@echo "  AS   $<"
	@$(AS) $(ASFLAGS) $< -o $@

# ---- 64-bit kernel objects ----
build/obj64:
	@mkdir -p build/obj64

build/obj64/%.o: kernel/%.c include/kernel.h include/keymap.h | build/obj64
	@echo "  CC64 $<"
	@$(CC) $(CFLAGS64) -c $< -o $@

build/obj64/%.o: boot/%.asm | build/obj64
	@echo "  AS64 $<"
	@$(AS) $(ASFLAGS64) $< -o $@

kv64: $(KERNEL64)

$(KERNEL64): $(OBJ64) linker64.ld
	@echo "  LD64 $@"
	@$(LD) $(LDFLAGS64) -o $@ $(OBJ64)
	@size $@ 2>/dev/null || true

# ---- UEFI stub (PE/COFF x86-64 application) ----
efi: $(EFI)

build/kvantefi.o: boot/kvantefi.c include/efi/efi.h | build/obj
	@echo "  CCEFI $<"
	@$(CC) $(CFLAGS_EFI) -maccumulate-outgoing-args -c $< -o $@

build/kvantefi.so: build/kvantefi.o tools/efi.lds
	@echo "  LDEFI $@"
	@$(LD) -nostdlib -znocombreloc -shared -Bsymbolic -T tools/efi.lds -o $@ $<

$(EFI): build/kvantefi.so
	@echo "  EFI  $@"
	@$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	    -j .rel -j .rela -j .reloc --target=efi-app-x86_64 $< $@

# ---- FAT16 ESP image (UEFI boot from CD/USB without any legacy code) ----
esp: $(ESPIMG)

$(ESPIMG): $(EFI) $(KERNEL64) tools/mkesp.py
	@$(MAKE) --no-print-directory apps
	@echo "  ESP  $@"
	@python3 tools/mkesp.py $@ $(EFI) $(KERNEL64) release/apps64

$(KERNEL): $(OBJ) linker.ld
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@if command -v grub-file >/dev/null 2>&1; then \
	    grub-file --is-x86-multiboot $@ && echo "  OK   Multiboot header is valid"; \
	else \
	    echo "  SKIP grub-file not installed"; \
	fi
	@size $@ 2>/dev/null || true

# --- building the bootable ISO with GRUB ---
# The bootloader used to install onto a hard disk: MBR + the GRUB body.
# Built by the same grub-mkstandalone but with biosdisk, so that after
# installation the system starts from the hard disk itself.
# Installed GRUB needs loadable segments, not the host/QEMU symbol table.
# gzip + embedded gzio keep the memdisk safely below its fixed 0x78000 cap.
# Embed only commands used by grub.cfg: i386-pc core has a 0x78000 byte cap.
build/kvant-boot.bin: $(KERNEL) Makefile
	@$(OBJCOPY) --strip-all $< $@.elf
	@gzip -n -9 -c $@.elf > $@
	@rm -f $@.elf

# Comments are useful source documentation, but need not consume GRUB's
# hard i386-pc memdisk cap. HD uses prelinked modules and no ISO-installer
# search/module (the installed kernel must not reinstall itself). Menus/apps stay.
build/hd-grub.cfg: grub/grub.cfg Makefile | build/obj
	@awk 'NF && $$1 !~ /^#/ && $$1 != "insmod" && $$1 != "search" && !index($$0,"hdboot.img") {print}' $< > $@

build/hdboot.img: build/kvant-boot.bin build/hd-grub.cfg tools/mkhdmem.py $(APPS) | build/obj
	@echo "  HD   bootloader for disk installation"
	@python3 tools/mkhdmem.py build/hd-memdisk.tar build/hd-grub.cfg build/kvant-boot.bin release/apps
	@printf 'set root=(memdisk)\nset prefix=(memdisk)/boot/grub\nnormal (memdisk)/boot/grub/grub.cfg\n' > build/hd-prefix.cfg
	@grub-mkimage --format=i386-pc --output=build/hd_core.img \
	    --prefix='(memdisk)/boot/grub' --config=build/hd-prefix.cfg \
	    --memdisk=build/hd-memdisk.tar --compression=xz \
	    memdisk tar gzio biosdisk part_msdos multiboot normal configfile vbe vga video_bochs video_cirrus reboot halt
	@cat /usr/lib/grub/i386-pc/boot.img build/hd_core.img > $@

iso: apps
	@$(MAKE) --no-print-directory $(ISO)

# A laboratory ISO is deliberately non-installable: no embedded disk installer
# payload, and no GRUB search/module referencing one. Normal media are unchanged.
$(ISO): $(KERNEL) $(KERNEL64) $(EFI) grub/grub.cfg Makefile $(if $(filter 1,$(STORAGE_LAB)),,build/hdboot.img)
	@echo "  ISO  $@"
	@rm -rf build/isodir
	@mkdir -p build/isodir/boot/grub build/isodir/boot/apps
	@cp $(KERNEL) build/isodir/boot/kvant.bin
	@if [ "$(STORAGE_LAB)" = 1 ]; then awk 'index($$0,"hdboot.img")==0 {if($$0 ~ /^set timeout=/) print "set timeout=0"; else print}' grub/grub.cfg > build/isodir/boot/grub/grub.cfg; else cp grub/grub.cfg build/isodir/boot/grub/grub.cfg; fi
	@if [ "$(STORAGE_LAB)" != 1 ]; then cp build/hdboot.img build/isodir/boot/hdboot.img; fi
	@for a in $(APPS); do cp $$a build/isodir/boot/apps/; done
	@# KvantOS 3.0 UEFI path: the EFI stub + 64-bit kernel live in the
	@# ISO9660 tree (USB/hard-disk installs) and a second copy of them
	@# goes into esp.img, the El Torito EFI boot image (CD/DVD boot).
	@mkdir -p build/isodir/EFI/BOOT
	@cp $(EFI) build/isodir/EFI/BOOT/BOOTX64.EFI
	@cp $(KERNEL64) build/isodir/boot/kvant64.bin
	@python3 tools/mkesp.py build/esp.img $(EFI) $(KERNEL64) release/apps64
	@cp build/esp.img build/isodir/esp.img
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
	    --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
	    -partition_offset 16 \
	    -efi-boot-part --efi-boot-image \
	    -b boot/grub/i386-pc/eltorito.img \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --grub2-boot-info \
	    -eltorito-alt-boot \
	    -e esp.img \
	    -no-emul-boot \
	    -iso-level 3 -r -J -joliet-long \
	    -V KVANTOS \
	    -o $@ build/isodir 2>/dev/null
	@mkdir -p release
	@cp $@ release/kvantos.iso
	@cp $(KERNEL) release/kvant.bin
	@cp $(KERNEL64) release/kvant64.bin
	@cp $(EFI) release/kvantefi.efi
	@cp build/esp.img release/esp.img
	@echo "  DONE: release/kvantos.iso (BIOS + UEFI)"

floppy: apps
	@$(MAKE) --no-print-directory build/kvantos.img

build/kvantos.img: build/kvant-boot.bin tools/mkhdmem.py Makefile
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
	@python3 tools/mkhdmem.py build/fd-memdisk.tar build/fd.cfg build/kvant-boot.bin
	@printf 'set root=(memdisk)\nset prefix=(memdisk)/boot/grub\nnormal (memdisk)/boot/grub/grub.cfg\n' > build/fd-prefix.cfg
	@grub-mkimage --format=i386-pc --output=build/fd_core.img \
	    --prefix='(memdisk)/boot/grub' --config=build/fd-prefix.cfg \
	    --memdisk=build/fd-memdisk.tar --compression=xz \
	    memdisk tar gzio biosdisk multiboot normal configfile vbe vga video_bochs video_cirrus reboot halt
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

build/kvantos-direct.iso: $(KERNEL) $(KERNEL64) $(EFI) tools/mkdirect.py boot/direct.asm
	@python3 tools/mkdirect.py $(KERNEL) release/apps $@ \
	    --efi $(EFI) --kernel64 $(KERNEL64) --apps64 release/apps64
	@mkdir -p release && cp $@ release/kvantos-direct.iso
	@echo "  DONE: release/kvantos-direct.iso"

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -serial stdio

run-curses: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -display curses

debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -s -S -serial stdio

# The full distribution set: GRUB and direct BIOS/UEFI ISOs, floppy,
# kernels, applications and an empty disk image. This target only builds
# files locally; publishing is
# handled separately by .github/workflows/release.yml on the v3.1.4 tag.
release:
	@test "$(STORAGE_LAB)" = 0 || { echo 'Refusing release artifacts from STORAGE_LAB build'; exit 1; }
	@mkdir -p build
	@if $(MAKE) --no-print-directory release-inner >build/release.log 2>&1; then \
	    cat build/release.log; \
	else \
	    rc=$$?; \
	    cat build/release.log; \
	    exit $$rc; \
	fi

release-inner: iso
	@test "$(STORAGE_LAB)" = 0 || { echo 'Refusing release artifacts from STORAGE_LAB build'; exit 1; }
	@$(MAKE) --no-print-directory floppy
	@# Run the direct builder after the GRUB ISO: both currently create build/esp.img.
	@$(MAKE) --no-print-directory iso-direct
	@mkdir -p release
	@test -f release/kvantos-disk.img || python3 sdk/mkdisk.py release/kvantos-disk.img 16 >/dev/null
	@rm -f kvantos-$(VERSION).tar.gz
	@cp LICENSE CHANGELOG.md README.md README.ru.md INSTALL.md INSTALL.ru.md RELEASE.md RELEASE.ru.md USB-IMPORT.md USB-IMPORT.ru.md release/
	@mkdir -p release/sdk
	@cp sdk/WORKSHOP.md sdk/WORKSHOP.ru.md release/sdk/
	@cd release && sha256sum *.iso *.img *.bin *.efi apps/*.kapp apps64/*.kapp > SHA256SUMS
	@tar czf kvantos-$(VERSION).tar.gz -C release \
	    kvantos.iso kvantos-direct.iso kvantos-floppy.img \
	    kvant.bin kvant64.bin kvantefi.efi esp.img kvantos-disk.img apps apps64 \
	    SHA256SUMS LICENSE CHANGELOG.md README.md README.ru.md INSTALL.md INSTALL.ru.md RELEASE.md RELEASE.ru.md USB-IMPORT.md USB-IMPORT.ru.md sdk
	@echo "  DONE: kvantos-$(VERSION).tar.gz ($$(du -h kvantos-$(VERSION).tar.gz | cut -f1))"

font:
	@python3 tools/mkfont.py

clean:
	@rm -rf build release
	@echo "  cleaned"

# Shared editor model and private GUI includes must rebuild both architectures.
build/obj/gui.o build/obj64/gui.o: kernel/gui_editor_state.h kernel/gui_editor_view.h kernel/gui_editor_input.h include/textedit.h
build/obj/textedit.o build/obj64/textedit.o: include/textedit.h

# Workshop preprocessing workspace/limits affect both targets.
build/obj/mcc.o build/obj64/mcc.o: kernel/mcc_pp.h kernel/mcc_types.h kernel/mcc_headers.h

# Bounded native rename names; no filesystem path interpretation in flat drivers.
build/obj/ramfs.o build/obj64/ramfs.o build/obj/kvfs.o build/obj64/kvfs.o: include/native_name.h

VFS_OBJECTS := fat fat_lab filemgr gui kvfs main ntfs partition shell utilcmds vfs vfs_native
$(addprefix build/obj/,$(addsuffix .o,$(VFS_OBJECTS))) $(addprefix build/obj64/,$(addsuffix .o,$(VFS_OBJECTS))): include/vfs.h

build/obj/filemgr.o build/obj64/filemgr.o: include/native_name.h include/filemgr.h
build/obj/gui.o build/obj64/gui.o: include/filemgr.h

build/obj/gui.o build/obj64/gui.o: kernel/gui_store_policy.h
