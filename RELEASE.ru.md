# KvantOS 3.1.4 — управляемая запись, SATA/OHCI и более строгая NTFS RO

18 октября 2026 года. Релиз для **BIOS/i386** и **UEFI/x86-64**. Старые теги и
загрузки 3.1.3 сохраняются без изменений.

## Коротко

3.1.4 объединяет обновления редакторов и C-мастерской, ограниченную запись
FAT32 через заранее подготовленный owned-file журнал, AHCI SATA и OHCI USB
backend, а также дополнительные отказоустойчивые границы NTFS. По умолчанию
диски не становятся записываемыми: разрешение требует явно подготовленного
FAT32, исправного WAL, подтверждённого flush и отдельного включения на текущий
сеанс. NTFS остаётся только для чтения.

## Что нового с 3.1.3

### Редакторы и C-мастерская

- EN/RU ввод, Alt+Shift и явный статус раскладки; буквальная UTF-8-подстрока
  ищется вперёд и назад с переходом через край. Replace One и Replace All
  атомарны; «Заменить всё» — одна отменяемая операция.
- Импорт из USB остаётся копированием в RAM. Добавлены явное состояние
  «RAM / KvFS», приглашение сохранить импорт на KvFS и понятное поведение при
  отсутствии диска/места — автоматической записи нет.
- Оба редактора получили общий мышиный Save As: выбор тома/папки, подтверждение
  замены, привязка следующих сохранений к выбранному назначению. Смена namespace
  или rescan не должны перенаправлять сохранение на другое устройство.
- Поддерживаются явные уведомления RO/ошибок и сохранность старого файла при
  отказе. `vcp` и файловый менеджер проверяют точный объём чтения, поколение
  namespace и no-replace семантику; ошибка чтения не выдаётся как EOF.
- В C-мастерской добавлены фиксированные одномерные массивы для локальных и
  глобальных объектов (в том числе указателей), decay/indexing и `sizeof`
  в ограниченном native C subset. Существующие лимиты source/data/stack
  сохраняются; VLA, инициализаторы массивов, полный ISO C и runtime-проверки
  индексов не заявляются.
- BIOS floppy образ упаковывает компактный GRUB memdisk и остаётся в лимите
  core; прямой BIOS/UEFI ISO сохраняется отдельно.

### Накопители и политика записи

- **Все обычные FAT-тома монтируются RO.** FAT16, неподготовленный FAT32,
  неизвестный или повреждённый WAL не становятся writable автоматически.
- Запись на подготовленный FAT32 доступна только для выделенного файла
  `KVANTWAL.SYS` с проверенной геометрией, идентичностью тома и исключительным
  владением extent. Валидное восстановление выполняется **до публикации
  namespace**; затем оператор отдельно включает том командой
  `fatwrite ИМЯ_ТОМА enable`. Отключение, rescan или перезагрузка снимают
  разрешение. Подготовка описана в [docs/STORAGE-M2.md](docs/STORAGE-M2.md);
  ядро не создаёт и не сбрасывает журнал само.
- USB BOT: UHCI, EHCI с OHCI companion и отдельный OHCI full-speed backend.
  AHCI/SATA поддерживает чтение; SATA WRITE/FLUSH EXT выдаётся только через
  общие generation-checked leases и тот же owned-WAL путь. Без требуемого
  FLUSH, после ошибки/timeout или при неопределённом завершении том остаётся
  RO/заблокирован до безопасного восстановления или перезагрузки.
- GPT остаётся read-only: проверяются CRC заголовка/таблицы, границы и резервная
  копия; GPT, MSR и разделы не используются как место для журнала. Никакого
  автоматического форматирования, конвертации MBR/GPT или ремонта.

### NTFS — только чтение

- Проверены частные образы ntfs-3g с logical sector 512/1024/2048/4096 байт,
  resident/nonresident `$DATA`, Unicode-имена, MBR start offset, неполные
  чтения/EOF и фрагментированный поток.
- Реализован zero-fill строго с NTFS Initialized Size, даже если он заканчивается
  внутри кластера. Тестовая область за границей отравлена `0xA5` и не возвращается
  читателю.
- BPB total sectors с ненулевым high DWORD и размер файла `$DATA` выше 32-битной
  границы отвергаются, а не превращаются в low-word alias. VFS имеет 32-битный
  размер; **файлы больше 4 GiB и объёмы за текущей адресацией не поддерживаются**.
- Сжатые/зашифрованные потоки, NTFS-запись, repair и восстановление Windows
  журналов не поддерживаются.

## Использование

1. Загрузите именно новый `kvantos.iso`; старая установленная копия на диске
   сама не обновляется. Для BIOS/i386 также доступен floppy fallback.
2. Импорт с USB выбирает файл мышью и копирует его в RAM. Источник остаётся
   неизменным; для сохранения после перезагрузки нужен KvFS либо явно выбранный
   и разрешённый FAT32 destination.
3. Обычный FAT остаётся RO. На заранее подготовленном owned-WAL FAT32 сначала
   проверьте recovery/status, затем при необходимости на текущий сеанс включите
   `fatwrite ИМЯ_ТОМА enable`. Используйте актуальное имя тома. После rescan
   разрешение потребуется выдать снова.
4. После неопределённой ошибки FAT сначала восстановите журнал в KvantOS;
   не запускайте внешнюю запись или `fsck` repair поверх незавершённого WAL.
   Обычный FAT-драйвер другой ОС журнал KvantOS не понимает.

Для подготовки/ограничений записи см. [STORAGE-M2.md](docs/STORAGE-M2.md) и
[M3.2 AHCI](docs/STORAGE-M3.2.md). Руководства: [USB/FAT32](USB-IMPORT.ru.md),
[C-мастерская](sdk/WORKSHOP.ru.md), [установка](INSTALL.ru.md).

## Проверки и границы доказательств

Локальный релизный gate выполнен с **GCC 12.4.0, QEMU 10.0.13 и OVMF
2025.02**:

- `make CC=gcc-12 release` и `tests/boot/check-images.py`: BIOS32, UEFI64,
  EFI, GRUB ISO, direct ISO, floppy, 11 приложений на архитектуру, пустой KvFS,
  валидные firmware entries и SHA-256 manifests — PASS.
- QEMU BIOS32/UEFI64 boot с CD, direct ISO и firmware USB — достигли `kvsh`;
  BIOS floppy fallback — PASS.
- GUI/native-app, C-мастерская, мышиные редакторы, EN/RU ввод/find/replace,
  перезагрузка и KvFS persistence — PASS для обеих архитектур.
- USB Import EHCI+OHCI на GPT и MBR — BIOS32 + UEFI64; source image SHA-256
  неизменны. AHCI read-only/EIO, смешанные AHCI+USB owned-WAL сохранения и GUI
  Save As в подготовленный FAT32 — PASS; recovery и независимый fsck включены.
- EHCI per-controller isolation/detach fencing — PASS на обеих архитектурах.
- MCC native compiler + ASan/UBSan parser/VFS/controller suites, GPT, FAT/WAL
  fault/performance, NTFS actual-image suite и отдельный M6.20 ASan/UBSan прогон
  — PASS. NTFS fixture содержит ровно 4-GiB `$DATA` и отравленный fragmented
  Initialized-Size хвост; все образы остаются неизменными.
- Тестовые образы создавались как временные regular files; **ноль физических
  устройств и host mounts**. QEMU/software pass не квалифицирует физические
  флешки/SATA или VirtualBox.

### Честные ограничения

- Экспериментальная ОС. Не храните единственную копию важных данных; резервная
  копия обязательна. Запись FAT требует единственного писателя, честного flush
  и корректно подготовленного WAL. Torn/повреждённый WAL может привести к
  отказу монтирования; это намеренный fail-closed, а не автоматический ремонт.
- AHCI hotplug, xHCI/USB 3, UAS, NVMe и ATAPI не поддерживаются. EHCI hub/TT
  маршрутизация остаётся ограниченной экспериментальной областью; не обещаются
  произвольные hub trees, split bandwidth, multi-TT или физическая совместимость.
- Реальное железо, VirtualBox passthrough, physical 4Kn и сторонняя параллельная
  запись не квалифицированы. Одна причина отказа параллельного x64 Save As из
  M3.5d остаётся открытой; не считать стресс/совместимость доказанными.
- Native C Workshop — ограниченное подмножество языка и не security sandbox;
  не запускайте недоверенный код. Полная изоляция приложений, полный GCC/ISO C,
  Unicode case-folding и NTFS write/repair не входят в релиз.
- FAT filesystem repair, GPT repair и автоматическая подготовка WAL отсутствуют.

## Файлы релиза

- `kvantos.iso` — основной BIOS/UEFI ISO, допускающий firmware USB boot.
- `kvantos-direct.iso` — альтернативный BIOS/UEFI ISO без GRUB; не предназначен
  для прямой записи как raw USB hybrid.
- `kvantos-floppy.img`, `kvantos-disk.img` (пустой KvFS), `kvant.bin`,
  `kvant64.bin`, `kvantefi.efi`, `esp.img`.
- `kvantos-3.1.4.tar.gz` — полный комплект с приложениями, документацией и
  SHA256SUMS; `kvantos-3.1.4-source.tar.gz` — исходный snapshot;
  `kvantos-3.1.4-SHA256SUMS.txt` — контрольные суммы отдельных assets.

BIOS/QEMU:

```sh
qemu-system-i386 -m 256 -boot d -cdrom kvantos.iso \
  -drive file=kvantos-disk.img,format=raw,if=ide
```

UEFI/QEMU (OVMF, Secure Boot выключен):

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom kvantos.iso -boot d \
  -device piix3-ide,id=kvide \
  -drive file=kvantos-disk.img,format=raw,if=none,id=kvdata \
  -device ide-hd,bus=kvide.0,drive=kvdata
```

`q35`-овый `-drive if=ide` обычно попадает на неподдерживаемый AHCI; пример
явно использует PIIX3/IDE. BIOS/QEMU persistence требует ATA PIO. Для полного
комплекта проверяйте `sha256sum -c SHA256SUMS`; для отдельных загрузок —
`sha256sum -c kvantos-3.1.4-SHA256SUMS.txt`.

Лицензия: MIT. Перед установкой прочитайте `INSTALL.ru.md` и сохраните резервную
копию данных.
