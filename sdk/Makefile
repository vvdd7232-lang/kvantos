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
GCC_INC64 := $(shell $(CC) -m64 -print-file-name=include)

# The same flags as the kernel: freestanding code without libc.
# -fno-pic is mandatory: an application is built for a fixed address.
CFLAGS := -m32 -march=i586 -std=gnu11 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc \
          -Wall -Wextra -O2 -I../include -isystem $(GCC_INC)

LDFLAGS := -m elf_i386 -T kapp.ld -nostdlib

# KvantOS 3.0: the 64-bit kernel needs 64-bit applications. The red
# zone is off (interrupts may fire inside an application), SSE is off
# (the kernel never saves XMM state).
CFLAGS64 := -m64 -std=gnu11 -ffreestanding -fno-builtin \
          -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
          -fno-pic -fno-pie -nostdlib -nostdinc \
          -Wall -Wextra -O2 -I../include -isystem $(GCC_INC64)

LDFLAGS64 := -m elf_x86_64 -T kapp.ld -nostdlib

SRC   := $(wildcard apps/*.c)
APPS  := $(patsubst apps/%.c,build/%.kapp,$(SRC))
APPS64:= $(patsubst apps/%.c,build64/%.kapp,$(SRC))

.PHONY: all clean list

all: $(APPS) $(APPS64)
	@echo ""
	@echo "  Done. Applications are in sdk/build/ (32-bit) and sdk/build64/ (64-bit):"
	@ls -1 build/*.kapp 2>/dev/null | sed 's|^|    |' || true

build:
	@mkdir -p build

build64:
	@mkdir -p build64

# Each application is one .c file, a separate ELF, then packaging
build/%.elf: apps/%.c kapp.ld | build
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o build/$*.o
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ build/$*.o

build/%.kapp: build/%.elf mkkapp.py
	@python3 mkkapp.py $< $@ "$(shell sed -n 's/.*KAPP_NAME[ \t]*"\(.*\)".*/\1/p' apps/$*.c | head -1)"
	@mkdir -p ../release/apps && cp $@ ../release/apps/

build64/%.elf: apps/%.c kapp.ld | build64
	@echo "  CC64 $<"
	@$(CC) $(CFLAGS64) -c $< -o build64/$*.o
	@echo "  LD64 $@"
	@$(LD) $(LDFLAGS64) -o $@ build64/$*.o

build64/%.kapp: build64/%.elf mkkapp.py
	@python3 mkkapp.py --arch64 $< $@ "$(shell sed -n 's/.*KAPP_NAME[ \t]*"\(.*\)".*/\1/p' apps/$*.c | head -1)"
	@mkdir -p ../release/apps64 && cp $@ ../release/apps64/

list:
	@echo "Available applications:"
	@ls -1 apps/*.c 2>/dev/null | sed 's|apps/|  |; s|\.c$$||'

clean:
	@rm -rf build build64
	@echo "  cleaned"
