# M3.5a — безопасная DMA-база EHCI перед HS hubs / TT

Локальное продолжение M3.4b, **не публичный релиз и не готовая поддержка TT**.
Исходная база восстановлена побайтно из доставленного M3.4b и зафиксирована
локальным `ab918a6`. Этот шаг меняет EHCI root-port transport, не формат FAT/WAL.

## Что изменено

* Вместо общего bounce-буфера и единственного qTD — отдельные periodic list,
  QH, qTD и 4096-байтный bounce на каждую **попытку** инициализации EHCI.
  Успешные и неуспешные попытки получают монотонные leases: нельзя выбрать
  память по индексу `hcs`, поскольку неудачный init позволяет повторно занять
  этот индекс другим контроллером. Четыре leases до перезагрузки; исчерпание
  пула — безопасный отказ, не автоматическое освобождение старого DMA.
* При init сначала firmware handoff и остановка/reset старого engine,
  затем публикация собственных DMA-структур и разрешение bus mastering.
  Проверяются тип/верхняя половина BAR, MMIO mapping, HCIVERSION/CAPLENGTH/ports,
  bounded legacy-capability chain, DMA32/alignment/contiguous page spans.
* Синхронный guard на каждый HC; валидация PID, endpoint, MPS, toggle, port,
  адреса, длины и буфера. Для HS root EP0 остаётся MPS64, для bulk — MPS512.
  Direct DMA в caller memory нет: SETUP/OUT сначала копируются в private bounce.
* До повторного заполнения QH/qTD обязательно `ASE=0`, затем подтверждение
  `ASS=0`. После завершения qTD это выполняется ещё раз: одного снятия Active
  у qTD недостаточно для доказательства retirement кэшируемого QH.
* После retirement проверяются текущий qTD в QH, terminated next, отсутствие
  Active/error в qTD и overlay, согласованный bounded остаток. Повторно
  проверяются root port и latched changes; есть проверка гонки до запуска DMA.
  Только после этого копируется IN и меняется caller toggle. Short IN/ZLP
  учитывают число пакетов, включая завершающий short packet; short OUT — ошибка.
* Register/frame/firmware polling ограничен и временем TSC, и числом итераций.
  Ошибка, malformed completion, timeout/stop failure или потеря порта блокируют
  HC до перезагрузки: interrupt/schedules off, запрос снятия PCI bus mastering.
  Память остаётся pinned даже при зависшем engine/позднем DMA. Автоматического
  повторения неопределённой записи, восстановления HC или reuse его lease нет.

Ограничение принципиально: fencing одного HC может отключить **все его диски**.
Изоляция другого PCI HC проверяется отдельно. Это не IOMMU и не защита от
произвольного вредоносного PCI DMA; неисправное железо может не выполнить
MMIO/PCI stop-записи. Нельзя обещать бесшовное переподключение fenced HC.

## Доказательства и их границы

### Host model, actual backend

`tests/mcc/test_ehci.c` включает непосредственно `kernel/usb_ehci.h` и собирается
под ASan/UBSan с низкими адресами `-no-pie`. Это модель MMIO/DMA, не другой драйвер.

Проверены control/SETUP и bulk IN/OUT, ZLP, длины 0…512 по каждому байту и далее
кратные 512 до 4096; short IN с нулём, границами пакета и untouched caller tail;
реальное ожидание deferred QH retirement, toggles, busy и invalid inputs.
Ошибки: stall, babble, buffer, transaction, missed microframe, timeout, detach,
latched change, HSE; invalid current/next/overlay/remaining, несогласованные
остатки, short OUT, stuck ASS off, отсутствие ASS on и гонка до issue.
Ошибка не меняет caller/toggle, не перезапускает команду, снимает разрешение DMA.

Отдельно: два HC, ошибочный init с последующим reuse **слота hcs**, но не DMA;
симулированная поздняя запись в quarantined pool; exhausted leases; BAR64 с
нулевой/ненулевой верхней половиной, capability/ownership/reset/start/map/DMA
отказы; BIOS handoff, cyclic capability chain, DMA overflow/alignment/page gaps.
С замороженным TSC и остановленными кадрами waits всё равно завершаются по
iteration budgets. Хостовая модель не доказывает выполнение PCI stop железом.

### Реальные QEMU HS root-контроллеры

`tests/boot/ehci-isolation-smoke.py`: BIOS32 и UEFI64, по два `usb-ehci` и два
HS `usb-storage` на **root ports**, без hub и companion. Оба показывают 480 Mb/s.
Внешний debugger ставит breakpoint на обычную `ehci_async_off` между проверкой
порта и программированием передачи; затем QEMU действительно удаляет USB0.
Нет product/lab hooks, изменения guest RAM или изменения исходников драйвера.
После resume: HC0 fenced, прежний RAM-файл не заменён неудачной копией, HC1 жив
и читает >16 KiB. Все исходные образы побайтно неизменны по SHA-256.
Это квалифицирует именно гонку и **разные** HC, не независимость siblings.

Дополнительные прогоны обычного ISO используют GPT/FAT32 и только disposable
regular-file images. Финальные журналы и результаты находятся в пакете M3.5a.
Старые полные M3.4b matrices остаются историческим доказательством той версии;
здесь не заявляется повторный запуск всей прежней fault matrix.

## Итог локальной проверки

Все перечисленные ниже финальные процессы завершились с exit 0.

| Проверка | BIOS32 | UEFI64 |
|---|---|---|
| Два независимых EHCI, debugger-timed detach | PASS | PASS |
| Полный GUI USB import suite: GPT + raw/diskless, 12 rescans, native Run | PASS | PASS |
| GUI import HUGE.C (~18 KiB), один клик | 0.278 s | 0.281 s |
| GUI owned FAT Save As 65535 B, один клик | 0.576 s | 0.615 s |
| Save/replace/cancel/LFN/UTF-8, cold readback/fsck | PASS | PASS |
| GUI commit cut + unplug, документ/Undo сохранены, cold recovery | PASS | PASS |
| Owned WAL `smoke`: RO/opt-in, write/mkdir/delete/disable, cold RO | PASS | PASS |
| `smoke`: stx_commit power cut, FAT2 EIO, read-only SCSI, reconnect | PASS | PASS |

Замеры — wall time конкретного QEMU/TCG прогона **HS root port**, не сравнение
с прежними FS hub timings и не обещание скорости физической флешки. Дедлайны
и схема одного measured click не увеличивались ради результата.
`smoke` — явно сокращённая fault matrix: один commit cut и один write-EIO,
не прежняя полная M3.4b матрица всех cut points/контроллеров.

ASan/UBSan: EHCI и прежние AHCI/OHCI/UHCI/hub/BOT/block/VFS/sector-WAL,
редактор/RAM/input/FAT metadata models PASS. Compiler/codegen: 73 + 73 PASS.
FAT performance и dirty metadata eviction: 793 boundaries, **52 выбранных**
cuts, batch listing и 12 read-EIO cuts PASS; это не exhaustive 793 cuts.
Независимая сборка из свежей копии всех 355 source files: оба ядра совпали
после нормализации четырёх строк DATE/TIME суммарно; EFI и все 22 приложения
совпали побайтно. Финальная пересборка ISO обновляет GRUB/ISO timestamps,
но не эти payloads; boot и two-HC isolation повторяются на финальном контейнере.
GRUB BIOS CD/floppy и UEFI достигли shell; image checks: headers, installed
BIOS, direct payloads и 11 apps на архитектуру/SHA PASS. Remote CI не запускался;
EHCI model и two-HC test добавлены в workflow для последующих запусков.

## Что осталось неизменным

Default RO; подготовленный owned-file WAL и явный `fatwrite VOLUME enable`;
generation/route/cache/lease guards; recovery до namespace; COW и no-replay.
Нет автоматического форматирования, provisioning, GPT repair или GPT writes.
GeneralUDisk и физические устройства не открывались. Публичные ABI и редакторские
лимиты не менялись: >128 строк — предупреждение, не запрет редактирования/сохранения.

## Дальше — не обходить отказы hub/split

1. Отдельно реализовать и проверить HS hub enumeration, speed/protocol metadata,
   уникальные адреса и ancestor/lifetime guards для HS descendants.
2. Затем добавить TT routing для FS/LS descendants, split scheduling/completion,
   single/multi-TT и отказные случаи. Не просто удалить текущий EHCI hub reject.
3. Найти подходящий backend/физический HS hub для квалификации. Штатный QEMU
   `usb-hub` — full-speed USB 1.1: он не проверяет HS hub/TT/split. Исходник:
   https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/usb/dev-hub.c
4. Физические USB/VirtualBox, real companion handoff, HID/LS/xHCI и IOMMU —
   отдельные незакрытые пункты. Публичный push/tag/release требует согласования.

## Воспроизведение

```sh
make -j2 CC=gcc-12 release
make -C tests/mcc CC=gcc-12 ehci
python3 tests/boot/ehci-isolation-smoke.py
USB_CONTROLLER=ehci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt \
  python3 tests/boot/save-as-smoke.py
STORAGE_CONTROLLER=ehci-ohci STORAGE_LAYOUT=gpt OWNED_PHASE=smoke \
  python3 tests/boot/fat-owned-smoke.py
```

Без `MCC_ARCH64` перечисленные guest scripts запускают обе архитектуры.
ISO должен быть NORMAL, без STORAGE_LAB. Нужны QEMU, GRUB/NASM, OVMF,
dosfstools/mtools, Pillow для GUI. Для compiler/codegen tests также Unicorn;
в локальной среде он установлен в `/usr/bin/python3`, не default Python.
Результаты QEMU/модели не заменяют аппаратную приёмку.
