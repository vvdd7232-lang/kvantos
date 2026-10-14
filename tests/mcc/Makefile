CC := gcc
FLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -DMCC_HOSTTEST -I../../include

check: build/libmcc.so build/libmcc64.so
	python3 test_mcc.py
	MCC_ARCH64=1 python3 test_mcc.py

build/libmcc.so: ../../kernel/mcc.c ../../include/mcc.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(FLAGS) -shared -fPIC $< -o $@

.PHONY: check sanitize ramfs

sanitize: build/fuzz_mcc build/fuzz_mcc64
	./build/fuzz_mcc
	./build/fuzz_mcc64

build/fuzz_mcc: fuzz_mcc.c ../../kernel/mcc.c ../../include/mcc.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer fuzz_mcc.c ../../kernel/mcc.c -o $@

ramfs: build/test_ramfs
	./build/test_ramfs

build/test_ramfs: test_ramfs.c ../../kernel/ramfs.c
	@mkdir -p build
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin -ffunction-sections -fdata-sections -I../../include -fsanitize=address,undefined -fno-omit-frame-pointer test_ramfs.c -Wl,--gc-sections -o $@

build/libmcc64.so: ../../kernel/mcc.c ../../include/mcc.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -shared -fPIC $< -o $@

build/fuzz_mcc64: fuzz_mcc.c ../../kernel/mcc.c ../../include/mcc.h
	@mkdir -p build
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -fsanitize=address,undefined -fno-omit-frame-pointer fuzz_mcc.c ../../kernel/mcc.c -o $@
