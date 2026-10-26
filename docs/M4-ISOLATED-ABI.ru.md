# M4.1 — контракт изолированных приложений, проект

> Дополнение: experimental format2 **protocol2** теперь добавляет shared ARGB
> surfaces с read-only front и публикацией на COMPLETE. Protocol1 ниже сохранён;
> новый путь и его ограничения описаны в
> [PROCFS-SHM-MODULES.ru.md](PROCFS-SHM-MODULES.ru.md#реализовано-экспериментальные-shared-surfaces-gui-protocol2).


Дата: 17.10.2026. **Исторический проект M4.1, не frozen binary ABI.**
Обновление: M4.2–M4.6 реализованы отдельными bounded increment’ами; см.
`M4.2-ADDRESS-SPACES.ru.md`, `M4.4-BOUNDED-SERVICES.ru.md` и
`M4.5-M4.6-RUNTIME.ru.md`. Ниже сохранено исходное обоснование и план,
а не актуальная таблица реализованных функций. Workshop по умолчанию ABI3.
Нельзя маркировать старые `.kapp` как безопасные или недоверенные программы как изолированные.

## Исходное положение

`kernel/kapp.c` вызывает kapp_main и callbacks в ring0; `include/kvapp.h`
ABI2/3 передаёт прямую таблицу kernel function pointers и native pointers.
Guard/longjmp ловит часть CPU exceptions, но не мешает записи в kernel memory,
CLI/OUT, DMA/MMIO, бесконечному циклу или порче стека. Это не security boundary.
`kernel/gdt.c` уже имеет user descriptors и TSS/rsp0/esp0. Это полезная заготовка,
не доказательство запуска user tasks. `kernel/paging.c` использует общую identity
map; наличие PAGE_USER constant само по себе не даёт отдельных address spaces.
Текущий fixed KAPP arena: 14–16 MiB; format1, один образ, compiler ABI3.

## Версии и совместимость

- ABI2/3 и format1 не менять. Старые SDK/compiler и delta_ms работают как прежде.
- Предлагаемые format2 + ABI4 требуют отдельного loader path, явного признака
  isolated mode и section metadata. До готовности loader такие файлы отвергает.
- Legacy apps обозначать «доверенное приложение / kernel mode». Не делать
  молчаливый fallback ABI4 → ring0 при ошибке проверки или нехватке памяти.
- Не загружать ABI3 и ABI4 одновременно в пересекающийся arena. На первом
  этапе одно user app, но отдельные address space, kernel stack и ownership.
- Формат заголовка и syscall numbering заморозить отдельным implementation
  checkpoint с layout/static-assert tests; этот документ ещё не binary ABI release.

## Память и загрузка

Первые ABI4 приложения могут сохранять virtual load base 0x00e00000 и 2 MiB
верхнюю границу arena, но physical frames выделяются каждому process отдельно.
Section table: bounded count, offsets/lengths без overflow, непересечение,
entry внутри executable section, architecture/ABI соответствуют kernel.
Zero-fill BSS и выделенные frames до user access; disk buffer/PMM/kernel не
маппить user aliases. Код RX, data/heap/stack RW, guard page у user stack;
W^X/NX проверяются по CPU capability, ограничения i386 без NX отражаются явно.
На каждой ступени page walk правильные U/S bits; supervisor mapping не должен
получать user access через большие identity pages. Нужны split страниц и TLB
invalidation. Kernel, MMIO, DMA, framebuffer и page tables — supervisor only.

У процесса отдельные user stack и kernel entry stack с guard/ownership;
TSS esp0/rsp0 обновляется при switch. IRQ entry/exit и exception frames
поддерживают CPL3→0 и обратно на обеих архитектурах; сохраняют все регистры.
IOPL=0, I/O bitmap запрещает порты, user не может CLI/HLT/менять CR3.
Переход через проверенные CS/SS, canonical x64 RIP/RSP и разрешённые flags.

## Transport v1 (предложение)

Для первого cross-architecture ABI — выделенный DPL3 `int 0x80` gate;
на i386 и x64 один register contract: EAX=call id, EBX=request offset,
ECX=request size; EAX=bounded signed result/negative error. В x64 upper halves
входных указателей/размеров не игнорируются: nonzero → EINVAL. SYSRET/SYSCALL
оптимизацию отложить; не переписывать существующие interrupt frames вслепую.

Wire packets — фиксированные u32/i32 fields, без native pointers, padding,
varargs, kernel structs и function pointers. Все buffer/string references —
32-bit offset + length в user arena; nested descriptors валидируются отдельно.
Один request не более 4096 bytes, file payload chunk не более 4096 bytes;
длинные операции имеют bounded chunk/event semantics, а не user-owned DMA.
Unknown id/version/size/flags → стабильная ошибка без побочных эффектов.

## copyin/copyout и TOCTOU

`offset <= arena_size && length <= arena_size-offset` до сложения;
проверить user pages целиком, доступ read/write, запрет kernel aliases,
canonical address и отсутствие overflow. Length=0 имеет явное определение
для каждого call, не освобождает от проверки самого request header.
Весь request и bounded strings копируются в kernel-owned storage один раз.
Не читать flags/path/length повторно из изменяемого user buffer. Mapping
прикреплён к процессу на время операции; termination не освобождает его до
завершения references. Ошибка copyout не превращает уже выполненный WRITE в
разрешение повторить его. Никаких disk locks/IRQ disable вокруг user access.
Kernel не вызывает user function pointer и не доверяет возвращаемому descriptor.

## Surface API

- EVENT_WAIT/COMPLETE/EXIT: kernel передаёт POD events (draw/key/click/tick/close),
  event id + generation. User loop выполняет собственные callbacks в ring3.
  COMPLETE принимает только текущий token, не kernel return address. Tick содержит
  capped delta_ms, совместимую по смыслу с ABI3; не зависит от частоты redraw.
- Рисование: bounded command packets, clipping на kernel side в client rectangle;
  нет framebuffer pointers. Invalid coords/counts/oversized batches отвергаются.
  Budget/dirtyrect/cache сохраняются; нельзя удержать desktop большой командой.
- `format`, string/memory helpers и allocator — user-side library, не перенос
  privileged varargs ABI через gate. Heap quotas и mapping ownership у процесса.
- File API: process-scoped opaque handle с generation/rights, bounded paths,
  explicit counts; не передавать disk pointers, FAT structs или kernel allocation.
  Сначала RAM/KvFS-compatible sandbox. Raw media/write grants не выдавать ABI4.
  Default RO, WAL, opt-in/generation для kernel GUI остаются независимо от ABI.
- Clock/lang/random/log/beep: POD/bounded copy, rate limit для внешних эффектов;
  app не управляет IRQ, PMM, PCI/USB, speaker registers или scheduler напрямую.

## Faults, scheduler и lifecycle

User #PF/#GP/#UD/#DE → terminate **только процесс**, stable reason, shell жив.
Kernel fault внутри syscall не маскировать старым in_app longjmp. Нужна явная
разница fault-on-copy и kernel bug. Cleanup отменяет events, отзывает handles,
ожидает outstanding I/O references, освобождает frames/heap/stack ровно один раз.
Kill, close callback, fault и failed load используют одну ownership state machine.

Ring3 без preemption не защищает от `while(1){}`. Timer возвращает управление
scheduler, kernel UI продолжает работать; app не удерживает kernel locks.
CPU/time/memory/file/draw quotas ограничены до допуска недоверенных приложений.
FPU/SIMD не обещать до save/restore/context policy и исключений; compiler floats
не добавлять раньше этого. Запрет инструкции только словами не является защитой.

## Этапы реализации и gates

1. M4.2: address-space primitives + host page-walk/range tests, no runtime claims.
2. M4.3: один minimal user task / return / IRQ / exception на BIOS32 и UEFI64,
   guards, TSS и teardown. Privilege/memory-negative QEMU fixtures.
3. M4.4: syscall gate + bounded copies + user event loop / draw / RAM file handles.
   Ещё не включать arbitrary untrusted apps без следующего gate.
4. M4.5: preemptive kill, quotas, fault isolation и deterministic cleanup.
5. M4.6: SDK/compiler opt-in ABI4, dual-loader regression ABI2/3, 11 apps,
   games/500Hz/delta_ms/dirtyrect/editors, reconnect/WAL unaffected.

Acceptance обеих архитектур: read/write kernel/null/guard/other process;
CLI/OUT/HLT/bad CS/noncanonical return; syscall cross-page buffers, wrap,
readonly copyout, unterminated/oversized strings, stale handles, changed request;
OOM/failed load/kill during pending I/O/repeated close; `while(1)` не блокирует UI;
no frame/handle leak после многократного запуска. ASan host tests недостаточны
для privilege proof; нужны native QEMU CPU tests, затем hardware qualification.

**Исторический статус на момент M4.1:** design checkpoint; implementation тогда OPEN.

**Актуализация:** M4.2–M4.15 software implementation/proof описаны в профильных
документах и ROADMAP. Opt-in ABI4 исполняется в CPL3; default ABI2/3 остаётся
trusted ring0. Private files не persistent grants. FP context/float и physical
qualification не закрыты. Этот исходный design не подменяет текущий wire contract.
