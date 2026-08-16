#!/usr/bin/env python3
# ============================================================
#  KvantOS SDK - упаковщик приложений
#
#  Превращает собранный ELF в файл .kapp: вырезает готовый
#  образ памяти и приписывает спереди 64-байтный заголовок,
#  который читает загрузчик ядра.
#
#  Запуск:
#     python3 mkkapp.py вход.elf выход.kapp "Название"
# ============================================================
import struct
import subprocess
import sys

LOAD_BASE      = 0x00E00000     # обязан совпадать с KAPP_LOAD_BASE
API_VERSION    = 1
FORMAT_VERSION = 1
HEADER_SIZE    = 64
MAX_SIZE       = 0x00200000     # 2 МиБ
FLAG_WINDOW    = 0x0001


def fail(msg):
    print(f"  ОШИБКА: {msg}", file=sys.stderr)
    sys.exit(1)


def run(*args):
    """Вызов утилиты binutils с внятной диагностикой."""
    try:
        return subprocess.run(args, capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        fail(f"не найдена программа {args[0]} (поставьте binutils)")
    except subprocess.CalledProcessError as e:
        fail(f"{args[0]} завершилась с ошибкой:\n{e.stderr}")


def symbol_address(elf, name):
    """Адрес символа из таблицы ELF."""
    for line in run("nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("Использование: mkkapp.py вход.elf выход.kapp [\"Название\"]")
        sys.exit(1)

    elf, out = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else out.rsplit("/", 1)[-1].replace(".kapp", "")

    # --- точка входа ---
    entry = symbol_address(elf, "kapp_main")
    if entry is None:
        fail("в приложении нет функции kapp_main - её обязан определить каждый .kapp")

    # --- границы образа ---
    file_end  = symbol_address(elf, "__kapp_file_end")
    bss_start = symbol_address(elf, "__bss_start")
    bss_end   = symbol_address(elf, "__bss_end")
    if file_end is None or bss_start is None or bss_end is None:
        fail("не найдены метки размещения - собирайте с sdk/kapp.ld")

    # --- вырезаем готовый образ памяти ---
    run("objcopy", "-O", "binary",
        "--set-section-flags", ".bss=alloc,load,contents",
        elf, out + ".tmp")
    with open(out + ".tmp", "rb") as f:
        blob = f.read()

    code_size = file_end - LOAD_BASE
    if code_size <= 0:
        fail("пустой образ: в приложении нет кода")
    if len(blob) < code_size:
        fail(f"образ короче ожидаемого ({len(blob)} < {code_size})")
    blob = blob[:code_size]

    bss_size = bss_end - bss_start
    if code_size + bss_size > MAX_SIZE:
        fail(f"приложение больше 2 МиБ ({(code_size + bss_size) // 1024} КиБ)")

    if not (LOAD_BASE <= entry < LOAD_BASE + code_size):
        fail(f"точка входа 0x{entry:08X} вне образа - неверный линкер-скрипт?")

    # --- заголовок ---
    nm = name.encode("utf-8")[:31]
    header = struct.pack(
        "<4sHHIIIIII32s",
        b"KAPP",
        FORMAT_VERSION,
        HEADER_SIZE,
        API_VERSION,
        FLAG_WINDOW,
        LOAD_BASE,
        entry,
        code_size,
        bss_size,
        nm,
    )
    assert len(header) == HEADER_SIZE, f"заголовок {len(header)} байт вместо {HEADER_SIZE}"

    with open(out, "wb") as f:
        f.write(header + blob)

    import os
    os.remove(out + ".tmp")

    total = len(header) + len(blob)
    print(f"  KAPP {out}")
    print(f"       название : {name}")
    print(f"       точка входа: 0x{entry:08X}")
    print(f"       код+данные : {code_size} байт")
    print(f"       bss        : {bss_size} байт")
    print(f"       файл       : {total} байт ({(total + 1023) // 1024} КиБ)")


if __name__ == "__main__":
    main()
