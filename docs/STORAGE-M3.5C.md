# M3.5c — экспериментальный EHCI FS async TT

Продолжение M3.5a/b, не аппаратная квалификация USB 2.0 и не завершение всей TT-задачи.
Обычное ядро, без продуктовых test hooks. Новая возможность: FS BOT-накопитель
непосредственно под HS-хабом; перед этим хабом допустима цепочка HS-хабов.
Работа идёт через EHCI, не через companion. Physical/VBox остаются OPEN.

## Реализация и ограничения

- Скорость хранится отдельно от вида контроллера: FS=0, HS=2 (кодировка EHCI).
  `speed` занимает бывший хвостовой padding `usb_disk_t`: NORMAL 40/48 байт
  i386/x64, LAB 48/56. Compile-only проверка **реальных** production typedefs
  проверяет также HC/Hub и offset; runtime multilib не нужен.
- После reset скорость берётся из статуса конкретного порта. При проверке
  цепочки каждая связь сравнивается со скоростью своего непосредственного
  потомка: FS leaf не превращает все вышестоящие HS-связи в FS.
- TT адрес/физический порт выводятся из непосредственного родителя. Проверяются
  HC/root port, адрес 1..127, epoch, protocol 1/2, живость и блокировка порта.
  Неправильный маршрут отказывается до DMA, не останавливая другой контроллер.
- Async QH: EPS=0, C=1 только для EP0, действительный MPS, hub address/port,
  Mult=1, S/C masks=0, RL=0. HS сохраняет EPS=2, прежние MPS и RL=8.
  FS EP0 и bulk принимают только 8/16/32/64. Начальный FS EP0 MPS=8;
  после первых восьми байт descriptor используется объявленный допустимый MPS.
- **SS/CS исполняет аппаратный EHCI**, а не программный цикл отправки USB токенов.
  ПО задаёт QH, начинает с Do-Start-Split и проверяет итог после снятия ASS.
  Остаточный SplitXState в qTD или QH при объявленном успехе считается
  неопределённым завершением: IN caller/toggle не публикуются, HC fencing.
- Успешное qTD writeback само по себе не разрешает переиспользование DMA:
  ASS retirement, текущий qTD, overlay, длина, error bits и root link проверяются
  как раньше. Private DMA pool каждой попытки init остаётся отдельным.
- Таймаут/ошибка/неопределённое завершение TT **останавливает весь этот EHCI до
  перезагрузки**, BME выключается, pool не переиспользуется. Это намеренно теряет
  и HS-соседей того же HC; другой HC независим. Это НЕ CLEAR_TT_BUFFER,
  не автоматическое восстановление TT и не IOMMU-защита.
- FS-хабы под EHCI не поддержаны: class-9 device отказывается после первых
  восьми байт descriptor, **до SET_ADDRESS/SET_CONFIGURATION**. LS, HID,
  periodic/interrupt/isochronous splits, multi-TT alternate 1 не реализованы.
  FS непосредственно в EHCI root / integrated root TT не добавлены; companion
  handoff остаётся отдельной незакрытой аппаратной задачей.
  Protocol-2 HS-хаб остаётся в default alt0/single-TT organization;
  это не проверка независимого планирования нескольких TT.
- Адресная изоляция, ancestor-first sticky guards, generation/cache invalidation,
  default RO, prepared-FAT32 opt-in и WAL/recovery-before-namespace сохранены.
  Лимиты depth=4/hubs=8/ports=15/disks=8/addresses=127 не расширены.

## Что проверяет модель, а что она НЕ доказывает

Actual-header host tests моделируют SS NAK/ACK, CS NYET/результат, вечный busy/NYET,
ошибки, disconnect, ASS и некорректный SplitXState. Проверяются неизменные caller
и toggle при отказе, запрет дальнейшего HS/FS replay на fenced HC, размеры/MPS,
маршрутизация FS leaf через вложенный HS hub и оба hub protocols в alt0.
Native guard suite дополнительно наблюдает live QH: неверный TT route намеренно
инжектируется изменением QH через debugger; отдельный case удаляет FS device
при active SplitXState. Проверяются отказ, целый прежний RAM destination и
чтение через другой HC. Это fault injection, не естественная аппаратная ошибка.
Это register/DMA model, не USB-анализатор и не независимая сертификация EHCI.

Штатный QEMU 10.0.0 не содержит полноценного async split engine (§4.12 TODO).
Поэтому создан **отдельный** `kvantos-lab-async-tt-hub`, с явными опциями тестов.
Cumulative pinned-source patch включает предыдущий HS fixture, не меняя тип
штатного `usb-hub` или поведение прежнего HS-only LAB по умолчанию.
Новая модель:

1. Заставляет выбранный leaf port согласовать настоящий FS descriptor/MPS.
   Вложенные хабы в линейной цепочке сохраняются HS.
2. На FS передачах проверяет реального непосредственного родителя/порт,
   EPS/C/MPS/Mult/S-C masks QH. Неверный QH не должен молча пройти forwarding.
3. Разделяет TD-level SS ACK, CS NYET и результат, задерживая результат до
   следующего миллисекундного шага FRINDEX; SplitXState меняется в QH.
   Trace `usb_ehci_lab_tt` включается только для explicit TT LAB тестов.

**Это TD-level функциональная модель.** Она всё ещё передаёт USBPacket обычному
эмулированному устройству; не моделирует каждый FS packet/split token, TT wire
buffer, USB 12-Mbit/s bandwidth, microframe arbitration, электрические ошибки,
реальный single/multi-TT contention или migration. Скорость GUI здесь нельзя
выдавать за физическую производительность FS. Даже положительные host+guest
проверки не закрывают физический TT пункт roadmap.

## Матрица этого preview

Результаты и точные границы приведены также в `VALIDATION.txt` поставки.

| Проверка | Результат |
|---|---|
| i386/x64 release + actual-header ASan/UBSan | PASS |
| NORMAL/LAB ABI layout, обе архитектуры | PASS |
| TT LAB: FS leaves под HS / вложенным HS, mixed HS root, rescans/unplug | PASS ×2 |
| TT LAB: импорт/Блокнот/мастерская/native Build/Run | PASS ×2 |
| TT LAB: owned FAT32 GUI Save As 65535 байт/отказы/recovery | PASS ×2; первый параллельный x64 отказ — см. ниже |
| TT LAB: scoped WAL smoke, RO/opt-in/CRUD/cut/EIO/reconnect | PASS ×2 (22 VM cases), не full fault matrix |
| TT LAB: неверный QH route и disconnect при active SplitXState, два HC | PASS ×2 |
| Stock UHCI/OHCI FS hubs, EHCI HS roots/isolation, boot/compiler | PASS ×2, compiler 73+73 |
| Независимая сборка исходников | 25 artifacts byte-exact с явными timestamp inputs |
| Physical USB/VBox/companion handoff/TT wire/full multi-TT | **OPEN** |

### Известное наблюдение: первый параллельный x64 Save As

Первый x64 прогон при одновременно запущенных USB regression suites вернул
ошибку большого сохранения. Последний TD в trace имеет SS ACK/CS NYET без
результата; **причина не установлена**. Не называем это исправленной ошибкой,
не списываем без доказательств на нагрузку/железо и не закрываем нагрузочную
TT-квалификацию. Bounds и модель не ослаблялись.

Снимок редактора подтверждает сохранность всех 65535 байт и отсутствие ложной
привязки target. Состояние приватного носителя, trace и исходный failure log
сохранены в proof. Отдельная диагностическая попытка и затем полный одиночный
x64 GUI suite прошли, включая fsck и commit-cut/recovery. Это не доказывает,
что первоначальный отказ больше не повторится.

Дополнительно **точный носитель после первого отказа** восстановлен холодным
NORMAL ядром отдельно на BIOS32 и UEFI64: старая namespace целиком, fsck clean,
default RO восстановлен, вне FAT-раздела SHA не изменился. Это воспроизводит
recovery конкретного on-disk состояния, **не воспроизводит timing-причину отказа**.
Для этого в proof есть сжатый приватный fixture и метаданные:

```sh
python3 tests/boot/ehci-tt-recovery-smoke.py /path/to/proof/initial-failure
```

Поэтому статус M3.5c — **экспериментальный функциональный preview с открытым
наблюдением**, не стабильный релиз и не всесторонняя TT/write квалификация.

WAL smoke — не полная матрица power cuts; записи только в приватные regular-file
fixtures. GeneralUDisk и физические накопители не открывались. Этот preview
не повод включать запись на реальном важном USB-носителе.

## Воспроизведение

```sh
make -j2 CC=gcc-12 release
make -C tests/mcc CC=gcc-12 usb-layout hub ehci uhci ohci
bash tests/boot/build-tt-qemu.sh
export QEMU_SYSTEM_X86_64="$PWD/build/async-tt-qemu/build/qemu-system-x86_64"
export USB_HS_LAB=1 USB_TT_LAB=1
USB_CONTROLLER=ehci python3 tests/boot/usb-hub-smoke.py
python3 tests/boot/ehci-tt-guard-smoke.py
export USB_HUB_DEPTH=2
USB_CONTROLLER=ehci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt \
  python3 tests/boot/save-as-smoke.py
STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt OWNED_PHASE=smoke \
  python3 tests/boot/fat-owned-smoke.py
```

Без `MCC_ARCH64` запускаются BIOS32 и UEFI64. Убрать четыре LAB/topology env
переменных для stock regressions. QEMU helper проверяет официальный архив
SHA-256 `22c075601fdcf8c7b2671a839ebdcef1d4f2973eb6735254fd2e1bd0f30b3896`;
требования сборки те же, что у HS helper. Для поставленной проверки использована
инкрементальная сборка того же pinned source/configure; отдельный новый helper
проверен синтаксически и patch replay, но не выдаётся за свежий end-to-end run.

Независимая сборка не использует старые objects. При exact-сравнении сохранены
шесть явных per-TU SOURCE_DATE_EPOCH предыдущей сборки (GUI/shell/utilcmds ×2),
чтобы timestamp string pooling не менял layout. Это не обещание одинакового
SHA при произвольных времени и окружении.

## Основания и следующие шаги

EHCI Revision 1.0: §3.6.2/3.6.3 (QH), §4.12.1 (async split state machine),
§4.10 (retirement); [спецификация](https://0x04.net/~mwk/doc/usb/ehci-r10.pdf).
QEMU v10.0.0 `hw/usb/hcd-ehci.c`, `ehci_execute_complete` содержит
[TODO splits](https://github.com/qemu/qemu/blob/v10.0.0/hw/usb/hcd-ehci.c).
Linux v6.12 `drivers/usb/host/ehci-q.c`: независимая сверка FS QH полей и того,
что error/unlink может оставить TT buffer в неопределённом состоянии;
[исходник](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/ehci-q.c).

Дальше отдельно: физический FS BOT через HS single-TT/alt0, controller-specific
errata и disconnect в split window; затем корректный CLEAR_TT_BUFFER и bounded
recovery вместо reboot-only fence. Полные multi-TT alternate/periodic/LS — новые
этапы, не часть этой поставки. Стабильный релиз, push/tag/publication не выполняются.
