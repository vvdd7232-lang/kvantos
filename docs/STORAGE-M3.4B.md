# M3.4b — меньше повторных FAT/BOT I/O

Продолжение доставленного M3.4a, не новый публичный релиз. Core: `076420b`.
Изменены FAT/VFS listing и bounded READ-only GPT batching, а не USB
topology/transport или формат owned WAL.
Физическая USB/VirtualBox-приёмка и EHCI TT/split остаются отдельными задачами.

## Ограниченное рабочее множество метаданных

* Восемь секторов FAT/FSInfo, до 4096 байт каждый, в BSS. Нет кэша файловых
  данных/каталогов и нет состояния, переживающего внешнюю операцию чтения/списка/записи.
* Чтение: полный прежний Floyd chain validation сохранён. Повторные обращения
  к одному сектору FAT внутри одного `read` обслуживаются из RAM. Scope закрывается
  и при ошибке. Каталог и пользовательские данные по-прежнему проходят block I/O
  с существующей USB cache/ancestor проверкой; transport guards не ослаблены.
* Запись: несколько изменений одного FAT/FSInfo-сектора объединяются только
  внутри текущей транзакции. Обе копии FAT по-прежнему сравниваются; несовпадение
  не чинится автоматически. FSInfo signatures/count bounds проверяются как раньше.
* Dirty eviction идёт исключительно через прежний `stx_write`: backup + barrier,
  journal header + barrier, затем target. Формат WAL, capacity 48 физических
  секторов, ambiguous-close fence и ownership/lease проверки не изменены.
* FAT dirty bit сбрасывается и физически отправляется в начале транзакции.
  Pending FAT/FSInfo сбрасываются перед directory publication, перед COW body/chain
  durability barrier и до commit/clean WAL header. Clean bit также проходит этот путь.
* При отказе сначала отбрасывается RAM working set, затем запускается прежний undo
  физических изменений. Неуспешная операция никогда не дописывает deferred state.
  Commit, abort и reset mounts инвалидируют scope. Fresh RO + explicit opt-in прежние.
* Сохраняется прежнее ограничение: FAT single-task/non-reentrant, один writer.
  Это не общий write-back block cache и не разрешение одновременно монтировать
  один носитель с другой ОС. USB по-прежнему принимает только сектор 512 байт;
  host FAT16/4KiB tests не являются поддержкой USB 4Kn.

## Список каталога и GPT

`vfs_readdir_many` возвращает до 128 записей за вызов; GUI использует BSS batch
из 32 записей. FAT проверяет всю цепочку и проходит каталог один раз вместо
повторного поиска от начала для каждой строки. Внешнего persistent cursor нет.
Остальные драйверы используют прежний single-entry fallback; публичный app ABI
не менялся. Фильтрация C-файлов, имена, 128 **файлов** в picker и 512 scanned
entries сохраняются; это не возвращение ограничения на 128 **строк текста**.

Полная CRC GPT entry array вычисляется, как раньше, до любых mount. Чтение CRC
и последующий разбор используют блоки максимум 4096 байт / 8 физических секторов,
строго внутри уже проверенных границ entry array. Strides 128…4096 и backup
fallback сохранены. Это только READ: нет ремонта GPT, MBR conversion или format.
22 GPT/MBR/raw tests, включая большой stride/count/slot, пройдены с неизменными SHA.

## Проверено на host

Одинаковый FAT/VFS/WAL harness, private regular-file FAT32 image, файл 65535 байт:

| Операция / block calls | M3.4a | M3.4b |
|---|---:|---:|
| Чтения во время создания | 974 | 20 |
| Записи во время создания, включая body и WAL | 917 | 157 |
| Sync во время создания | 18 | 18 |
| Чтения при cold full-file read | 451 | 131 |
| Список 19 записей, старый single API / новый batch consumer | 260 | 12 |

Это **host block calls**, не счётчик USB-команд и не время передачи.
Независимые `mcopy`/`fsck.fat -n`, cold reopen и точные 65535 байт проверены. Batch listing сравнен по всем полям
с single-entry API, проверены offsets/EOF/границы/не-каталог и 12 read-EIO cuts:
нет partial success, весь исходный образ побайтно неизменён.

* Реальный FAT + production owned WAL под ASan/UBSan: 1696 before/after/EIO/unplug
  cuts; 26 torn-sector writes; 32 recovery interruptions; read errors, invalid
  ownership/mirrors, full volume, cluster sizes, GPT/MSR/gaps. Все PASS.
* Fragmented image: 28 разных FAT target sectors при восьми cache slots;
  793 write/barrier boundaries, **52 выборочных**, не исчерпывающих, cuts.
  Cold namespace строго old-or-new, независимый fsck; ASan/UBSan PASS.
* Actual working-set header: scope/overlay exclusion/dirty eviction/discard,
  read/write EIO/live fence, logical sectors 512/1024/2048/4096; ASan/UBSan PASS.
* Legacy FAT undo suite: FAT16/32, LFN cross-cluster create/remove/mkdir/rmdir,
  full/RO/cyclic chain; provisioner policy и exhaustion 48 records PASS.
* Sector transaction persistence model, USB BOT, UHCI/OHCI/hub models, AHCI/block,
  VFS/KvFS, text editor/input и compiler 73 + 73 checks PASS.

## QEMU GUI/регрессии

Все восемь полных GUI suites **PASS**: UHCI/MBR и OHCI/GPT, BIOS32 и UEFI64.
Фиксированные ядра/ISO; существующие deadlines не увеличены, каждый target —
один измеренный click без слепых повторов.

| Контроллер / layout | Ядро | Save As 65535 байт | HUGE.C ≈18 КиБ | Suite |
|---|---|---:|---:|---|
| UHCI / MBR | BIOS32 | 17.042 с | 4.374 с | PASS |
| UHCI / MBR | UEFI64 | 16.464 с | 4.340 с | PASS |
| OHCI / GPT | BIOS32 | 18.462 с | 4.377 с | PASS |
| OHCI / GPT | UEFI64 | 20.305 с | 4.549 с | PASS |

Дополнительно OHCI/BIOS32 CLI copy тех же 65535 байт: **17.232 s** против
119.598 s в доставленном M3.4a (~6.9× быстрее), cold fresh RO,
точный независимый `mcopy` и `fsck.fat -n` PASS. Это не физический throughput.
Повторный OHCI GUI Refresh: 12/12, 2.307…2.832 s, прежний предел 8 s.
Полные import suites тоже проверили по 12 rescans, unplug/replug и неизменные SHA.
Save suites проверили bound Save, UTF-8/LFN, overwrite/cancel, cold GUI import,
реальный unplug в normal-kernel commit cut, неизменные document/Undo и recovery/fsck.

Nested topology повторно PASS: оба HC × оба ядра, два вложенных хаба/три диска,
12 rescans/unique addresses, cached reads после leaf/ancestor unplug,
rebuild/sibling isolation, все исходные SHA неизменны. Mixed SATA+USB шесть
root cells и GRUB/direct BIOS+UEFI boot smoke повторно PASS.
M3.4a software GUI blocker закрыт; аппаратные пункты и TT/split НЕ закрыты.

История разработки сохраняется отдельно: preliminary OHCI Save As 18.072 s
проверялся до исправления scope чтения; такой запуск намеренно остановлен.
Первый UHCI import на том промежуточном ядре повторил прежний 8 s timeout.
Host call-count regression обнаружил сброс read scope внутри `walk_dir`;
исправление сохраняет scope до выхода из внешнего `read`. Эти промежуточные
логи не заменяют финальную GUI qualification.

Следующая промежуточная версия прошла импорт большого файла, но не полный
OHCI Refresh. Debugger измерил примерно 6.58 s в scan и 3.92 s в построении
списка. Одного FAT-link cache оказалось недостаточно. Directory batches убрали
повторные проходы, однако единичный Refresh 7.386 s не доказал 12-cycle acceptance:
полный suite ещё поймал 8 s boundary. Поэтому дополнительно объединено чтение GPT.
История failed/stopped runs сохранена по core commit, deadlines не повышались.

## Воспроизведение

```sh
make -j2 CC=gcc-12 release
make -C tests/mcc CC=gcc-12 fat-meta storage uhci ohci hub
make -C tests/fs CC=gcc-12 fat-owned fat-perf fat-meta-eviction
# Полная production FAT fault matrix под санитайзерами:
make -C tests/fs CC=gcc-12 build/fat-owned-sanitize
FAT_OWNED_HARNESS="$PWD/tests/fs/build/fat-owned-sanitize" python3 tests/fs/test_fat_owned.py
# Для каждого MCC_ARCH64=0/1:
MCC_ARCH64=0 USB_HUB_DEPTH=1 USB_CONTROLLER=uhci USB_LAYOUT=mbr python3 tests/boot/usb-import-smoke.py
MCC_ARCH64=0 USB_HUB_DEPTH=1 GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ohci STORAGE_LAYOUT=gpt python3 tests/boot/save-as-smoke.py
```

Для полной hub GUI матрицы обе команды выполняются для UHCI/MBR и OHCI/GPT,
на BIOS32 и UEFI64. Свежий носитель требует отдельного явного `fatwrite … enable`.
Provisioner работает только на одноразовых тестовых образах; пользовательский
GeneralUDisk и физические устройства не изменялись. Push/tag/publication не выполнялись.
