# ============================================================
#  KvantOS SDK - building .kapp applications
#
#  make            - build every sample in apps/
#  make apps/name.kapp - build a single application
#  make clean      - clean up
#
#  Finished .kapp files land in build/ and also in ../release/apps,
#  from where they can be written into a disk image.
# ============================================================
CC  := gcc
LD  := ld

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

# The same flags as the kernel: freestanding code without libc.
# -fno-pic is mandatory: an application is built for a fixed address.
CFLAGS := -m32 -march=i586 -std=gnu11 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc \
          -Wall -Wextra -O2 -I../include -isystem $(GCC_INC)

LDFLAGS := -m elf_i386 -T kapp.ld -nostdlib

SRC  := $(wildcard apps/*.c)
APPS := $(patsubst apps/%.c,build/%.kapp,$(SRC))

.PHONY: all clean list

all: $(APPS)
	@echo ""
	@echo "  Done. Applications are in sdk/build/:"
	@ls -1 build/*.kapp 2>/dev/null | sed 's|^|    |' || true

build:
	@mkdir -p build

# Each application is one .c file, a separate ELF, then packaging
build/%.elf: apps/%.c kapp.ld | build
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o build/$*.o
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ build/$*.o

build/%.kapp: build/%.elf mkkapp.py
	@python3 mkkapp.py $< $@ "$(shell sed -n 's/.*KAPP_NAME[ \t]*"\(.*\)".*/\1/p' apps/$*.c | head -1)"
	@mkdir -p ../release/apps && cp $@ ../release/apps/

list:
	@echo "Available applications:"
	@ls -1 apps/*.c 2>/dev/null | sed 's|apps/|  |; s|\.c$$||'

clean:
	@rm -rf build
	@echo "  cleaned"
