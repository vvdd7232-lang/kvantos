CC := gcc
FLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -DMCC_HOSTTEST -I../../include

check: build/libmcc.so build/libmcc64.so
	python3 test_mcc.py
	MCC_ARCH64=1 python3 test_mcc.py

build/libmcc.so: ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(FLAGS) -shared -fPIC $< -o $@

.PHONY: check sanitize ramfs

sanitize: build/fuzz_mcc build/fuzz_mcc64
	./build/fuzz_mcc
	./build/fuzz_mcc64

build/fuzz_mcc: fuzz_mcc.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie fuzz_mcc.c ../../kernel/mcc.c -o $@

ramfs: build/test_ramfs build/test_ramfs_commands
	./build/test_ramfs
	./build/test_ramfs_commands

build/test_ramfs: test_ramfs.c ../../kernel/ramfs.c
	@mkdir -p build
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin -ffunction-sections -fdata-sections -I../../include -fsanitize=address,undefined -fno-omit-frame-pointer -DMCC_HOSTTEST -no-pie test_ramfs.c -Wl,--gc-sections -o $@

build/libmcc64.so: ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h ../../include/kvapp.h
	@mkdir -p build
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -shared -fPIC $< -o $@

build/fuzz_mcc64: fuzz_mcc.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h
	@mkdir -p build
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie fuzz_mcc.c ../../kernel/mcc.c -o $@

.PHONY: editors
editors: build/test_textedit
	./build/test_textedit

build/test_textedit: test_textedit.c ../../kernel/textedit.c ../../include/textedit.h
	@mkdir -p build
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -Werror -I../../include -fsanitize=address,undefined -fno-omit-frame-pointer test_textedit.c ../../kernel/textedit.c -o $@

.PHONY: input
input: build/test_keymap
	./build/test_keymap

build/test_keymap: test_keymap.c ../../kernel/keymap.c ../../include/keymap.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer test_keymap.c ../../kernel/keymap.c -o $@

.PHONY: storage
storage: build/test_kvfs build/test_usb_bot build/test_sector_tx build/test_store_format
	./build/test_kvfs
	./build/test_usb_bot
	./build/test_sector_tx
	./build/test_store_format

build/test_kvfs: test_kvfs.c ../../kernel/kvfs.c ../../include/vfs.h
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-unused-parameter -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined test_kvfs.c -Wl,--gc-sections -no-pie -o $@

build/test_usb_bot: test_usb_bot.c ../../kernel/usb_bot.c ../../include/usb_bot.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined test_usb_bot.c ../../kernel/usb_bot.c -o $@


build/test_sector_tx: test_sector_tx.c ../../kernel/sector_tx.c ../../include/sector_tx.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined test_sector_tx.c ../../kernel/sector_tx.c -o $@

.PHONY: vfs
vfs: build/test_vfs_native build/test_shell_vcp build/test_shell_vls build/test_ntfs_listing
	./build/test_vfs_native
	./build/test_shell_vcp
	./build/test_shell_vls
	./build/test_ntfs_listing

build/test_vfs_native: test_vfs_native.c ../../kernel/vfs_native.c ../../kernel/vfs.c ../../kernel/ramfs.c ../../include/vfs.h
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-unused-parameter -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined $< -Wl,--gc-sections -no-pie -o $@

.PHONY: ahci
ahci: build/test_ahci build/test_blk
	./build/test_ahci
	./build/test_blk

build/test_ahci: test_ahci.c ../../kernel/ahci.c ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

build/test_blk: test_blk.c ../../kernel/blk.c ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: ohci
ohci: build/test_ohci
	./build/test_ohci

build/test_ohci: test_ohci.c ../../kernel/usb_ohci.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: hub
hub: build/test_usb_hub
	./build/test_usb_hub

build/test_usb_hub: test_usb_hub.c ../../kernel/usb_hub.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: uhci
uhci: build/test_uhci
	./build/test_uhci

build/test_uhci: test_uhci.c ../../kernel/usb_uhci.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: fat-meta
fat-meta: build/test_fat_meta
	./build/test_fat_meta

build/test_fat_meta: test_fat_meta.c ../../kernel/fat_meta.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: ehci
ehci: build/test_ehci
	./build/test_ehci

build/test_ehci: test_ehci.c ../../kernel/usb_ehci.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-misleading-indentation -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: usb-layout
usb-layout:
	CC=$(CC) python3 test_usb_layout.py

.PHONY: user-space
user-space: build/test_user_space
	./build/test_user_space
build/test_user_space: test_user_space.c ../../kernel/user_space.c ../../include/user_space.h
	@mkdir -p build
	$(CC) $(FLAGS) -DKU_HOSTTEST -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_user_space.c ../../kernel/user_space.c -o $@

.PHONY: user-service
user-service: build/test_user_service
	./build/test_user_service
build/test_user_service: test_user_service.c test_user_space.c ../../kernel/user_space.c ../../kernel/user_surface.c ../../kernel/user_service.c ../../include/user_surface.h ../../include/user_space.h ../../include/user_service.h
	@mkdir -p build
	$(CC) $(FLAGS) -DKU_HOSTTEST -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_user_service.c ../../kernel/user_space.c ../../kernel/user_surface.c ../../kernel/user_service.c -o $@

.PHONY: user-image
user-image: build/test_user_image
	./build/test_user_image
build/test_user_image: test_user_image.c test_user_space.c ../../kernel/user_space.c ../../kernel/user_image.c ../../include/user_space.h ../../include/user_image.h
	@mkdir -p build
	$(CC) $(FLAGS) -DKU_HOSTTEST -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_user_image.c ../../kernel/user_space.c ../../kernel/user_image.c -o $@

../../build/mcc_user_blob.h: ../../sdk/isolated/mcc_runtime.c ../../sdk/isolated/mcc_runtime.lds ../../sdk/isolated/runtime_pack.py ../../sdk/isolated/kvuser.h ../../include/kvapp.h ../../sdk/isolated/Makefile
	$(MAKE) -C ../../sdk/isolated CC="$(CC)" runtime
build/libmcc.so build/libmcc64.so build/fuzz_mcc build/fuzz_mcc64: ../../build/mcc_user_blob.h

.PHONY: isolated sanitize-isolated
isolated: build/libmcc.so build/libmcc64.so build/test_user_image
	python3 test_isolated_target.py
sanitize-isolated: build/fuzz_mcc_isolated build/fuzz_mcc64_isolated
	./build/fuzz_mcc_isolated
	./build/fuzz_mcc64_isolated
build/fuzz_mcc_isolated: fuzz_mcc.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -DMCC_FUZZ_MODE=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie fuzz_mcc.c ../../kernel/mcc.c -o $@
build/fuzz_mcc64_isolated: fuzz_mcc.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_types.h ../../kernel/mcc_headers.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -DMCC_FUZZ_MODE=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie fuzz_mcc.c ../../kernel/mcc.c -o $@

.PHONY: includes
includes: build/test_mcc_includes build/test_mcc64_includes
	./build/test_mcc_includes
	./build/test_mcc64_includes
build/test_mcc_includes: test_mcc_includes.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_headers.h ../../kernel/mcc_types.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -DMCC_PP_TEST_HEADERS=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_mcc_includes.c -o $@
build/test_mcc64_includes: test_mcc_includes.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_headers.h ../../kernel/mcc_types.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -DMCC_PP_TEST_HEADERS=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_mcc_includes.c -o $@

.PHONY: rtc-cache
rtc-cache: build/test_rtc_cache
	./build/test_rtc_cache
build/test_rtc_cache: test_rtc_cache.c ../../kernel/rtc.c
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: budgets
budgets: build/test_mcc_budgets build/test_mcc64_budgets
	./build/test_mcc_budgets
	./build/test_mcc64_budgets
build/test_mcc_budgets: test_mcc_budgets.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_headers.h ../../kernel/mcc_types.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@
build/test_mcc64_budgets: test_mcc_budgets.c ../../kernel/mcc.c ../../kernel/mcc_pp.h ../../kernel/mcc_headers.h ../../kernel/mcc_types.h ../../include/mcc.h ../../build/mcc_user_blob.h
	$(CC) $(FLAGS) -DMCC_TARGET64=1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

.PHONY: filemgr
filemgr: build/test_filemgr_copy build/test_filemgr_rename
	./build/test_filemgr_copy
	./build/test_filemgr_rename
build/test_filemgr_copy: test_filemgr_copy.c ../../kernel/filemgr.c ../../kernel/vfs.c ../../include/filemgr.h ../../include/vfs.h
	@mkdir -p build
	$(CC) $(FLAGS) -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -Wl,--gc-sections -o $@

build/test_ramfs build/test_kvfs build/test_vfs_native: ../../include/native_name.h ../../include/kernel.h Makefile

build/test_filemgr_rename: test_filemgr_rename.c ../../kernel/filemgr.c ../../kernel/vfs.c ../../kernel/keymap.c ../../include/filemgr.h ../../include/vfs.h ../../include/native_name.h
	@mkdir -p build
	$(CC) $(FLAGS) -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< ../../kernel/keymap.c -Wl,--gc-sections -o $@

build/test_filemgr_copy build/test_filemgr_rename: ../../include/kernel.h ../../include/native_name.h ../../include/keymap.h Makefile

build/test_ramfs_commands: test_ramfs_commands.c ../../kernel/ramfs.c ../../kernel/utilcmds.c ../../include/kernel.h ../../include/vfs.h ../../include/native_name.h
	@mkdir -p build
	$(CC) $(FLAGS) -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -Wl,--gc-sections -o $@

build/test_store_format: test_store_format.c ../../kernel/gui_store_policy.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -o $@

# Full shell TU contains unrelated legacy paging casts; discard its unused sections.
build/test_shell_vcp: test_shell_vcp.c ../../kernel/shell.c ../../kernel/vfs.c ../../include/kernel.h ../../include/vfs.h Makefile
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -Wl,--gc-sections -o $@

build/test_shell_vls: test_shell_vls.c ../../kernel/shell.c ../../kernel/vfs.c ../../include/kernel.h ../../include/vfs.h Makefile
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -Wl,--gc-sections -o $@

build/test_ntfs_listing: test_ntfs_listing.c ../../kernel/ntfs.c ../../include/kernel.h ../../include/vfs.h Makefile
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-unused-parameter -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie $< -Wl,--gc-sections -o $@

.PHONY: procfs
procfs: build/test_procfs
	./build/test_procfs

build/test_procfs: test_procfs.c ../../kernel/procfs.c ../../kernel/sched.c ../../kernel/vfs.c ../../kernel/shell.c ../../kernel/printf.c ../../include/procfs.h ../../include/vfs.h ../../include/kernel.h
	@mkdir -p build
	$(CC) $(FLAGS) -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_procfs.c ../../kernel/printf.c -Wl,--gc-sections -o $@

.PHONY: user-surface
user-surface: build/test_user_surface
	./build/test_user_surface
build/test_user_surface: test_user_surface.c test_user_space.c ../../kernel/user_space.c ../../kernel/user_surface.c ../../kernel/user_service.c ../../include/user_space.h ../../include/user_surface.h ../../include/user_service.h
	@mkdir -p build
	$(CC) $(FLAGS) -DKU_HOSTTEST -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie test_user_surface.c ../../kernel/user_space.c ../../kernel/user_surface.c ../../kernel/user_service.c -o $@
