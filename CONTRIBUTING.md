# Участие в разработке KvantOS

Начните с [ROADMAP](ROADMAP.md), [текущей очереди](docs/AUTONOMOUS-QUEUE.ru.md)
и [матрицы квалификации](docs/QUALIFICATION-MATRIX.ru.md). Исторические release
описания не заменяют текущий experimental/software/hardware статус.

## Изменения

- Один проверяемый increment на commit/PR; не переписывайте прошлые tags/releases.
  Указывайте проблему, изменение, выполненные проверки и то, что осталось OPEN.
- Сохраняйте BIOS/i386 и UEFI/x86-64, GRUB/direct images, limited native C,
  ABI2/3 и opt-in ABI4. ABI2/3 — trusted ring0, не sandbox.
- Kernel/SDK runtime без FPU до отдельного context/profile gate. Нельзя просто
  снять TS или добавить float keywords. См. [FPU safety](docs/FPU-CONTEXT-SAFETY.ru.md).
- Не раздувайте таблицы/stack/limits, чтобы скрыть ошибку fixture. Проверяйте
  границы, overflow, OOM, generation/ownership, отказ без частичных записей,
  cleanup/yield/reload. [Compiler budgets](docs/M5.5-COMPILER-BUDGETS.ru.md).
- Редактор хранит packed text; >128 строк — предупреждение, не запрет. Отказ
  import/build/save не должен обрезать или уничтожать документ.

## Сборка и software checks

Зависимости Debian/Ubuntu перечислены в `.github/workflows/build.yml`: GCC12
с multilib, binutils/NASM, GRUB/xorriso/mtools, QEMU/OVMF, Python3 и venv.
Для execution-model tests нужны Unicorn2.1.0 и setuptools<81 (pkg_resources);
GUI screenshot helpers используют Pillow. В конкретном окружении выбирайте Python с этими пакетами через PATH.
Не устанавливайте host packages/credentials в tracked файлы.

```sh
make -C sdk/isolated CC=gcc-12 all runtime
make -j2 CC=gcc-12 STORAGE_LAB=0 release
make -C tests/mcc CC=gcc-12 check includes budgets rtc-cache isolated \
  sanitize sanitize-isolated user-space user-service user-image filemgr
python3 tests/boot/user-isolation-smoke.py
python3 tests/boot/isolated-workshop-smoke.py
python3 tests/boot/isolated-app-smoke.py
python3 tests/boot/filemgr-copy-smoke.py
python3 tests/boot/filemgr-rename-smoke.py
python3 tests/boot/filemgr-namespace-smoke.py
python3 tests/boot/shell-vcp-smoke.py
python3 tests/boot/shell-path-smoke.py
python3 tests/boot/listing-status-smoke.py
python3 tests/boot/kvfs-state-smoke.py
git diff --check
```

Native scripts сами запускают BIOS32 и UEFI64, если MCC_ARCH64 не задан.
Они используют disposable regular-file images. Не пересобирайте kernel/symbols,
пока исполняются native tests. Не подменяйте native proof только ASan/Unicorn.
Дополнительные host/editor/storage tests выбирайте по затронутой области;
актуальные команды находятся в workflow и профильной proof-документации.

В build workflow добавлен `compiler-isolation` job с этими suites и log-only
artifacts. Наличие YAML **не** означает, что hosted CI уже выполнен/зелёный:
для этого нужны реальные run URL, commit и conclusion. Здесь нет auto-publication.

## Носители и доверие

- Host tests — обычные файлы, без mount/format/raw disk writes. Для physical
  тестов нужен отдельный согласованный disposable носитель, backup и план отката.
- Default RO, explicit opt-in, owned WAL, generations/private DMA/fences
  сохраняются. GPT write grant не равен разрешению GPT repair/repartition.
- USB/TT timeout не лечить blind retry WRITE, расширением timeout или restart
  failed controller без понимания ownership/late DMA/transaction state.
- «QEMU PASS» не закрывает VirtualBox/physical USB/SATA/TT/vendor gates.
  Исторический Save64 отказ остаётся не диагностированным.

## Отчёты и артефакты

Укажите commit/toolchain, firmware/machine/CPU, точные команды, exit status,
ожидание/факт и минимальный исходник. При storage проблеме важны controller,
hub/скорость, layout, RO/opt-in/WAL, reconnect и сохранность старого файла.
Логи/result.json прикладывайте без credentials, serial numbers и личных данных.

Не коммитьте воспроизводимые build/SDK objects, test disk images или токены.
Сохраняйте компактный proof archive с source SHA256 и final logs; отдельно
маркируйте ранние failed runs, а не удаляйте их из истории доказательства.
`testshots/index.html` — generated gallery, помечена Linguist-generated; сами
исторические screenshots и опубликованные assets этим не удаляются.

Push/tag/release/publication, закрытие чужих PR и destructive tests — отдельные
действия с отдельным согласованием, не следствие локальной успешной сборки.
