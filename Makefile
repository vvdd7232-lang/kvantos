# ============================================================
#  KvantOS - система сборки
# ============================================================
NAME     := kvantos
KERNEL   := build/kvant.bin
ISO      := build/kvantos.iso

CC       := gcc
AS       := nasm
LD       := ld

# freestanding-заголовки GCC (stdint.h, stddef.h, stdarg.h) при -nostdinc
GCC_INC  := $(shell $(CC) -m32 -print-file-name=include)

CFLAGS   := -m32 -march=i586 -mtune=generic -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-pic -fno-pie -nostdlib -nostdinc -Wall -Wextra -O2 \
            -Iinclude -isystem $(GCC_INC) -Wno-unused-parameter
ASFLAGS  := -f elf32
LDFLAGS  := -m elf_i386 -T linker.ld -nostdlib -z noexecstack

# Приложения .kapp вкладываются ВНУТРЬ загрузочного образа: тогда они
# попадают в систему на любой машине, даже без диска и без сети -
# достаточно записать ISO. Ядро подхватывает их как модули Multiboot.
# Берём из release/apps: каталог sdk/build называется "build" и не
# попадает в снапшот рабочей области, а release/apps сохраняется.
APPS       := $(wildcard release/apps/*.kapp)
APP_GRAFT  := $(foreach a,$(APPS),"boot/apps/$(notdir $(a))=$(a)")
FD_MODULES := $(foreach a,$(APPS),module /boot/apps/$(notdir $(a)) $(notdir $(a)) ;)

C_SRC    := $(wildcard kernel/*.c)
ASM_SRC  := $(wildcard boot/*.asm)
OBJ      := $(patsubst kernel/%.c,build/obj/%.o,$(C_SRC)) \
            $(patsubst boot/%.asm,build/obj/%.o,$(ASM_SRC))

.PHONY: all iso run run-curses clean font debug

all: $(KERNEL)

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
	@grub-file --is-x86-multiboot $@ && echo "  OK   Multiboot-заголовок корректен"
	@size $@ 2>/dev/null || true

# --- сборка загрузочного ISO с GRUB ---
# Загрузчик для установки на жёсткий диск: MBR + тело GRUB.
# Собирается тем же grub-mkstandalone, но с biosdisk - чтобы
# после установки система стартовала уже с винчестера.
build/hdboot.img: $(KERNEL) grub/grub.cfg | build/obj
	@echo "  HD   загрузчик для установки на диск"
	@grub-mkstandalone --format=i386-pc --output=build/hd_core.img \
	    --install-modules="biosdisk part_msdos multiboot normal echo configfile test true sleep all_video vbe vga video_bochs video_cirrus minicmd reboot halt" \
	    --modules="biosdisk multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" --compress=xz \
	    "boot/grub/grub.cfg=grub/grub.cfg" "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT) 2>/dev/null
	@cat /usr/lib/grub/i386-pc/boot.img build/hd_core.img > $@

iso: $(ISO)

$(ISO): $(KERNEL) grub/grub.cfg build/hdboot.img
	@echo "  ISO  $@"
	@rm -rf build/isodir
	@mkdir -p build/isodir/boot/grub
	@cp $(KERNEL) build/isodir/boot/kvant.bin
	@cp grub/grub.cfg build/isodir/boot/grub/grub.cfg
	@cp build/hdboot.img build/isodir/boot/hdboot.img
	@# Собираем МОНОЛИТНЫЙ eltorito-образ: все нужные модули и сам
	@# grub.cfg зашиты внутрь core.img. Иначе GRUB дочитывает 276
	@# отдельных .mod и шрифт 2.4 МБ с привода - на изношенных DVD
	@# старых ноутбуков это виснет сразу после "Welcome to GRUB!".
	@grub-mkstandalone \
	    --format=i386-pc \
	    --output=build/core.img \
	    --install-modules="biosdisk iso9660 part_msdos multiboot normal echo linux16 test true sleep configfile search search_label search_fs_uuid search_fs_file terminal videotest videoinfo all_video vbe vga video_bochs video_cirrus gfxterm minicmd reboot halt" \
	    --modules="biosdisk iso9660 part_msdos multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" \
	    --compress=xz \
	    "boot/grub/grub.cfg=grub/grub.cfg" \
	    "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT) 2>/dev/null
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
	@echo "  ГОТОВО: release/kvantos.iso"

floppy: $(KERNEL)
	@echo "  FLOPPY  build/kvantos.img (запасной вариант без DVD)"
	@# Файловой системы на дискете НЕТ намеренно: core.img, положенный
	@# со 2-го сектора, затирал бы FAT. Вместо этого ядро и grub.cfg
	@# вкладываются ВНУТРЬ core.img (memdisk) - привод и ФС не нужны.
	@printf 'set timeout=5\nset default=0\n' > build/fd.cfg
	@printf 'menuentry "KvantOS - graphics 1024x768" { multiboot /boot/kvant.bin ; $(FD_MODULES) boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - VGA text 80x25" { multiboot /boot/kvant.bin text ; $(FD_MODULES) boot }\n' >> build/fd.cfg
	@printf 'menuentry "KvantOS - safe mode" { multiboot /boot/kvant.bin text safe ; boot }\n' >> build/fd.cfg
	@grub-mkstandalone --format=i386-pc --output=build/fd_core.img \
	    --install-modules="biosdisk multiboot normal echo configfile test true sleep all_video vbe vga video_bochs video_cirrus minicmd reboot halt" \
	    --modules="biosdisk multiboot normal configfile" \
	    --locales="" --fonts="" --themes="" --compress=xz \
	    "boot/grub/grub.cfg=build/fd.cfg" "boot/kvant.bin=$(KERNEL)" \
	    $(APP_GRAFT) 2>/dev/null
	@cat /usr/lib/grub/i386-pc/boot.img build/fd_core.img > build/kvantos.img
	@truncate -s 1474560 build/kvantos.img
	@mkdir -p release && cp build/kvantos.img release/kvantos-floppy.img
	@echo "  ГОТОВО: release/kvantos-floppy.img ($$(du -h build/kvantos.img | cut -f1))"

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -serial stdio

run-curses: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -display curses

debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 128 -s -S -serial stdio

font:
	@python3 tools/mkfont.py

clean:
	@rm -rf build release
	@echo "  очищено"
