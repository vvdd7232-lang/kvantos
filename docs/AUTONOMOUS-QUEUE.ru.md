# Очередь самостоятельной работы

Пользователь разрешил продолжать по ROADMAP без вопросов (17.10.2026).
Не означает разрешение на физические диски, публикацию или фиктивное закрытие проверок.

## Порядок

1. **Checkpoint готов: M3.5d — диагностика EHCI.** Закончить regression/холодное recovery
   сохранённых finite-delay образов; зафиксировать диагностический checkpoint.
   Исторический отказ M3.5c не называть исправленным: точной причины нет.
2. **M3 TT cleanup — отдельный safety design.** Не включать простой повтор WRITE.
   Без квалифицированной модели CLEAR_TT_BUFFER/ошибок сохранить fail-closed fence.
   Аппаратные TT/VBox/companion/SATA проверки остаются отложенными.
3. **M4.1 — контракт изолированного ABI.** Описать новый контракт без прямых
   kernel-pointer API, совместимость старых ABI2/3, syscall/buffer/resource rules,
   milestones и тесты i386/x64. Это дизайн, не готовая ring3-изоляция.
4. **M5.1 — массивы и sizeof: реализованный ограниченный increment.** Независимая полезная работа, пока runtime M4
   требует больших архитектурных изменений. Начать с ограниченных одномерных
   массивов и sizeof; не добавлять float до FPU/context/ABI.
5. Затем простой bounded object-like #define; после него структуры/typedef и
   architecture-specific layout tests. Каждый пункт закрывать отдельно.
6. После проверенных compiler increments вернуться к реализации M4 по контракту:
   address spaces/stacks/TSS, безопасные syscalls, exception cleanup, preemption.

## Правила checkpoints

- Сохранять BIOS32/UEFI64, ограниченный native C, старые .kapp/ABI2/3 и редакторы.
- Default RO/opt-in/WAL/generations/private DMA/fences не ослаблять.
- NumPad MouseKeys **уже есть** в kernel/keyboard.c; не реализовывать повторно.
- Проверки на временных regular-file fixtures, не на пользовательских дисках.
- Старые root ZIP/Git/uploads/исходники не удалять. После проверки сжимать proof,
  удалять только созданные воспроизводимые build/test images и объекты.
- Не оставлять гигабайтную QEMU build; историческая диагностика сохранена в
  /home/user/workspace-checkpoints, нужна выборочная распаковка.
- Записывать точные PASS/OPEN/ограничения; не обещать, что весь roadmap закрыт.

## Журнал

- Начальное состояние: HEAD 2c4c6c2, WIP EHCI fault snapshot/probes/trace decoder.
- До очистки: finite I/O delay на обоих ядрах даёт reason=250-frames, len13 CSW;
  caller/history сохранены. Параллельные x64 гости: 10/10 SaveAs + fsck PASS.
- Архивы checkpoint и diagnostics проверены; workspace после очистки 112 MiB.
- M3.5d checkpoint: NORMAL release/boot + host models + EHCI isolation PASS;
  cold slow32/slow64 recovery через stock OHCI всеми ядрами 4/4 PASS (не TT).
  FRINDEX host model исправлен; независимые FS TT clocks/wrap/budget PASS.
  Историческая причина OPEN; TT safety design сохранён, recovery не включён.
- M4.1: docs/M4-ISOLATED-ABI.ru.md — проект контракта; ring3/runtime OPEN.
- M5.1: fixed arrays + unevaluated sizeof реализованы; 151+151 host checks,
  40000+40000 mutations ASan/UBSan, native Workshop BIOS32/UEFI64 и boot PASS.
  Stack/data/source limits и ABI3 сохранены. VLA/array initializers/size_t не обещаны.
- Следующий пункт: M5.2 bounded object-like #define → structs/typedef → M4.2.
  M3 historical cause / TT recovery / hardware по-прежнему OPEN.

- Пользователь подтвердил: checkpoint — не конец работы; не останавливаться между
  независимыми пунктами. M5.2 host/sanitizer PASS, native smoke выполняется;
  сразу продолжается M5.3 structs/typedef. Восстановленные source bytes M5.1
  сверены с source archive; Git ref предыдущего turn не сохранился, baseline
  заново закреплён commit 90731e2 без изменения этих исходников.

- M5.2 native BIOS/UEFI PASS. M5.3 structs/typedef: 267+267 host checks,
  40000+40000 sanitizer mutations и NORMAL release PASS; native smoke запущен.
  Не ждём нового сообщения: начат M4.2 implementation, address-space primitives.

- M5.3 native BIOS32/UEFI64 PASS. M4.2 private page tables/copies/ownership
  реализованы, ASan/UBSan PASS. M4.3 настоящий CPL3 native CPU suite 3 раза
  на архитектуру PASS (kernel/privilege/fault/guard/NX/copy/таймер/cleanup).
  M4.4 bounded event/draw/private-RAM services реализованы; 20000 mutations
  request/target PASS, расширенный native suite выполняется. Public loader OPEN.
  Следом без остановки — M4.5 preempt/resume и проверка kernel peer liveness.

- M4.4 host/native PASS зафиксирован отдельной proof-документацией.
- M4.5 full-frame preempt/resume, DF/R12, peer/kill и FPU/fast-entry fences:
  BIOS32 32×3 + UEFI64 37×3 = 207 fixtures PASS, процесс exit 0.
- M4.6 strict format2/API4, persistent event runtime, отдельный native SDK и
  оконный bridge реализованы. Host sanitizer/image OOM/mutations PASS.
  Реальный GUI BIOS32/UEFI64 PASS: draw/click/private file, malicious images,
  dual loader ABI2/4, close/free pages, unchanged disposable disk SHA.
  Найденные native defects исправлены: syscall drawing заменён cache/replay,
  win_close/GUI exit теперь выгружают приложение. Ничего не опубликовано.
- **Следом без ожидания пользователя M4.7**: supervisor-map coherence,
  FSGSBASE/CPU-policy hardening, явный legacy trust label и расширенные
  queued-draw/GUI-exit regressions. Затем compiler ABI4/services; floats
  только после FPU context. Workshop/default11 apps пока ABI3/ring0.
- M4.7 supervisor refresh / inherited FSGSBASE policy / IRQ guards / legacy trust
  label реализованы. Host sanitizer и 267+267 compiler PASS; native CPU 210
  fixtures PASS, SDK GUI delayed-DRAW (реальный yield), cover/uncover и GUI exit
  PASS на обеих архитектурах. Полная Workshop BIOS/UEFI regression exit 0.
- Следом без паузы: M4.8 bounded UTF-8 text/graphics service и replay в compositor,
  затем изолированный compiler adapter/opt-in Workshop target. Не выдавать ABI3
  default apps за изолированные; аппаратные storage gaps остаются OPEN.
- M4.8 optional bounded UTF-8 TEXT/QUERY и ordered rect/text replay готовы:
  ASan/UBSan + 5000 text mutations/arch PASS; native BIOS32/UEFI64 exit0,
  28 text bytes / 369 visible text pixels и delayed-DRAW/GUI-exit PASS.
  SDK docs добавлены; default compiler/11 apps не переключены.
- Следом M4.9: private status/readonly hz/language metadata, затем user-only
  compiler adapter и mouse opt-in target в Мастерской. Движемся без разрешений
  на каждый checkpoint, без публикации и без расширения disk grants.
- M4.9 STATUS/INFO и kernel-owned ABI4/legacy trust labels: host sanitizer PASS,
  native GUI BIOS32/UEFI64 exit0; hz/lang совпадают, status работает, cleanup
  и disk SHA сохранены. Prelinked user-only MCC adapter уже написан/собран,
  далее M4.10 — target generation + mouse opt-in + host/native tests.
- M4.10 opt-in native compiler ABI4 target и mouse switch реализованы.
  Свежая полная software verification process-e35832da exit0: SDK/adapter,
  release, legacy267+267, isolated20+20, ASan target40000+40000 и native CPU,
  SDK GUI, in-OS Workshop BIOS32/UEFI64 PASS. См. M4.10-ISOLATED-WORKSHOP.ru.md.
- Workspace snapshot сохранил исходники, но исключил build/ и вернул Git refs
  к 2c4c6c2. Поэтому всё перепроверено заново; старые SHA не подделываются.
  Три защищённых chmod вне scope. Дальше: compiler-profile resource failures,
  дополнительные capabilities/graphics; FPU/float только после context support.
- M4.11 fail-closed DRAW/TEXT/STATUS adapter + ровно25 quantum budget: готово.
  Native in-OS compiler kernel-write/command+text quotas/spin/heap/files cases
  BIOS32/UEFI64 exit0 (abi4-3f5a5deb), PMM baseline и private namespace PASS.
  Исходники предыдущего checkpoint сверены: 410/410 SHA256, закреплены ac0033b;
  protected3chmod не включены. Далее M4.12 line + private listing.
- M4.12 clipped signed16 LINE и private LIST: host sanitizer/random geometry/
  atomic outputs PASS; CPU/SDK/Workshop native BIOS32/UEFI64 exit0. line/file_list
  включены в isolated compiler; clock/beep/log ещё compile errors. Далее M5.4
  bounded readonly virtual-header #include (whitelist/depth/cycle/work guards).

- M5.4 readonly builtin headers готовы: kvui/kvkeys/kvtypes, include-once/depth4/
  cycle/work/output guards, source error rows сохранены. 301 legacy +23 isolated
  checks/arch, standalone guard ASan/UBSan и по40000 mutations/target/arch PASS.
  NORMAL release, native Workshop (оба targets с headers)/CPU/SDK BIOS32+UEFI64
  exit0. Никаких filesystem includes/stdint/full CPP; source16000 сохранён.
- Далее без остановки: M4.13 readonly clock adapter с bounded kernel snapshot
  (без RTC I/O из user syscall), затем review compiler source/table budgets.
  FPU/float и исторический Save64/physical TT recovery остаются OPEN.

- M4.13 CLOCK15/cap64: readonly cached RTC без I/O в syscall, атомарный output20,
  nullable/aliased compiler adapter, exit35, caps127 готовы. Host RTC cache/
  service/ASan/UBSan/fuzz/301 legacy+22 isolated checks/arch и release PASS.
  Native BIOS32/UEFI64 Workshop clock сверён с trusted RTC snapshot, CPU/SDK
  regressions exit0 (readonly-clock-bios32-uefi64-b9b25e43). Исправлен decode
  12h midnight/noon; 48 binary/BCD/AM-PM cases и exhausted UIP guard PASS.
  Первый extended native clock-check выявил только невыравненный offset в
  harness: global allocations aligned to MCC_WORD; исправлено, rerun PASS.
- Durable M411/M412/M54 proof: kvantos-m411-m412-m54-software-proof.tar.gz,
  33999 B, SHA256 b8e1870d6e0af02ba8b1c811bc9a6900875988f10b489e0aeba11a298fbc2680,
  415 source hashes, old named logs и M54 latest native JSON.
- Следом M5.5: review source/table budgets и sanitizer boundary tests без
  автоматического раздувания лимитов. Physical/FPU/float/Save64 TT OPEN.

- M5.5 review завершён: source16000 и прочие budgets сохранены, 80 sanitizer
  boundary/guard-page/table/row checks на архитектуру (оба targets) PASS.
  Ровно16000 bytes принято,16001 отказ, UTF-8 bytes/строка15001 корректны;
  header costs/таблицы учтены, output atomicity и reset PASS. Production source
  не менялся после8cf7b2c; native M413 proof не подменяется новым запуском.
- Далее M4.14: закрыть недостающие native негативные STATUS33/LINE34 случаи,
  переиспользуя quota.c/kapp, не раздувая RAMFS32 и не трогая physical devices.

- M4.14 native STATUS33/LINE34/clock-kernel-pointer #PF прошли на обеих
  архитектурах (native-status-line-clock-899f200a exit0);25 isolated host cases/
  arch PASS. quota.c/kapp переиспользуются, RAMFS32 прежний, PMM baseline и
  GUI/heap/files/legacy switch сохранены. Exit35 negative native не заявляется.
- M413/M55 proof archived:419 source SHA,32797B,
  b9d166cef5c8302a0b68261c5c8cfe63ba0383c9cb02182dae78a3d62d81ac41.
- Далее FPU/context safety design + проверяемая policy модель; не снимать TS
  до сохранения полного доступного CPU state, CPUID gates и native proof.

- M4.15 default-deny x87/MMX/SSE/MXCSR/FXSAVE/XSAVE probes: software security
  regression нашёл QEMU10.0.13 TS-only bypass при inherited OSXSAVE. Policy
  теперь явно очищает OSXSAVE и восстанавливает priorCR4; не меняет assertion
  на «любой fault». Финальный osxsave-fence-9821f38c exit0:288 CPU fixtures,
  Workshop/SDK обеих архитектур PASS,1024B state-output без partial writes.
  Full host/sanitizers/budget80/legacy301/isolated25 и release PASS. FP context
  не включён; design в FPU-CONTEXT-SAFETY.ru.md, float по-прежнему OPEN.
- Далее сопровождение: отдельный compiler-isolation CI job (local config,
  не hosted PASS), CONTRIBUTING/issue+PR templates, qualification matrix и
  точечный Linguist-generated для testshots/index.html; без push/publication.

- Сопровождение: CONTRIBUTING/issue+PR templates/qualification matrix,
  compiler-isolation CI job и точечный Linguist-generated готовы. Local YAML/
  forms/schema essentials/bash -n/link/permissions/log-only checks PASS.
  Эквивалентные команды M415 реально PASS; hosted CI не запущен и не заявлен.
  Нет push/tag/release/PR closure, release workflow и protected3chmod прежние.
- Далее без запроса разрешений: полный текущий regression checkpoint и durable
  proof M414/M415, затем очередной software increment из roadmap. FP backend,
  physical/TT/Save64 и remote publication требуют собственных gates.

- Полная общая regression af17a7a: software-kvantos-937b5f2d exit0, host
  compiler/runtime/editor/storage/controller/FS, release/image layout, BIOS32
  CD+floppy и UEFI64 boot, native editor input/history/persistence PASS.
  Durable429 source SHA/39058B/6b4cd4038759d804fb8a8af354571c3dceb2b306f759eacbf61d9bb30fc4993e.
- M6.1 copy hardening готов: точный read count доwrite, bounded path/name/no
  truncation, same-path no-op, namespace epoch и uncertain-write diagnostics.
  Source не удаляется; atomic move не заявлен. Host EN/RU+2000 mutations и
  full models/sanitizer PASS. process-ba43db21 exit0: native mouse copy/whole
  disk SHA no-op, Workshop/CPU288/SDK обеих архитектур PASS. CI wiring local only.
- Далее M6.2 mouse filemanager listing identity/double-click и rename/move
  prerequisites. Не смешивать copy hardening с выполненным cross-volume move.

- M6.2 listing/VFS identity и reset после toolbar/background готовы; wrap epoch
  host test и2000 fault mutations PASS. Native process-d63f63a8 exit0: single
  click после смены списка не открывает файл, настоящий double-click preview
  byte-exact, RAM→KvFS/no-op и Workshop/CPU288/SDK обеих архитектур PASS.
- Далее M6.3: backend no-replace rename RAMFS/KvFS/VFS (один metadata commit,
  unknown-outcome fence, без cross-volume copy-delete); затем mouse UI.

- M6.3 backend no-replace rename готов: RAM allocation-free/full-table; KvFS
  одна metadata публикация с uncertain-outcome fence; VFS RO/unsupported/
  cross-volume fail-closed. Старый mv больше не удаляет назначение заранее.
  Full host+release+native rename/filemgr/Workshop/CPU/SDK обеих архитектур
  PASS; kvantos-m6-3-native-rename-gui-ba7c84df exit0. См. M6.3 contract.
- M61/M62 durable proof HEAD36705b1:433 source SHA/33993B,
  0bcf6884ce096daf31586f292b424fa030fa2568ac03d572b31edc4e9f0b9960.
- Далее M6.4: мышиный rename modal с snapshot pane/listing/VFS identity,
  bounded paths, no overwrite, честной неопределённостью и кнопками Apply/
  Cancel. Не выдавать M6.3 backend/shell за готовый mouse move. Заодно
  закрыть truncated 15-byte volume alias в resolve; затем старый RAM shell
  cp (сейчас предварительно удаляет target перед allocation) отдельным
  hardening increment без расширения storage opt-in/FP/hardware scope.

- M6.4 завершён: новый mouse rename/new-folder modal, Apply/Cancel и RU
  Unicode input/backspace; source/listing/pane/path/VFS snapshot на момент
  открытия; закрытие окна отменяет input. Truncated volume component alias
  закрыт. Targeted/full host и native mouse rename+copy обеих архитектур PASS;
  общая backend/Workshop/CPU/SDK native regression также PASS,
  kvantos-m6-4-native-regression-d7d63633 exit0. Дополнительный 640×480
  обеих архитектур PASS, kvantos-640-480-75e70547 exit0. Final release PASS,
  kernel SHA до/после упаковки тот же. Не physical/hosted/publication proof.
- M6.3 durable proof HEAD4788384:436 committed source SHA/45059B,
  cf60613542e84b8e191cb883c0019062c1b9acecad1e202c023363239000dc69.
- Следующие integrity gates: старый RAM shell cp не должен заранее удалять
  target; mounted=false после I/O fence нельзя выдавать за отсутствие ФС
  с предложением форматировать (Programs/shell сейчас так делают); затем
  per-pane namespace-at-load identity, включая сбой generation посреди
  listing и stale listing до открытия действия. Текущий M6.4 защищает
  snapshot от изменений ПОСЛЕ открытия, не закрывает эти будущие cases.

- Далее без остановки: M6.5 старый shell RAM cp → COW/no-predelete/samepath
  no-op, host OOM+full-table+alias regression и native; затем truthful
  unmounted/fenced KvFS UX вместо подсказки format, плюс per-pane namespace
  at-load gate и shell vcp exact-read/path/namespace parity.

- M6.4 durable proof HEADad935c5:439 committed source SHA/52815B,
  9f13219ec29f54f9067a2f4d34b339471ce6252c375873bb9173c5dcb2657ed4.
- M6.5 shell RAM cp готов: actual forced-OOM regression воспроизвёл потерю
  target на старом code; теперь COW/no-predelete/same-name no-allocation.
  Full host+release+diskless native cp/mv +existing metadata rename/EIO
  обеих архитектур PASS; kvantos-m6-5-shell-copy-integrit-a23bc85a exit0.
  Compiler/CPU/SDK native последний полный PASS M6.4, не новый M6.5 claim.
- Далее M6.6 truthful KvFS unavailable/fenced UX + explicit validated
  mount/check вместо советов format, destructive GUI format confirmation.
  Затем per-pane namespace-at-load gate и shell vcp safety parity.

- ПАУЗА ПО ПРОСЬБЕ ПОЛЬЗОВАТЕЛЯ: «workspace почисть». Разработка
  остановлена; завершён M6.5, M6.6 ещё не реализован (только черновой
  kernel/gui_store_policy.h). Не возобновлять roadmap автоматически после
  очистки. Сохранены source/WIP, пакеты и software proof до M6.5.

- ПОЛЬЗОВАТЕЛЬ ВОЗОБНОВИЛ РАЗРАБОТКУ: «продолжай разработку». Пауза
  очистки закончена. 442 M6.5 source hashes сверены (кроме записи о паузе);
  snapshot Git drift восстановлен отдельным baseline commit f710936 без
  reset/clean/переписывания истории и без protected3chmod.
- M6.6 завершён: truthful KvFS state boot/shell/Programs, explicit
  read-only check/mount, nonzero namespace invalidation raw native mount/
  format/metadata fence; отдельный erase confirmation с Cancel/Esc/close,
  Enter не подтверждает. Host/full GPT+ownedWAL и production native
  state/rename/copy/Workshop/CPU/SDK обеих архитектур PASS. Дополнительный
  640×480 PASS; kvantos-m6-6-sdk-44dc61c9 exit0. Исходный fresh venv import
  failure (pkg_resources) и missing SDK fixture сохранены отдельно,
  test predicates не ослаблены; setuptools<81 добавлен в CI recipe.
- Далее M6.7: per-pane namespace-at-load, отказ при generation change
  посреди listing и stale listing ДО открытия действия; bounded preview/
  delete snapshots. Затем shell vcp exact-read/path/namespace parity.

- M6.7 завершён: namespace-at-load для каждой панели, discard mixed/error/
  malformed listings, stale getters/row hits/actions, bounded preview и
  delete confirmation snapshot, uncertainty без retry. Host/full/GPT/ownedWAL,
  BIOS32/UEFI64 native namespace/state/rename/copy/Workshop/CPU/SDK, 640×480
  PASS. Runner typo workshop-smoke.py (exit2) сохранён отдельно; корректный
  runtime runner exit0. Не заявляется физическая qualification.
- Следом M6.8: shell vcp bounded paths/exact-read/namespace/samepath/RO parity.
  Затем M6.9: truthful listing status для adapters, скрывающих unavailable/
  I/O как EOF; M6.7 сам не может распознать невыданную backend ошибку.

- M6.8 завершён: actual shell vcp short-read RED→GREEN; bounded absolute/
  CWD/directory target join, samepath no allocation, early RO, namespace и
  exact read/write counts, unknown write без retry/ложного Copied. Full host/
  sanitizers/GPT/owned-WAL/release/layout, native vcp/read+flush+metadata EIO/
  cold mount/RAM cp+mv/mouse copy/Workshop/CPU/SDK обеих архитектур PASS.
- M6.9 начат: подтверждён RED actual native adapter, unmounted KvFS readdir
  ошибочно EOF=0 вместо unavailable=-1. Аудит нашёл двух kernel callers:
  filemgr уже проверяет strict 0/1/error, shell vls ещё truthy-loop. FAT single
  adapter теряет negative batch result; NTFS игнорирует walk_index failure.
  Следующий шаг — явный status и обновление всех consumers, не blind retry.
- После M6.9: bounded shell vcat/vrm/vmkdir/vcd (старый sh_abs вне vcp ещё
  truncating); осторожный NTFS parser hardening отдельно, без обещания
  полной hostile-image qualification. Hardware/TT/FPU остаются OPEN.

- M6.9 завершён: strict EOF/error и batch generation/count guards, native
  unavailable KvFS, FAT single negative propagation, NTFS sticky read I/O,
  strict vls consumer. Full host/GPT/ownedWAL/FAT-perf и native BIOS32/UEFI64
  listing/namespace/state/vcp/Workshop/CPU/SDK, listing 640×480 PASS; 3 runners exit0.
- M6.10 начат: подтверждены actual vcat heap overflow при read overcount
  (ASan RED) и vrm wrong truncated path при длинном CWD (assert RED).
  Следом общий bounded shell resolver, preview read bounds/namespace и
  mutation outcome guards. Не останавливать автономную очередь.

- M6.10 завершён: actual vcat overcount ASan RED→GREEN; длинный CWD wrong
  remove RED→GREEN; общая bounded shell path, exact capped read/namespace,
  delete/create uncertainty и safe cd. Host/full host + BIOS32/UEFI64 real
  ATA read/delete EIO shell path, vcp/listing/state/namespace, Workshop/CPU/SDK
  PASS. Initial combined SDK runner exit1 missing boxes fixture сохранён отдельно;
  explicit SDK all/runtime и dedicated replay PASS.
- M6.11 завершён по software scope: NTFS index-entry/attribute/runlist/UTF16
  bounds, USA/node sentinels и strict errors; 50k decoder +20k parser mutations
  ASan/UBSan, full host/GPT/ownedWAL/FATperf, release/layout PASS. Native
  BIOS32/UEFI64 regressions PASS; synthetic NTFS host fixture, не real NTFS
  volume. Далее M6.12: NTFS logical sector geometry ↔ 512-byte block API: RED
  подтвердил 4KiB NTFS sector читается одним 512-byte sector, не восемью.
  Добавить conversion и partition bounds; physical qualification не заявлять.

- M6.11 завершён/commit 65d4c7e: bounded NTFS attribute/index/runlist/UTF16
  parser; full host/ASanUBSan/GPT/owned-WAL/FATperf/release/layout; native
  BIOS32/UEFI64 shell/storage/apps + 640×480 listing PASS. NTFS fixture
  synthetic, physical/real image qualification OPEN.
- M6.12 завершён по software scope: NTFS logical sectors 512/1024/2048/4096
  переводятся в 512-byte block I/O; volume/partition/device-LBA и MFT/cluster
  arithmetic bounds, actual synthetic adapter fixtures. Полный MCC host/
  ASan/UBSan, GPT22, owned-WAL 1696 cuts, FAT-perf, release/layout/11 apps и
  BIOS32/UEFI64 shell/vcp/listing/namespace/KvFS-state/CPL3/isolated-Workshop
  PASS. GCC12 release PASS; GCC14 ICE зафиксирован, не замаскирован. Реальные
  NTFS образы/physical 4Kn не квалифицированы. Результаты: `docs/M6.12-NTFS-
  SECTOR-BOUNDS.ru.md`, `build/autonomous/m612-*-native.log`,
  `build/autonomous/m612-kernels.sha256`.
- M6.13 software complete: private regular-file ntfs-3g images 128MiB, logical
  sector 512/1024/2048/4096. Independent ntfscat + actual NTFS/VFS adapter
  listing/read exact payload, RO denial, zero writes, unchanged full-image SHA
  PASS. 4Kn image revealed USA stride=512; production fix and USA unit fixture
  PASS. Full MCC/compiler/ASanUBSan, GPT22, owned-WAL1696 cuts, FAT-perf,
  GCC12 release/layout and BIOS32/UEFI64 shell/vcp/listing/namespace/state/CPL3/
  Workshop process `ntfs-bios-uefi-regression-b2f47b29` exit0 PASS. No host mount
  or physical media. Docs `docs/M6.13-NTFS-IMAGE-QUALIFICATION.ru.md`.
- M6.14 завершён: actual NTFS volume embedded at MBR LBA 2048 на private file,
  512/4096 bps: VFS start LBA, root/list/read, RO, write=0, full image SHA unchanged
  PASS; too-long MBR extent отказывает без raw alias/OOB read/write. Реальные
  NTFS 512/1024/2048/4096 raw fixtures продолжают PASS. См.
  `docs/M6.14-NTFS-PARTITION-OFFSETS.ru.md`; GPT-offset and complex NTFS OPEN.
- M6.15 завершён по software scope: реальные ntfs-3g private regular images с
  resident proof и 1MiB nonresident `$DATA` по Unicode root filename; `ntfsinfo`
  подтвердил nonresident, `ntfscat` и production adapter byte-exact PASS на
  512/1024/2048/4096. MBR offset 2048 + overrun rejection на 512/4096 также
  покрывают оба файла; RO/zero writes/full SHA invariants PASS. GPT22,
  owned-WAL 1696 interruptions/recovery cuts, FAT-perf PASS. См.
  `docs/M6.15-NTFS-NONRESIDENT-UNICODE.ru.md`.
- M6.16 завершён: actual ntfs-3g resident/nonresident image reads по ненулевому
  offset через sector/cluster boundary; короткий хвост у EOF и чтения на/после
  EOF проверены для raw 512/1024/2048/4096 и MBR-offset 512/4096. RO, zero writes,
  no-OOB, unchanged full-image SHA PASS. См. `docs/M6.16-NTFS-PARTIAL-READ-BOUNDS.ru.md`.
- M6.17 завершён в bounded scope: ntfs-3g regular-file image имеет настоящий
  1MiB nonresident `$DATA` с двумя несмежными runs; `ntfsinfo` подтверждает
  Initialized Size=4096, `ntfscluster` проверяет runlist. Poison `0xA5` в
  allocated/uninitialized хвосте возвращается нулями независимым `ntfscat` и
  production adapter; старый путь не учитывал Initialized Size. Fix zero-fills
  хвост, RO/zero writes/no-OOB/full SHA PASS. Whole NTFS image suite под ASan/UBSan,
  MCC VFS/parser, GPT22 + owned-WAL1696 recovery cuts + FAT perf и clean GCC12
  `all` для BIOS32/UEFI64/EFI PASS. QEMU/OVMF недоступны в текущей среде, runtime
  на обновлённом kernel не заявляется. См. `docs/M6.17-NTFS-FRAGMENTED-INITIALIZED-DATA.ru.md`.
- M6.18 завершён: `parse_boot` читает оба DWORD 64-битного NTFS `total_sectors`
  и отвергает ненулевой high word, чтобы не монтировать low-word alias поверх
  32-bit block API. Private image high-word mutation: 0 volumes, no-OOB/zero-write,
  image SHA unchanged. Полные NTFS image, MCC parser/VFS, GPT22, owned-WAL1696/FAT
  perf и clean GCC12 BIOS32/UEFI64/EFI compile PASS. См.
  `docs/M6.18-NTFS-64BIT-SECTOR-COUNT.ru.md`.
- M6.19 завершён: Initialized Size=1234 пересекает первый 4096-byte cluster
  внутри его data; весь хвост poison `0xA5`. До фикса full/offset read RED;
  читаемый span теперь ограничен `data_want-done`, затем zero-fill. Independent
  `ntfscat` и actual adapter совпали byte-exact, image SHA unchanged. Полные
  NTFS normal + ASan/UBSan, MCC, GPT22/WAL1696/FAT perf и clean GCC12
  BIOS32/UEFI64/EFI compile PASS. См.
  `docs/M6.19-NTFS-SUBCLUSTER-INITIALIZED-SIZE.ru.md`.
- M6.20 завершён: 32-bit VFS не должен принимать NTFS `$DATA` размером
  `0x100000000` за пустой файл. Private 128 MiB ntfs-3g image + `ntfstruncate`
  подтвердили 4-GiB data size при low-word `$FILE_NAME`; RED на предыдущем
  committed driver показал доступный stat и отсутствие отказа при listing.
  Исправленный adapter проверяет high DWORD по MFT, отказывает path/read/size и
  целевой dirent, а `read_attr()` имеет независимую защиту. Actual NTFS matrix,
  4-GiB fixture (RO, no-OOB, zero writes, same SHA), MCC parser/VFS ASan/UBSan и
  GPT/FAT regressions PASS. Файлы >4 GiB не поддерживаются; runtime QEMU/OVMF не
  запускался. См. `docs/M6.20-NTFS-64BIT-FILE-SIZE-REFUSAL.ru.md`.
- Далее — bounded review очередного storage/parser safety края, только после
  конкретного воспроизводимого RED; не расширять драйвер ради галочки. Другие
  fragment patterns/multi-runs, nested Unicode, GPT-offset, Windows/dirty/
  sparse/compressed и physical 4Kn остаются OPEN; `/dev/*`, NTFS writes/repair и
  host mounts запрещены.
