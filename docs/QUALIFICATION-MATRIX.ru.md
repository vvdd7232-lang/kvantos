# Матрица software / physical qualification

Дата статуса:2026-10-17. Локальная development ветка; опубликованный stable3.1.3
не заменяется и не переименовывается. PASS ниже относится к указанному scope,
не к «полной поддержке любого оборудования» и не к security certification.

| Область | Подтверждённый software scope | Что не закрыто |
|---|---|---|
| BIOS32/i386 + UEFI64/x86-64 | NORMAL release, GRUB/direct images; QEMU native suites | Произвольные firmware/physical machines |
| Native C compiler | M5.1–5.4 arrays/sizeof/macros/structs/typedef/virtual headers; оба targets | ISO C, unsigned/stdint/libc, float/double |
| Compiler budgets | M5.5:80 checks/arch, guards/tables/source16000/row15001, ASan/UBSan | Увеличение source до editor65535, рост tables/stack |
| ABI4/CPL3 | Private spaces/syscalls/events/preemption, strict image loader, fault/cleanup suites | SMP/concurrent multi-user, independent security audit |
| ABI4 graphics/text/status | Bounded rect/text/line replay, quotas/UTF-8, native adapter fail-stop31..34 | Общий framebuffer/device grant, arbitrary async drawing |
| ABI4 files | Только own private8×1024B, checked handles/LIST | Persistent/global KvFS/FAT grants |
| ABI4 clock | Readonly cached H/M/S, zero user-triggered CMOS I/O; native comparison | Time setters/date/timezone/precision/physical RTC qualification |
| Legacy ABI2/3 | Default Workshop/11 apps, dual-loader/native regression | Это trusted ring0, НЕ изолированные приложения |
| FPU/SIMD | Default deny; отдельный safety design, FP context не включён | Save/restore/owner/exception/vendor gates; compiler floats |
| Editors/UX | Packed text<64KiB,120B/line, >128 warning only; mouse UI/NumPad MouseKeys | Все physical keyboard/mouse/device combinations |
| KvFS/FAT/WAL | Software COW/owned WAL/recovery/fsck/power-cut fixtures по milestone docs | GeneralUDisk/произвольные пользовательские диски не test fixtures |
| USB UHCI/OHCI/EHCI roots | Host models + software native import/save/reconnect matrices | M2 physical USB/VirtualBox/companion qualification |
| EHCI HS hubs/FS leaf через TT | M35a–c experimental private DMA/topology/native fault/recovery/WAL fixtures | Исторический Save64 refusal, stuck-TT recovery, LS/HID/periodic/FS hubs/multiTT alt1 |
| AHCI SATA | M31 RO +M32 opt-in owned WAL/flush/recovery software | Physical SATA/installer/KvFS/hotplug rediscovery |
| NTFS/GPT | NTFS RO: ntfs-3g private images at 512/1024/2048/4096; resident + verified nonresident 1MiB Unicode root file; MBR offset 2048 at 512/4096; exact full/offset/tail/EOF reads; one verified 1MiB/two-run nonresident fixture, Initialized Size=1234 inside a 4096-byte cluster with poisoned suffix zero-filled; high-DWORD total-sector volumes and a 4-GiB `$DATA` file fail closed instead of 32-bit aliasing; RO/zero-write/SHA and extent refusal | GPT offset, nested dirs, additional fragment/run shapes, Windows/dirty/compressed/sparse interoperability, >4-GiB files and physical 4Kn OPEN |
| Future drivers | Roadmap only | xHCI/NVMe/NIC stack/AC97-HDA не объявляются готовыми |

## Где искать проверяемые результаты

- Текущие команды/commit и ссылки на профильные docs: AUTONOMOUS-QUEUE.ru.md.
- Runtime/адаптеры: M4.10–M4.14 proof docs; FP prerequisite:
  FPU-CONTEXT-SAFETY.ru.md. Негативный clock caller pointer — #PF proof,
  не native proof unavailable-cache exit35.
- Compiler: M5.1–M5.5 docs; source budgets не равны editor storage limits.
- Storage: M35D-DIAGNOSTICS.ru.md и TT-RECOVERY-SAFETY.ru.md. Поздние успешные
  Save64 не объясняют исторический отказ и не снимают hardware gates.
- build/* result.json перезаписываются следующим запуском. Для durable proof
  используются компактные source-SHA/log archives в workspace-checkpoints;
  hashes записаны в очереди/профильных proof docs.
- Локально выполненные команды и hosted CI — разные утверждения. Workflow
  configuration не является run success. Push/tag/publication не выполнены.

Новый отчёт должен добавлять точную configuration + scope + evidence, а не
заменять OPEN общей фразой «поддерживается». Hardware gate закрывается только
реальным согласованным hardware test, а не новой строкой в этой таблице.
