#!/bin/bash
# ============================================================
#  KvantOS - вложить приложения в загрузочный образ
#
#  Самый простой способ доставить .kapp в систему: положить его
#  внутрь ISO. Диск, сеть и флешка не нужны - программа окажется
#  в системе сразу после загрузки.
#
#  Использование:
#      ./sdk/addapp.sh моя_игра.kapp [ещё.kapp ...]
#
#  Итог: release/kvantos.iso и release/kvantos-floppy.img,
#  в которых лежат ваши приложения.
# ============================================================
set -e

cd "$(dirname "$0")/.."          # корень проекта

if [ $# -eq 0 ]; then
    echo "Использование: ./sdk/addapp.sh файл.kapp [файл2.kapp ...]"
    echo
    echo "Приложения будут вложены в загрузочный образ."
    echo "Сейчас в образе:"
    ls -1 release/apps/*.kapp 2>/dev/null | sed 's|release/apps/|  |' || echo "  (пусто)"
    exit 1
fi

mkdir -p release/apps

for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "  ОШИБКА: файл '$f' не найден"
        exit 1
    fi
    # Проверяем подпись: чтобы не вложить случайно не тот файл
    sig=$(head -c 4 "$f")
    if [ "$sig" != "KAPP" ]; then
        echo "  ОШИБКА: '$f' не приложение KvantOS (нет подписи KAPP)"
        echo "  Соберите его через sdk/mkkapp.py"
        exit 1
    fi
    name=$(basename "$f")
    cp "$f" "release/apps/$name"
    echo "  + $name ($(stat -c%s "$f") байт)"
done

# grub.cfg перечисляет модули поимённо - перегенерируем этот список
python3 - <<'PY'
import glob, os, re

apps = sorted(os.path.basename(p) for p in glob.glob("release/apps/*.kapp"))
lines = "".join(f"    module /boot/apps/{a} {a}\n" for a in apps)

cfg = open("grub/grub.cfg").read()
# убираем прежние строки module и вставляем свежие после каждого multiboot
cfg = re.sub(r"^ *module /boot/apps/.*\n", "", cfg, flags=re.M)
cfg = re.sub(r"(    multiboot /boot/kvant\.bin(?: text)?\n)(?!.*safe)",
             lambda m: m.group(1) + lines, cfg)
open("grub/grub.cfg", "w").write(cfg)
print(f"  grub.cfg: {len(apps)} приложений в меню")
PY

echo "  Пересборка образов..."
touch kernel/main.c
make >/dev/null 2>&1
rm -f build/kvantos.iso
make iso  >/dev/null 2>&1
make floppy >/dev/null 2>&1

echo
echo "  ГОТОВО."
echo "    release/kvantos.iso          - записать на диск/флешку или подключить в VMware"
echo "    release/kvantos-floppy.img   - запасной вариант"
echo
echo "  В системе: guimenu -> клавиша G -> ваша программа в списке."
