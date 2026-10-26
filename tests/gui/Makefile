CC ?= gcc
ROOT := ../..
OUT := $(ROOT)/build/gui-visual
CFLAGS := -std=gnu11 -O1 -g -fno-builtin -ffunction-sections -fdata-sections \
          -I$(ROOT)/include -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
          -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie
SOURCES := render.c $(ROOT)/kernel/string.c $(ROOT)/kernel/printf.c \
           $(ROOT)/kernel/charset.c $(ROOT)/kernel/i18n.c \
           $(ROOT)/kernel/font8x16.c $(ROOT)/kernel/fontaa.c
.PHONY: check clean
check: $(OUT)/render
	$(OUT)/render $(OUT)
	python3 png.py $(OUT)
$(OUT)/render: $(SOURCES) $(wildcard $(ROOT)/kernel/gui*.h) $(ROOT)/kernel/gui.c $(ROOT)/kernel/fb.c $(ROOT)/include/kernel.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SOURCES) -Wl,--gc-sections -fsanitize=address,undefined -no-pie -o $@
clean:
	rm -rf $(OUT)
