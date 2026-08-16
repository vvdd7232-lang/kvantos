# ============================================================
#  KvantOS SDK - сборка приложений .kapp
#
#  make            - собрать все примеры из apps/
#  make apps/имя.kapp - собрать одно приложение
#  make clean      - убрать за собой
#
#  Готовые .kapp кладутся в build/ и заодно в ../release/apps,
#  откуда их можно записать в образ диска.
# ============================================================
CC  := gcc
LD  := ld

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

# Флаги те же, что у ядра: свободностоящий код без libc.
# -fno-pic обязателен: приложение собрано под фиксированный адрес.
CFLAGS := -m32 -march=i586 -std=gnu11 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc \
          -Wall -Wextra -O2 -I../include -isystem $(GCC_INC)

LDFLAGS := -m elf_i386 -T kapp.ld -nostdlib

SRC  := $(wildcard apps/*.c)
APPS := $(patsubst apps/%.c,build/%.kapp,$(SRC))

.PHONY: all clean list

all: $(APPS)
	@echo ""
	@echo "  Готово. Приложения в sdk/build/:"
	@ls -1 build/*.kapp 2>/dev/null | sed 's|^|    |' || true

build:
	@mkdir -p build

# Каждое приложение - один файл .c, отдельный ELF, затем упаковка
build/%.elf: apps/%.c kapp.ld | build
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o build/$*.o
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ build/$*.o

build/%.kapp: build/%.elf mkkapp.py
	@python3 mkkapp.py $< $@ "$(shell sed -n 's/.*KAPP_NAME[ \t]*"\(.*\)".*/\1/p' apps/$*.c | head -1)"
	@mkdir -p ../release/apps && cp $@ ../release/apps/

list:
	@echo "Доступные приложения:"
	@ls -1 apps/*.c 2>/dev/null | sed 's|apps/|  |; s|\.c$$||'

clean:
	@rm -rf build
	@echo "  очищено"
