# M3.5b — экспериментальные HS-only хабы EHCI

Продолжение M3.5a, не новый публичный релиз. Поддерживается ограниченное дерево
**high-speed хабов с high-speed потомками**. Full-/low-speed устройства за EHCI
пока отключаются до обращения к их EP0: TT/split НЕ реализованы.

## Изменения и ограничения

* Убран не общий запрет, а добавлена отдельная проверяемая HS-ветка:
  HS hub class/subclass, USB2 version, EP0 MPS64, device/interface protocols,
  единственный активный hub interface/interrupt endpoint, длины дескрипторов.
* Single-TT device protocol 1 / interface protocol 0 и multi-TT capability
  device protocol 2 / default interface protocol 1 распознаются. Остаёмся в
  **alternate 0**; не включаем multi-TT alternate 1 и не используем ни один TT.
  Protocol хранится в прежнем padding topology record, без изменения disk layout.
  Соответствие протоколов: [2](https://community.infineon.com/t5/Knowledge-Base-Articles/How-to-Check-Whether-a-USB-2-0-Hub-is-Multi-TT-or-Single-TT-KBA93279/ta-p/258747).
* HS hub descriptor читается сначала как 7-byte header, затем полностью;
  проверяются согласованность header, стандартные bitmap lengths и characteristics.
  Старый FS QEMU 8-port/10-byte вариант остаётся допустим только в FS-ветке.
* Скорость определяется не только в начале: после reset и при каждом live
  ancestor GET_STATUS требуется HIGH_SPEED=1, LOW_SPEED=0 для EHCI.
  UHCI/OHCI по-прежнему требуют FS. До TT нельзя отправлять HS qTD к FS/LS.
  Отказавший порт disable с подтверждением; невозможность изоляции fence-ит HC.
* До переиспользования descendant addresses отключаются **все root links**,
  теперь включая EHCI. Потом reset hub и disable всех siblings до enum children.
  Root addresses зарезервированы, downstream allocation общий для каждого HC.
* Старые depth=4, hubs=8, ports<=15, USB disks=8, addresses<=127 сохранены.
  Ancestor-first проверки, sticky edge refusal, generation/lease/cache fencing
  и fresh RO после rescan не ослаблены. EHCI frame waits используют M3.5a budgets.
* Сообщение USB status теперь различает общее количество хабов и HS subset.
  DMA-пулы, ASS retirement, no-copy/no-toggle on failure и reboot-only HC fence
  остаются из M3.5a. Потеря HC может отключить всех его siblings.

## Что именно считается доказательством

### 1. Host model — actual production topology code

`make -C tests/mcc CC=gcc-12 hub` включает `kernel/usb_hub.h` непосредственно,
под ASan/UBSan. Сохранены прежние FS tests, descriptor mutation/truncation,
лимиты и global-address collision checks. Добавлены вложенные HS single/multi
protocol descriptors (50 scans, только alternate 0), live speed loss до query
нижнего хаба, sticky refusal и восстановление только новым epoch.
FS/LS/invalid-speed children не получают ни одного EP0 запроса в модели;
проверяются bad device/version/protocol/interface/interrupt/header/bitmap cases.
Модель control bus не моделирует электрическую USB и PCI/IOMMU.

### 2. Отдельная QEMU HS-only fixture — не штатный hub и НЕ TT

Оригинальный QEMU `usb-hub` — FS-only, поэтому он не способен квалифицировать
новую HS-ветку. Для интеграционной проверки есть маленький patch к **QEMU 10.0.0**:
`tests/boot/fixtures/qemu-10.0.0-hs-hub.patch`. Он добавляет явный тип
`kvantos-lab-hs-hub`, не меняет FS capability штатного типа `usb-hub` и не
подменяет установленный QEMU. Настоящие kernel/EHCI/qTD/BOT/FAT/VFS исполняются
в VM; хаб, speed bits и HS address forwarding — тестовая эмуляция.

Fixture рекламирует USB2 single-TT descriptor, чтобы тестировать корректный
HS descriptor path, **но TT engine в ней отсутствует**. Нет split execution,
USB2 wire timing, physical late-DMA/PHY/электрической квалификации. Это не
доказательство полного USB2 hub compliance. Multi-TT descriptors проверены
только host model, не физическим multi-TT хабом или этой single-TT fixture.

Первый fixture запуск обнаружил, что FS-класс QEMU не переключает descriptor
на HS при attach: исправлено `handle_attach=usb_desc_attach` в LAB-классе.
Negative mouse fixture явно задаёт `usb_version=1`: default QEMU mouse тоже
умеет HS и не подходит для доказательства отказа FS. Эти исправления относятся
к тестовой модели, не обходят driver guards.

### 3. Штатный QEMU

Прежние UHCI/OHCI FS topology suites и EHCI root-port/import/isolation выполняются
отдельно, без LAB hub patch. Они проверяют регрессии, но не HS hubs/TT.
Локальные результаты, команды и логи перечислены в доставленном пакете M3.5b.
Remote CI не запускался; host `hub` target автоматически включает новые cases.

## Финальные локальные результаты

Все финальные parent-процессы завершились с exit 0. Обычный NORMAL ISO,
без product/lab hooks; один и тот же ISO SHA используется во всех прогонах.

| HS-only LAB, два уровня хабов | BIOS32 | UEFI64 |
|---|---|---|
| 3 диска / 2 хаба, 12 rescans, уникальные адреса и fresh RO | PASS | PASS |
| Leaf/ancestor/root unplug, cached stale reads, reconnect | PASS | PASS |
| Отказ USB1.x child, HS siblings живы, source SHA неизменны | PASS | PASS |
| Полный GUI import suite + native Build/Run, >128 строк, UTF-8 | PASS | PASS |
| Один клик: HUGE.C ~18 KiB | 0.889 s | 0.647 s |
| Один клик: owned FAT Save As 65535 B | 5.033 s | 4.063 s |
| Save/replace/cancel/LFN/cold import/fsck | PASS | PASS |
| GUI commit cut + unplug, документ/Undo сохранены, cold recovery | PASS | PASS |
| WAL smoke: RO/opt-in, CRUD/disable/cold RO, cut/EIO/RO/reconnect | PASS | PASS |

Это wall time конкретного QEMU/TCG LAB прогона, не физическая скорость и не
сравнение с прежними root/FS измерениями. Дедлайны не увеличены ради PASS.
WAL `smoke` — один `stx_commit` cut и один FAT2-EIO, не full fault matrix.

Штатный QEMU 10.0.13: UHCI и OHCI nested topology на обоих ядрах, EHCI root GUI
import и два независимых EHCI с debugger-timed detach — PASS. BIOS CD/floppy и
UEFI reached shell; release image/header/direct/installed payload/SHA checks PASS.
Host ASan/UBSan suites и compiler 73+73, FAT cache/performance/batch/eviction
(793 boundaries, **52 выбранных** cuts, 12 batch read-EIO) PASS.

Независимая сборка из свежей копии: оба ядра, EFI и 22 приложения exact.
Для побайтного сравнения заданы исходные per-TU `SOURCE_DATE_EPOCH` для шести
объектов GUI/shell/utilcmds (два backend): иначе разные TIME strings по-разному
объединяются linker string pooling и меняют layout. Старые objects не копируются;
явные timestamp inputs и журнал пересборки включены в proof. Это не утверждение,
что произвольная сборка в другое время без этих inputs даст тот же SHA.

## Воспроизведение HS-only LAB

Нужны GCC12/NASM/GRUB/QEMU/OVMF, mtools/dosfstools, Pillow; для сборки QEMU также
GLib/Pixman development files, Python modules meson/ninja/distlib, curl/patch.
129-MiB QEMU source archive проверяется pinned SHA-256. Сборка одноразовая:

```sh
make -j2 CC=gcc-12 release
bash tests/boot/build-hs-qemu.sh
export QEMU_SYSTEM_X86_64="$PWD/build/hs-qemu/build/qemu-system-x86_64"
export USB_HS_LAB=1
USB_CONTROLLER=ehci python3 tests/boot/usb-hub-smoke.py
export USB_HUB_DEPTH=2
USB_CONTROLLER=ehci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt \
  python3 tests/boot/save-as-smoke.py
STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt OWNED_PHASE=smoke \
  python3 tests/boot/fat-owned-smoke.py
```

Без `MCC_ARCH64` guest suites запускают обе архитектуры. Для stock regressions
убрать `QEMU_SYSTEM_X86_64`, `USB_HS_LAB`, `USB_HUB_DEPTH`. HS helper требует и
явного opt-in, и реально доступного LAB device type; штатный FS hub нельзя
незаметно выдать за HS. EHCI+companion fixture использует root port 3 согласованно
и для цепочки хабов, и для leaf storage.

## Дальше

TT/split — отдельная реализация speed/TT routing, split scheduling/completion,
TT error cleanup и single/multi-TT qualification. Затем физические HS hubs,
VirtualBox passthrough и companion handoff. HS-only fixture не закрывает эти пункты.
Default RO, явный prepared-FAT32 opt-in, WAL/COW/recovery-before-namespace и
запрет автоматического форматирования/GPT repair сохранены. GeneralUDisk не
открывался. App ABI/редакторские лимиты не менялись; >128 строк — предупреждение.
