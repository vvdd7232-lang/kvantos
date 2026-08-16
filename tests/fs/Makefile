# ============================================================
#  KvantOS - filesystem driver tests that run on the host
#
#  The FAT and NTFS drivers are plain C over a "read a sector /
#  write a sector" interface, so they can be compiled for the host
#  and pointed at a disk image. Finding a bug here takes a second
#  instead of a minute-long boot in an emulator.
#
#    make            build the harness
#    make image      build a test disk (needs sfdisk/mkfs.vfat/mkntfs)
#    make check      run the tests
# ============================================================
CC     := gcc
CFLAGS := -std=gnu11 -g -O1 -Wall -I. -Iinclude
SRC    := harness.c ../../kernel/fat.c ../../kernel/ntfs.c \
          ../../kernel/vfs.c ../../kernel/partition.c ../../kernel/vfs_native.c
IMAGE  := build/fsdisk.img

all: build/harness

build:
	@mkdir -p build include

build/harness: harness.c $(SRC) | build
	@cp ../../include/vfs.h include/
	@cp kernel_stub.h include/kernel.h
	$(CC) $(CFLAGS) $(SRC) -o $@

image: | build
	./mkimage.sh $(IMAGE)

check: build/harness
	@test -f $(IMAGE) || { echo "run 'make image' first"; exit 1; }
	@cp $(IMAGE) build/test.img
	./build/harness build/test.img

clean:
	rm -rf build include

.PHONY: all image check clean
