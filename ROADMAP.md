# ROADMAP — KvantOS

Рабочий план после **3.1.3**, ветка `feature/storage-m3` (**3.1.4-dev**). Статус: экспериментальная ОС, не для единственной
копии важных данных. Пункты закрываются кодом и воспроизводимыми тестами,
а не обещаниями. Релизы 3.1.3 и старше не переписываем.

## Правила работы

* BIOS/i386 и UEFI/x86-64 сохраняются на каждом этапе.
* Каждая новая возможность получает host-тесты и/или QEMU-проверку.
* Успех в QEMU не выдаём за проверку физического устройства или VirtualBox.
* Работа с пользовательскими дисками — только после тестов на отдельных образах.
* Не оцениваем написание драйвера как «30 минут». Публикация — отдельное действие
  после проверок, без автоматического push из рабочей ветки.

## M1. Редакторы и защита от регрессий — кандидат в 3.1.4

### M1.1. Первый исполняемый шаг

- [x] Многошаговая история обоих редакторов: независимые Undo/Redo, кнопки и
  Ctrl+Z/Ctrl+Y, новая правка отбрасывает только ветку redo.
- [x] Правильный признак несохранённых изменений: возврат к сохранённой версии
  снимает `*`, уход от неё возвращает `*`; ошибка сохранения не меняет точку.
- [x] История ограничена памятью, а не полными копиями документа на каждый символ;
  вытеснение старых записей не блокирует редактирование и не портит текст.
- [x] Подготовлен workflow для каждого PR: сборка обоих ядер, модель редактора под ASan/UBSan,
  RAMFS/OOM, структура образов и загрузка BIOS + UEFI.

**Проверено локально:** история/ветвление/переполнение/UTF-8/сохранённая версия,
25000 случайных правок под ASan/UBSan; кнопки обоих редакторов, Ctrl+Z/Y,
открытие файла, RAM/KvFS, NumPad и 640×480 в QEMU на обеих архитектурах.
Workflow обновлён в рабочей ветке; нового удалённого запуска GitHub Actions
пока нет. Это первый законченный шаг, не завершение всего M1.

### M1.2. Ввод, поиск и сохранение импорта

- [x] Русская раскладка: явный переключатель EN/RU, отображение состояния,
  UTF-8-ввод в редакторы; команды оболочки и горячие клавиши не ломаются.
- [x] Поиск вперёд/назад и замена: сначала один результат, затем «заменить всё»
  одной отменяемой операцией. Ошибка лимита не меняет документ.
- [x] Постоянно различать «изменено», «сохранено в RAM», «сохранено на KvFS».
- [x] После импорта предложить «Сохранить на KvFS», не писать автоматически
  и не обещать сохранение при отсутствии диска. Это не требует FAT32-записи.

**Проверено локально:** ASan/UBSan для раскладки (100000 scan-байтов),
3000 дифференциальных замен и прежние тесты истории/лимитов; QEMU BIOS + UEFI:
ЙЦУКЕН, ё/Ё/№, Caps/Shift, Alt+Shift и EN/RU, Ctrl-сочетания и латинские имена;
поиск в обе стороны с переходом через край, замена одного/всех, одна отмена,
640×480, отказ по лимиту без изменения текста, RAM-only и полный каталог KvFS.
После USB-импорта кнопка «На KvFS» пишет только по явному нажатию; отсутствие
диска не объявляется сохранением. Источники GPT/FAT32 EHCI+OHCI проверены
неизменными по SHA-256. Прежние mouse-first/MouseKeys/редакторы также проверены
на обеих архитектурах. BIOS-установка `--keep` на одноразовый образ загружается
с диска без CD. Новый Actions workflow подготовлен, но удалённо не запускался.

Ограничения M1: поиск буквальный, чувствителен к регистру; оба поля до 127 байт
UTF-8, без regex. Раскладка действует на текст двух редакторов и поиск, а не
переводит ABI приложений, оболочку или имена файлов на Unicode. Статус хранения
относится к версии документа, а не обещает crash-safety или мониторинг всех
внешних записей. Запись FAT32/USB всё ещё отключена.

## M2. Безопасная запись на FAT32-флешку

- [x] M2.1: аудит KvFS: новый extent до переключения каталога, отказ без потери
  старого файла при нехватке места, валидация метаданных. Неопределённый исход
  записи каталога блокирует mount до переподключения, а не пытается опасный rollback.
  Cache-flush ошибки ATA теперь учитываются. Это не гарантия против torn sectors.
- [x] M2.1: USB BOT/SCSI WRITE(10), SYNCHRONIZE CACHE(10), ограниченные ожидания,
  ошибки/REQUEST SENSE, инвалидация кэша и запрет повторной записи после сбоя.
  Проверено **только в lab-сборке** на явно маркированной scratch-области образа;
  в обычной сборке USB-запись закрыта, FAT/VFS-тома USB остаются RO.
- [x] M2.2a: FAT COW и ограниченный rollback метаданных; прототип постоянного
  undo-журнала на явно выделенной области **host-образа**, 1048 обрывов + fsck.
  В обычных ядрах только RAM undo (не защита от выключения); USB остаётся RO.
- [x] M2.2b: реальные FAT/VFS операции через USB и recovery до публикации
  namespace в QEMU lab (UHCI/MBR, EHCI+OHCI/GPT, оба ядра); поколения устройств,
  раздельные FAT/WAL-разрешения и отказ при повреждённом журнале. Не для реальных носителей.
- [x] M2.4: запись FAT32 через общий VFS и журнал в выделенном обычном файле;
  учтены выделение кластеров, несколько FAT,
  каталог, свободное место, замену файла и сбой между этапами операции.
- [x] M2.3a: общее «Сохранить как → Том / папка» в двух редакторах,
  VFS-назначение, подтверждение замены, явные RAM/RO/ошибки и защита старого
  назначения после rescan. Обычные USB по-прежнему RO; FAT-запись — только lab.
- [x] M2.4: программные тесты на одноразовых образах: пустой/многокластерный файл,
  заполненный том, ошибочный сектор,
  отключение во время записи, повторное подключение и проверка внешним fsck.

**M2.4 закрывает оставшуюся программную часть, но не аппаратную приёмку M2.**
Обычный ISO понимает журнал в заранее подготовленном `KVANTWAL.SYS`, проверяет
его исключительное владение по обеим FAT и дереву каталогов через undo-overlay,
восстанавливает незавершённую операцию **до публикации тома**. Свободные области
диска/разделов и GPT не используются. По умолчанию **все FAT-тома RO** (в том
числе прежние ATA/FAT16); только подготовленный FAT32 можно явно включить
`fatwrite ИМЯ_ТОМА enable` на текущий сеанс. RAM/KvFS работают как прежде.
Восстановление разрешено самим фактом явной предварительной подготовки журнала,
а новые сохранения требуют отдельного opt-in после каждого rescan/перезапуска.
Безопасная подготовка выполняется отдельной Linux-утилитой без форматирования;
обычный kernel никогда не создаёт/не сбрасывает журнал автоматически.

Проверки владения, полного тома, обрывов, ошибки/RO, повторной загрузки и внешнего
fsck выполняются на **одноразовых образах**, включая обычный USB/VFS/GUI путь,
не только lab. Настоящая флешка и VirtualBox аппаратно не проверены.
- [ ] Отдельная приёмка на физическом носителе: подготовка с резервной копией,
  Save/полное выключение/чтение другой ОС, отключение/переподключение и fsck.
- [ ] Проверка VBox с реальным USB passthrough; OHCI-only/xHCI не заявлены.

Ограничения и обязательный exclusive-media договор: [docs/STORAGE-M2.md](docs/STORAGE-M2.md).
После аварии **сначала recovery в KvantOS**, не запись другой ОС/ремонт fsck:
обычный FAT-драйвер чужой ОС не понимает наш WAL. Dirty-bit — подсказка, не
межсистемная блокировка. Некорректный owner/WAL блокирует том, а не сбрасывается.
CI дополнен normal-owned матрицей; удалённого запуска этой ветки не было.

**Готово, когда:** файлы читаются после полной перезагрузки, внешняя проверка
не находит повреждений, а прерванная операция не объявляется успешной.
Сначала одноразовые образы; запись на реальные флешки включать отдельно.
GPT-заголовки при сохранении файла изменять не нужно. Ремонт GPT не требуется.
NTFS пока оставить RO, явно показывая это в UI.

Аппаратная приёмка M2 отложена пользователем до доступа к тестовому носителю;
переходим к M3, не отмечая эти два пункта выполненными.

## M3. Совместимость накопителей и USB — отдельные этапы

- [x] M3.1: PCI AHCI, передача владения от BIOS, остановка firmware DMA,
  IDENTIFY и DMA-чтение 512-байтных SATA через общий block/VFS, raw/MBR/GPT,
  импорт в оба редактора, таймаут/ошибки/карантин порта. Только RO.
- [x] M3.2 (программный этап): AHCI-запись/FLUSH EXT с отдельным разрешением,
  общим owned WAL и recovery. Оба ядра × MBR/GPT: реальные power cuts/EIO,
  GUI flush timeout/late completion, fsck; совместная SATA/USB запись и warm reset.
- [ ] Аппаратная приёмка AHCI/SATA отдельно; установка на AHCI и hotplug
  пока не поддержаны. Снятие RO в M3.2 не означает эту приёмку.

**M3.1:** локальные ASan/UBSan register/DMA-model и QEMU ICH9 AHCI: оба ядра,
два SATA-порта + IDE/KvFS, обычный FAT32/VFS/GUI, холодная загрузка, read EIO,
полные SHA источников и fsck. Смешанный SATA RO + USB WAL RW проверяется отдельно
для UHCI/MBR и EHCI+OHCI/GPT. Подробности M3.1: [docs/STORAGE-M3.md](docs/STORAGE-M3.md); продолжение записи:
[docs/STORAGE-M3.2.md](docs/STORAGE-M3.2.md).
Программная проверка не означает физическую SATA/VirtualBox-квалификацию.
- [x] M3.3 (программный этап): отдельный OHCI full-speed HCCA/ED/TD backend,
  общий BOT/WAL. Оба ядра × MBR/GPT: power cuts/EIO/RO/full/recovery и reconnect;
  GUI/импорт/нативный Run, смешанные SATA + UHCI/OHCI/EHCI, warm reset, fsck.
  [Границы и воспроизведение](docs/STORAGE-M3.3.md). OHCI не переименованный UHCI.
- [ ] Физическая OHCI/VirtualBox-приёмка и реальный EHCI→companion FS handoff;
  программная матрица не закрывает аппаратные пункты.
- [x] M3.4a (программный этап): full-speed USB-хабы UHCI/OHCI, уникальные
  адреса, bounded topology и ancestor/generation/cache/write-lease fencing.
  [Топология и scoped WAL](docs/STORAGE-M3.4A.md); [M3.4b](docs/STORAGE-M3.4B.md):
  operation-local FAT/FSInfo, batch directory/GPT reads; все восемь полных
  hub GUI/import suites BIOS32/UEFI64 PASS, 65535 байт и >128 строк без блокировки.
  Физические хабы, full UEFI fault matrix и EHCI TT/split этим не квалифицированы.
- [x] M3.5a (программный prerequisite): EHCI private DMA per init attempt,
  ASS retirement before copy/toggle, bounded stop/frame failures и reboot-only
  fencing. Два реальных QEMU HS-контроллера: отключение первого в гонке не
  останавливает второй; это не IOMMU/физическая квалификация.
  [Границы, проверки и следующий шаг](docs/STORAGE-M3.5A.md).
- [x] M3.5b (экспериментальная программная HS-only ветка): EHCI high-speed
  hubs с HS потомками, protocol/bitmap/speed guards и global root isolation.
  Host actual-code model + отдельная QEMU HS-only fixture; это не physical/TT
  qualification. [Границы и воспроизведение](docs/STORAGE-M3.5B.md).
- [x] M3.5c (экспериментальная программная подзадача): FS BOT непосредственно
  под HS hub, async TT QH/routing/completion и per-edge speed; host SS/CS model
  + отдельная TD-level QEMU LAB fixture. Ошибка/неопределённый TT: весь этот HC
  fenced до reboot, НЕ CLEAR_TT_BUFFER. [Границы](docs/STORAGE-M3.5C.md).
- [x] M3.5d: bounded fault snapshots/probes, независимые clocks модели и
  4/4 cold recovery архивных slow fixtures через OHCI. Это диагностика, не TT
  recovery. [Checkpoint](docs/M35D-DIAGNOSTICS.ru.md),
  [safety design без WRITE retry](docs/TT-RECOVERY-SAFETY.ru.md).
- [ ] Установить причину первого параллельного x64 TT Save As отказа: одиночный
  полный прогон и recovery сохранённого состояния PASS, но наблюдение OPEN.
- [ ] Полная EHCI TT/split квалификация: physical/VBox, TT cleanup/recovery,
  FS hubs, LS/periodic и multi-TT alternate scheduling отдельно. M3.5c не
  закрывает wire/timing/электрическую приёмку и этот полный пункт.
- [ ] Физические full-speed хабы / VBox passthrough / companion FS handoff.
- [ ] xHCI — после стабилизации общего USB-интерфейса; NVMe — после AHCI.

**Готово для каждого драйвера:** загрузка/чтение/запись там, где заявлены,
таймаут при зависшем устройстве, повторный запуск, отсутствие регрессий старых
контроллеров. AHCI и OHCI не объединять в одну «среднюю» задачу.

## M4. Изоляция приложений — архитектурная линия

- [x] M4.1: проект пользовательского ABI/syscalls без прямых kernel pointers:
  [контракт и gates](docs/M4-ISOLATED-ABI.ru.md), затем M4.2–M4.13 implementation.
- [x] Opt-in ABI4: Ring3/private memory, user/kernel stacks, checked buffers (BIOS/UEFI).
- [x] ABI4 exceptions/resource quotas/cleanup без падения desktop; native fault suite PASS.
- [x] ABI4 quantum resume и остановка бесконечного цикла; single-GUI dispatcher, не SMP.

**Software proof:** см. docs/M4.10–M4.15, opt-in Workshop target. Legacy ABI2/3
остаются ring0; physical qualification этим не закрыта.

**Готово, когда:** нулевой/чужой указатель, попытка записи в ядро и бесконечный
цикл завершают приложение, но ОС остаётся управляемой. Один watchdog без
изоляции и безопасного прерывания этого не обеспечивает.

## M5. Native-компилятор — по полезности

- [x] M5.1: фиксированные одномерные массивы и `sizeof` в ограниченном C.
  Literal extent, без VLA/инициализаторов/whole-array pointers; результат sizeof — int,
  не size_t. [Границы подмножества](sdk/WORKSHOP.ru.md#поддерживаемое-подмножество-c).
  Остальные формы массивов/ISO C type system не заявлены.
- [x] M5.2: bounded object-like `#define`, token boundaries/recursion/depth/output
  guards; [ограничения и проверки](docs/M5.2-PREPROCESSOR.ru.md).
- [x] M5.3: bounded structs/typedef, поля/указатели/массивы, размеры/выравнивание
  на обеих архитектурах; без aggregate-by-value ABI.
  [Границы и проверки](docs/M5.3-TYPES.ru.md).
- [x] M5.4: readonly virtual-header `#include`, whitelist3/include-once/depth4/
  cycle/work/output guards; оба targets, native BIOS32/UEFI64 PASS.
  [Контракт](docs/M5.4-VIRTUAL-HEADERS.ru.md), не filesystem/full CPP.
- [x] M5.5: [review source/table budgets](docs/M5.5-COMPILER-BUDGETS.ru.md),
  точные границы/guard pages/диагностика под ASan/UBSan; source16000 сохранён.
  Увеличение бюджетов — отдельный будущий increment.
- [ ] `float/double` — после FPU-контекста, ABI и math tests.
  [Safety gates](docs/FPU-CONTEXT-SAFETY.ru.md); M4.15 проверяет default deny,
  включая inherited OSXSAVE, но не включает FP context.

## M6. Другие направления, не блокирующие M1

- [ ] Сеть: выбрать один NIC, сначала link/ARP/ICMP, затем DHCP и TCP/HTTP.
- [ ] Звук AC97/HDA. PC-speaker и `beep` уже существуют, не считать их новой задачей.
- [ ] Установка `.kapp` USB → KvFS с проверкой формата, архитектуры и подтверждением.
- [x] M6.1: [copy safety](docs/M6.1-FILEMANAGER-COPY-SAFETY.ru.md), exact read/paths/
  epochs/uncertain outcomes, host faults + native mouse RAM→KvFS PASS.
- [ ] Менеджер файлов: rename/move с отдельной recovery/confirmation policy;
  cross-volume atomic move и streaming copies не реализованы.
- [ ] Secure Boot: отдельная модель доверия, ключи пользователя и проверка цепочки;
  не обещать, что одной подписи EFI достаточно для всех компьютеров.

## Репозиторий и сопровождение

- [ ] Сравнить PR #1/#2 с текущим кодом, забрать полезное, затем закрыть с объяснением.
  Одинаковое название функции не означает, что PR целиком устарел.
- [x] CONTRIBUTING, issue/PR templates и [матрица qualification](docs/QUALIFICATION-MATRIX.ru.md).
  [CI job и local/hosted границы](docs/MAINTENANCE-CI.ru.md); hosted run не заявлен.
- [x] `testshots/index.html` помечен Linguist-generated; PNG/галерея не удалены.
- [ ] Бейдж CI и topics; галерея — вспомогательная задача, не замена тестам.
- [ ] Quantum уже описан в CHANGELOG. Исторический тег обсуждать отдельно,
  не изображать задним числом новый протестированный релиз.

## Что уже есть в 3.1.3 — не начинать заново

USB UHCI/EHCI, чтение GPT/FAT32, импорт мышью, сохранение импорта на KvFS,
предупреждение после 128 строк, NumPad MouseKeys, PC-speaker API и PR-сборка.
Ограничения остаются реальными: редактор <64 КиБ и 120 байт/строка,
native-компилятор <=16000 байт исходника, legacy-приложения ring0, ABI4 opt-in,
USB/GPT writes ограничены отдельно проверенными opt-in/WAL paths, не общим разрешением.

### M6.2–M6.3 software increments

Listing/namespace-bound double-click identity and no-replace RAMFS/KvFS/VFS
rename delivered; full host + BIOS32/UEFI64 native regression PASS. Backend
rename is available via existing mv/rename, not yet a mouse dialog. FAT/NTFS
rename and cross-volume copy-delete remain unsupported. Details in
`docs/M6.2-FILEMANAGER-CLICK-IDENTITY.ru.md` and
`docs/M6.3-NATIVE-NOREPLACE-RENAME.ru.md`; next M6.4 mouse modal.

### M6.4 mouse rename

Rename and new-folder name modal delivered with Apply/Cancel, original path,
open-time pane/listing/namespace binding, no-replace backend and UTF-8 RU input.
Host + BIOS32/UEFI64 native, including 640×480 and real metadata EIO, PASS;
backend/Workshop/CPU/SDK regressions PASS. See
`docs/M6.4-MOUSE-RENAME-MODAL.ru.md`. Cross-volume move/FAT/NTFS rename stay open.

### M6.5–M6.6 integrity and truthful storage state

RAM shell cp now COW/no-predelete/samepath no-op. Unmounted KvFS is no longer
reported as an empty/unformatted disk. Programs offers explicit read-only
validation/mount and separate destructive-format consent; Enter cannot erase.
Raw native remount/format/fence invalidates VFS generation without rescan.
Full host + GPT/owned WAL, BIOS32/UEFI64 GUI/Workshop/CPU/SDK and 640×480 PASS.
See `docs/M6.5-SHELL-RAM-COPY-SAFETY.ru.md` and
`docs/M6.6-KVFS-STATE-AND-FORMAT.ru.md`. Physical/TT/FPU/move remain open.

### M6.7 listing namespace

Per-pane namespace-at-load, mixed/error/malformed listing discard, stale rows/
actions refusal, bounded preview and delete-confirmation snapshots delivered.
Full host and BIOS32/UEFI64 production native, including 640×480 and real
prepublication delete metadata EIO, PASS. See
`docs/M6.7-FILEMANAGER-LISTING-NAMESPACE.ru.md`. Legacy adapters may still hide
unavailable as EOF after an explicit Refresh; truthful status is a separate
follow-up, not physical hotplug/atomic multiwriter qualification.

### M6.8 shell VFS copy

Actual shell vcp short-read overwrite regression reproduced RED and fixed.
Bounded paths/CWD/directory target, exact read/write, namespace, same-string
no-op and early RO refusal delivered without source removal or blind retry.
Host/sanitizers/GPT/owned-WAL and native BIOS32/UEFI64 vcp/EIO/cold persistence,
RAM cp/mv, mouse copy, Workshop/CPU/SDK PASS. See
`docs/M6.8-SHELL-VFS-COPY.ru.md`; aliases/multiwriter/hardware stay unqualified.

### M6.9 truthful directory listing

EOF/error and per-call namespace/count checks delivered across VFS adapters
and shell/GUI consumers. Unavailable KvFS no longer becomes empty on Refresh.
Host/sanitizers/GPT/owned-WAL/FAT-perf and BIOS32/UEFI64 native listing/state/
namespace/vcp/Workshop/CPU/SDK, listing at 640×480 PASS. See
`docs/M6.9-TRUTHFUL-LISTING-STATUS.ru.md`. Hostile NTFS parser qualification
and physical hotplug remain open.

### M6.10 bounded shell operations

Shared checked resolver replaces truncating shell path construction. vcat requires
exact bounded read and unchanged namespace before displaying; vrm/vmkdir report
uncertain outcomes after namespace fence; vcd preserves cwd on invalid/stale path.
Host/full regression and BIOS32/UEFI64 real KvFS read/delete EIO PASS. See
`docs/M6.10-BOUNDED-SHELL-PATHS.ru.md`.

### M6.11 bounded NTFS index parser

Bounded index/key/UTF16/attribute/runlist/fixup checks and explicit parser errors;
ASan/UBSan 50k decoder +20k metadata mutations, full host/GPT/owned-WAL/FATperf,
release/layout and BIOS32/UEFI64 non-NTFS storage/application regressions PASS.
NTFS parser fixture is synthetic, no real NTFS image/hardware claim. See
`docs/M6.11-BOUNDED-NTFS-INDEX-PARSER.ru.md`.

### M6.12 NTFS sector geometry — software increment complete

RED reproduced 4KiB logical sectors passed as one 512-byte block. Added logical-to-
512-byte block translation, volume/partition/device-LBA overflow fences and checked
MFT/cluster arithmetic. Actual adapter 512/4096 synthetic fixtures, ASan/UBSan parser
mutations, full host/storage/GPT/WAL/FAT-perf/release/layout, and BIOS32/UEFI64
shell/storage/CPU/Workshop regressions PASS; GCC 12 is required here because GCC 14
hit a compiler ICE in 64-bit `main.c`. This does not qualify real NTFS interoperability
or physical 4Kn. See `docs/M6.12-NTFS-SECTOR-BOUNDS.ru.md`.

### M6.13 limited actual NTFS image qualification — software complete

Private regular-file images produced by ntfs-3g at 512/1024/2048/4096 logical
bytes per sector pass independent ntfscat + production host block/VFS read-only
adapter: root listing, exact file bytes, denied write/remove/mkdir, zero write
callbacks and unchanged image SHA. The 4Kn image exposed USA fixup stride=512 bytes
(not BPB logical sector size); production parser and 4KiB USA tests corrected.
Host compiler/sanitizer/storage/GPT/WAL/perf, GCC12 release/layout and BIOS32/UEFI64
shell/storage/CPL3/Workshop regression all PASS. No host mount, loop or physical I/O.
Scope is a clean ntfs-3g test volume with one simple file, not arbitrary NTFS
or hardware compatibility. M6.14 separately qualifies MBR offset bounds. See
`docs/M6.13-NTFS-IMAGE-QUALIFICATION.ru.md` and
`docs/M6.14-NTFS-PARTITION-OFFSETS.ru.md`.

### M6.14 NTFS MBR partition offsets — software complete

Production autoscan mounts actual ntfs-3g images embedded in a private MBR disk at
512-byte LBA 2048 for 512/4096 logical-sector sizes; VFS preserves start LBA,
listing/read/RO/zero-write/full-image SHA PASS. A partition whose extent ends one
block beyond its image is refused before NTFS mount, without raw alias or OOB reads.
No `/dev`, host mount or physical I/O. GPT offset, complex NTFS metadata and hardware
remain OPEN; see `docs/M6.14-NTFS-PARTITION-OFFSETS.ru.md`.

### M6.15 nonresident file + Unicode name — software complete

Private ntfs-3g images contain a 1MiB Unicode root file; `ntfscat` confirms exact
bytes and `ntfsinfo` verifies `$DATA Resident: No` before the actual adapter test.
Raw 512/1024/2048/4096 and MBR-offset 512/4096 cases pass exact read/list, RO,
zero-write and full-image SHA invariants. GPT22/owned-WAL 1696 interruption cases/
FAT-perf pass. The file is freshly allocated; deliberate fragmentation, nested
Unicode directories and arbitrary NTFS metadata are not qualified. See
`docs/M6.15-NTFS-NONRESIDENT-UNICODE.ru.md`.

### M6.16 NTFS partial-offset and EOF reads — software complete

Actual ntfs-3g image harness now checks nonzero-offset reads across sector/cluster
boundaries, bounded short reads from the final 17 bytes, and zero-byte reads at
and beyond EOF. PASS for resident and 1MiB nonresident files across raw 512/1024/
2048/4096 logical-sector images and MBR-offset 512/4096 wrappers; RO/no-OOB/zero-
write/full-SHA checks remain. This is only tested on the simple clean ntfs-3g
fixture shapes; no arbitrary or fragmented NTFS claim. See
`docs/M6.16-NTFS-PARTIAL-READ-BOUNDS.ru.md`.

### M6.17 fragmented stream and Initialized Size — bounded software complete

A private ntfs-3g image now proves a true two-run, 1MiB nonresident stream and
Initialized Size=4096. Its allocated but uninitialized tail is poisoned with
0xA5: independent ntfscat and the read-only production adapter both return zeroes,
without writes or image mutation. This exposed/fixed an NTFS initialized-tail
readout bug in `kernel/ntfs.c`. Raw M6.15 geometry/MBR matrix plus GPT22,
owned-WAL1696/FAT-perf, MCC VFS/parser ASan/UBSan, actual NTFS image ASan/UBSan,
and GCC12 BIOS32/UEFI64/EFI clean compile PASS. QEMU/OVMF runtime not run in this
environment (tools absent). This proves only the one tested two-run fixture, not
arbitrary fragmentation. See `docs/M6.17-NTFS-FRAGMENTED-INITIALIZED-DATA.ru.md`.

### M6.18 NTFS volume sector-count truncation — software complete

NTFS BPB `total_sectors` is 64-bit, while the current block API is 32-bit. The
parser now rejects a nonzero high DWORD rather than mounting a truncated low-word
alias. A private ntfs-3g image with the high word changed is refused: zero mounted
volumes, no OOB reads/writes, unchanged hash. Actual image, MCC/FAT/GPT and clean
GCC12 BIOS32/UEFI64/EFI compile regressions pass. This is fail-closed handling,
not support for oversized volumes. See
`docs/M6.18-NTFS-64BIT-SECTOR-COUNT.ru.md`.

### M6.19 NTFS Initialized Size sub-cluster boundary — software complete

Strengthened M6.17's two-run poison fixture from a whole-cluster initialized prefix
to 1234 bytes. RED with M6.18 source: full-file and offset-crossing reads failed;
fixed the inner read length to stop exactly at Initialized Size before zero-filling.
`ntfsinfo`, `ntfscluster`, and independent `ntfscat` verify the fixture and expected
bytes. Normal + ASan/UBSan actual-image suites, MCC, GPT/WAL/FAT and GCC12 native
BIOS32/UEFI64/EFI builds pass. No QEMU runtime claim. See
`docs/M6.19-NTFS-SUBCLUSTER-INITIALIZED-SIZE.ru.md`.

### M6.20 NTFS 64-bit file-size refusal — software complete

The VFS exposes only 32-bit file sizes, while NTFS `$DATA` stores 64-bit Real and
Initialized Size. A private ntfs-3g 128 MiB image with a nonempty file truncated
to exactly 4 GiB has `$DATA` size `0x100000000` and a low-word `$FILE_NAME` index;
RED on M6.19 source exposed the path and failed to reject its directory listing.
The driver now refuses nonresident streams with a nonzero size high DWORD at read,
size, path-resolution and dirent boundaries, including checking the MFT attribute
rather than trusting a stale index alone. Actual image matrix, this 4-GiB fixture,
MCC parser/VFS ASan/UBSan and GPT/FAT regressions pass; the image remains RO with
zero writes/OOB reads and unchanged SHA. This is fail-closed behavior, not support
for files above 4 GiB. See
`docs/M6.20-NTFS-64BIT-FILE-SIZE-REFUSAL.ru.md`. QEMU/OVMF runtime not run.
