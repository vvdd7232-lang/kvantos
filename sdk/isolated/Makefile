CC ?= gcc
COMMON = -std=gnu11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-pic -msoft-float -mno-sse -mno-sse2 -mno-mmx -Wall -Wextra -Werror
all: build/boxes32.kapp build/boxes64.kapp
build/boxes32.o: boxes.c kvuser.h
	@mkdir -p build
	$(CC) $(COMMON) -m32 -march=i586 -c $< -o $@
build/boxes64.o: boxes.c kvuser.h
	@mkdir -p build
	$(CC) $(COMMON) -m64 -mno-red-zone -c $< -o $@
build/boxes32.elf: build/boxes32.o user.lds
	ld -m elf_i386 -T user.lds -z noexecstack $< -o $@
build/boxes64.elf: build/boxes64.o user.lds
	ld -m elf_x86_64 -T user.lds -z noexecstack $< -o $@
build/boxes32.kapp: build/boxes32.elf pack.py
	python3 pack.py $< $@ 0
build/boxes64.kapp: build/boxes64.elf pack.py
	python3 pack.py $< $@ 1
.PHONY: all clean
clean:
	rm -rf build

# Prelinked adapter runs ONLY in CPL3. No host compiler is invoked by Workshop.
runtime: ../../build/mcc_user_blob.h
build/mcc_runtime32.o: mcc_runtime.c kvuser.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(COMMON) -I../../include -m32 -march=i586 -c $< -o $@
build/mcc_runtime64.o: mcc_runtime.c kvuser.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(COMMON) -I../../include -m64 -mno-red-zone -c $< -o $@
build/mcc_runtime32.elf: build/mcc_runtime32.o mcc_runtime.lds
	ld -m elf_i386 -T mcc_runtime.lds -z noexecstack $< -o $@
build/mcc_runtime64.elf: build/mcc_runtime64.o mcc_runtime.lds
	ld -m elf_x86_64 -T mcc_runtime.lds -z noexecstack $< -o $@
../../build/mcc_user_blob.h: build/mcc_runtime32.elf build/mcc_runtime64.elf runtime_pack.py
	python3 runtime_pack.py build/mcc_runtime32.elf build/mcc_runtime64.elf $@
.PHONY: runtime
