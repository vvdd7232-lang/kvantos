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
CFLAGS := -std=gnu11 -g -O1 -Wall -DMCC_HOSTTEST -I. -Iinclude -I../../include
SRC    := harness.c ../../kernel/fat.c ../../kernel/sector_tx.c ../../kernel/ntfs.c \
          ../../kernel/vfs.c ../../kernel/partition.c ../../kernel/vfs_native.c ../../kernel/blk.c ../../kernel/ramfs.c
IMAGE  := build/fsdisk.img

all: build/harness

build:
	@mkdir -p build include

build/harness: ../../kernel/fat_meta.h ../../kernel/fat_wal.h harness.c $(SRC) ../../include/vfs.h ../../include/sector_tx.h | build
	@cp ../../include/vfs.h ../../include/sector_tx.h include/
	@cp kernel_stub.h include/kernel.h
	# Legacy algorithm tests explicitly opt in to RAM undo; NOT normal policy.
	$(CC) $(CFLAGS) -DFAT_TX_TEST $(SRC) -o $@

image: | build
	./mkimage.sh $(IMAGE)

check: build/harness
	@test -f $(IMAGE) || { echo "run 'make image' first"; exit 1; }
	@cp $(IMAGE) build/test.img
	./build/harness build/test.img

clean:
	rm -rf build include

.PHONY: all image check clean

# Does not mount or format a host device; private regular-file images only.
.PHONY: gpt
gpt: build/harness
	python3 test_gpt.py

# Explicitly provisioned regular-file journal; never linked into an ISO.
build/fat-fault: ../../kernel/fat_meta.h ../../kernel/fat_wal.h fat_fault.c harness.c $(SRC) ../../include/sector_tx.h | build
	@cp ../../include/vfs.h ../../include/sector_tx.h include/
	@cp kernel_stub.h include/kernel.h
	$(CC) $(CFLAGS) -DFAT_TX_TEST fat_fault.c $(filter-out harness.c,$(SRC)) -o $@

.PHONY: fat-write
fat-write: build/fat-fault
	python3 test_fat_write.py

build/fat-fault-sanitize: ../../kernel/fat_meta.h ../../kernel/fat_wal.h fat_fault.c harness.c $(SRC) ../../include/sector_tx.h | build
	@cp ../../include/vfs.h ../../include/sector_tx.h include/
	@cp kernel_stub.h include/kernel.h
	$(CC) $(CFLAGS) -DFAT_TX_TEST -fsanitize=address,undefined -fno-omit-frame-pointer fat_fault.c $(filter-out harness.c,$(SRC)) -o $@

# Normal policy, WITHOUT FAT_TX_TEST or KV_STORAGE_LAB; real FAT + STX + VFS.
build/fat-owned: fat_owned.c harness.c $(SRC) ../../kernel/fat_meta.h ../../kernel/fat_wal.h ../../include/sector_tx.h | build
	@cp ../../include/vfs.h ../../include/sector_tx.h include/
	@cp kernel_stub.h include/kernel.h
	$(CC) $(CFLAGS) fat_owned.c $(filter-out harness.c,$(SRC)) -o $@

build/fat-owned-sanitize: fat_owned.c harness.c $(SRC) ../../kernel/fat_meta.h ../../kernel/fat_wal.h ../../include/sector_tx.h | build
	@cp ../../include/vfs.h ../../include/sector_tx.h include/
	@cp kernel_stub.h include/kernel.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer fat_owned.c $(filter-out harness.c,$(SRC)) -o $@

.PHONY: fat-owned
fat-owned: build/fat-owned
	python3 test_fat_owned.py

.PHONY: fat-perf
fat-perf: build/fat-owned
	python3 test_fat_perf.py

.PHONY: fat-meta-eviction
fat-meta-eviction: build/fat-owned
	python3 test_fat_meta_eviction.py

# Real NTFS images are created only as private regular files under build/.
# Requires ntfs-3g tools; never loops, mounts, or writes a physical device.
.PHONY: ntfs-image
ntfs-image: build/harness
	python3 test_ntfs_image.py
