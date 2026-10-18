# M3.4a — ограниченные full-speed USB-хабы

Рабочая ветка: `feature/storage-m3`, продолжение M3.3. Публикация, tag и push
не входят в этот шаг. Этот документ фиксирует **исторический промежуточный M3.4a**:
на момент его доставки transport/topology прошли, но большие GUI-файлы — нет.
GUI blocker закрыт последующим [M3.4b](STORAGE-M3.4B.md), где приведены текущие
замеры и восемь полных GUI suites. Физическая приёмка по-прежнему открыта.

## Поддерживаемый путь

* USB 1.1 **full-speed, 12 Мбит/с**, через UHCI или OHCI.
* До 8 хабов суммарно, 4 хабов в цепочке, 15 портов на хаб, 8 BOT-дисков,
  4 host controllers. Каждый адрес находится в диапазоне 1…127.
* Один общий путь BOT/SCSI → block → FAT32/NTFS; FAT32 default RO и прежний
  explicit opt-in + owned file WAL, recovery-before-publication. NTFS — RO.
* Не используются IRQ хабов: состояние нужного пути опрашивается синхронными
  class control requests. Поэтому работа через цепочку хабов заметно медленнее
  root-port; максимальная скорость и время загрузки с WAL не заявлены.
* Для восьмипортового USB 1.1 дескриптора допускается укороченная неиспользуемая
  `PortPwrCtrlMask` (QEMU: 10 вместо 11 байт). Число портов, границы и минимальный
  размер проверяются; ни одна из битовых карт не используется как адрес памяти.

**Не реализованы:** high-speed hubs, EHCI TT/split transactions, low-speed
потомки, xHCI, HID, UAS, носители с сектором не 512 байт. USB 2.0 хаб, работающий
в high-speed режиме, не поддерживается только потому, что за ним FS-флешка.
Нужен именно full-speed upstream хаба и UHCI/OHCI. Совместимость реальных USB
2.0 хабов в FS fallback и EHCI companion routing требует отдельной проверки.

## Адресация и повторное сканирование

`port+1` оставлен только для **зарезервированных root-адресов** каждого HC.
Потомки получают уникальные адреса из `hc.ports+1…127`, общий allocator на HC.

Rescan создаёт новый ненулевой epoch, очищает диски, кэш, хабы и leases. Перед
первым новым адресом выполняется **общий проход отключения всех FS root links**,
а не reset/enumeration одного root при ещё доступных старых соседних деревьях.
Reset upstream хаба возвращает его downstream links в начальное состояние;
после configuration все его порты дополнительно явно отключаются и проверяются
**до** адресации любого ребёнка. Далее порты сбрасываются и адресуются строго
последовательно. При reset сначала квитируются начальные события, но новое
connection/suspend/overcurrent изменение во время reset — отказ, а не повод
стереть свидетельство замены.

Неудачная enumeration обязана отключить соответствующий root/downstream link:
неадресованный ответчик на address zero не должен остаться видимым при следующем
reset. Если отключение downstream link нельзя подтвердить, HC quarantined:
списки остановлены, PCI bus mastering выключен. Повторного использования живого
address zero при такой ошибке нет. Переполнение пула/глубины/адресов означает
отказ ветке, а не wrap, запись за массив или снятие ограничений.

## Живой путь и старые подключения

В `usb_disk_t` сохраняется настоящий root port для HCD и цепочка `parent` /
`downport`. Перед/после каждого BOT-командного обмена проверяются:

1. Root link, change latches и состояние контроллера.
2. Путь **от корня к листу**, с проверкой HC, root, epoch, глубины и номеров.
3. Hub global status/change, затем port connection/enable/power, отсутствие
   suspend/reset/overcurrent, LS/HS и change flags.

Недоступный предок не адресуется через заведомо неверный путь. Плохой порт
защёлкивается в `blocked` до rescan; его reconnect не оживляет старый mount.
Потеря питания/глобальное изменение хаба блокирует всё его поддерево. Независимые
соседние порты не блокируются намеренно.

Проверка выполняется и для **попадания в sector cache**, и перед выдачей FAT
write lease. При отказе поколение и разрешение записи сбрасываются, кэш больше
не выдаётся. Число секторов/epoch — дешёвые metadata queries: проверяют root и
уже известные блокировки, но сами не посылают USB control transfers. Это не I/O
и не разрешение на запись; настоящий read/write/flush и lease всегда проходят
активную проверку. Без rescan новые устройства автоматически не публикуются.

Как и M3.3, активная транспортная ошибка OHCI может fenced весь HC до reboot;
прозрачное восстановление любого active-unplug не обещается. Idle unplug,
отказ старому namespace и явный rescan — отдельные проверяемые сценарии.

## UHCI DMA

Прежние общие TD/data заменены статическими private TD/data на опубликованный
HC. Очередь снимается, два кадра должны подтвердить retirement. Если frame clock
остановлен, HC quarantined, его слот и память не используются другим HC.
Адреса PCI сохранены также для UHCI, чтобы отказ мог отключить bus mastering.
Модель выполняет настоящий `usb_uhci.h`, включая late completion в старую память.
Это модель DMA, а не квалификация физического контроллера/IOMMU.

## Воспроизведение

```sh
make -j2 CC=gcc-12 STORAGE_LAB=0 release
make -C tests/mcc uhci ohci hub ahci storage vfs editors input ramfs check

# По умолчанию обе архитектуры; MCC_ARCH64=0 / 1 выбирает одну.
USB_CONTROLLER=ohci python3 tests/boot/usb-hub-smoke.py
USB_CONTROLLER=uhci python3 tests/boot/usb-hub-smoke.py

USB_CONTROLLER=ohci USB_LAYOUT=gpt USB_HUB_DEPTH=1 MCC_BOOT_TIMEOUT=120 \
  python3 tests/boot/fat-owned-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=mbr USB_HUB_DEPTH=1 MCC_BOOT_TIMEOUT=120 \
  python3 tests/boot/fat-owned-smoke.py
USB_CONTROLLER=ohci USB_LAYOUT=gpt USB_HUB_DEPTH=1 MCC_BOOT_TIMEOUT=120 \
  GUI_SAVE_OWNED=1 python3 tests/boot/save-as-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=mbr USB_HUB_DEPTH=1 MCC_BOOT_TIMEOUT=120 \
  python3 tests/boot/usb-import-smoke.py
```

Все образы одноразовые regular files; физические диски не открываются. У обычных
root suites `USB_HUB_DEPTH=0` по умолчанию: прежние пути и бюджеты сохраняются.
Для больших GUI write через hub бюджет одного измеряемого клика увеличен
пропорционально глубине. Повторные слепые клики не используются.

Host topology tests: 181 rescan одного дерева с глобальным поиском адреса на
модельной шине; roots + nested siblings, sticky edge, ancestor-first refusal,
независимость соседей, power/change/HC/epoch/cycle/port limits, descriptor walk
с truncation/mutations под ASan/UBSan, ограничения depth/pool/127 адресов,
ошибка изоляции и отказ EHCI хабу. Это не заменяет реальные guest tests.

Guest topology test: два хаба, три различных диска (включая native root), 12
rescan, uniqueness всех адресов хабов/дисков, отключение leaf/промежуточного/
root хаба без предварительного OS rescan, stale cache refusal, reconnect и
независимый SHA256 целых исходных образов.

CI содержит native UHCI/MBR и OHCI/GPT × BIOS32/UEFI64. Удалённый CI этим локальным
шагом не запускается. Итоговые фактически выполненные проверки и хэши сборки
фиксируются в отчёте поставки; наличие CI job не считается его прохождением.

## Открытая аппаратная приёмка

Physical hubs/UHCI/OHCI, VirtualBox passthrough, EHCI companion FS handoff,
USB 2.0 FS fallback, питание/overcurrent реального оборудования и нестандартные
hub quirks не квалифицированы. Общая M2 hardware acceptance остаётся открытой.
Ни `GeneralUDisk`, ни таблицы разделов пользовательских устройств не менялись.

## Локальный объём WAL-проверок (не путать с настройкой CI)

BIOS32: native UHCI/MBR и OHCI/GPT — основной owned-WAL набор, включая три
power-cut точки, реальные EIO/RO, failed recovery, invalid owner/header,
unplug/reconnect и full filesystem. Исходные all-suite процессы остановились
на старом 60-секундном бюджете последнего full-space теста. Отдельное измерение
на неизменном ядре: корректный отказ за 71.342 с, затем cold fsck/namespace PASS.
Бюджет **только** полного сканирования теперь `60*(depth+1)` с; root остаётся 60 с.

`OWNED_PHASE=full` воспроизводит только этот заключительный сценарий;
`OWNED_PHASE=main` — основной набор без full-space; `all` — прежний полный набор
и значение по умолчанию. Для UEFI64 в этом шаге выбран `OWNED_PHASE=smoke`:
normal/default RO/opt-in/write/mkdir/remove/cold, commit power cut, FAT2 EIO,
RO backend и unplug/rescan. Это **не** вся UEFI матрица corruption/recovery/full
filesystem. Полная конфигурация CI оставлена, но удалённо не запускалась.
Итоговый отчёт различает исходные failed parents и отдельно завершённые фазы,
а не выдаёт составные доказательства за успешный all-suite parent.

## Известный блокер: большие файлы через GUI

Первый OHCI/BIOS32 Save As 65535 байт превысил бюджет одного измеренного клика
120 с; UHCI/BIOS32 импорт `HUGE.C` (около 18 КиБ) превысил стандартные 8 с.
Повторных слепых нажатий/подбора координат не делали. Это **не PASS** GUI-suite;
ни на UEFI64, ни на других хабах успешная большая GUI-операция не заявляется.
Те же product kernels отдельно измеряются через консоль, без мыши.

Активный опрос предков на каждом BOT/sector-cache доступе делает повторные
FAT metadata/chain проходы дорогими. Следующий шаг — сокращение повторных FAT
операций/укрупнение безопасных I/O, с сохранением epoch/port-change/cache/lease
защит. Простое увеличение всех таймаутов или отключение проверок не считается
исправлением. Новый CI hub job оставлен как приёмочный барьер, не как зелёный
результат. EHCI TT/split не начинается вместо устранения этого блокера.

### Фактически подтверждённая базовая часть

Native UHCI/MBR и OHCI/GPT: оба ядра прошли реальную nested/root топологию с
двумя хабами и тремя дисками, 12 rescan, уникальность всех адресов, stale cache
refusal при leaf/ancestor unplug, независимость соседей и неизменность полных
RO образов. BIOS main WAL + отдельный full-space и UEFI transport smoke описаны
выше; это не успешные all-suite parents. Консольная OHCI-запись 65535 байт прошла
за 119.598 с, затем cold RO, независимое точное чтение и fsck PASS.

Такая длительность — реальное UX-ограничение, а не повод закрыть GUI-приёмку
увеличением ожиданий. Этап остаётся в ROADMAP незавершённым. Отчёт поставки
отдельно перечисляет успешные root/SATA/boot регрессии и неуспешные hub GUI suites.
