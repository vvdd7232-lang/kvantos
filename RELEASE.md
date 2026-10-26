# KvantOS 3.5.1 — procfs and shared GUI

October 26, 2026. **BIOS/i386** and **UEFI/x86-64**.
Published 3.4.0 and earlier releases remain unchanged.

## Direct BIOS boot fix

The earlier 3.5.0 attempt failed boot acceptance and was not published; its tag
has not moved. 3.5.1 separates E820/VBE firmware buffers and the protected-mode
stack from the direct loader's kernel/app bounce area, fixes mem_upper, and
reserves boot modules before page-table allocation. The builder rejects memory
collisions; regressions execute the actual E820 instructions and compare every
embedded app's guest RAM bytes with the original files.

## Highlights

- Read-only `/proc`: CPU, PMM memory, uptime/ticks and kernel-task status.
  Use `cat /proc/meminfo` or `cat /proc/0/status`. The system mount survives
  rescans and does not consume one of the eight storage slots.
- IRQ-protected scheduler metadata copies and bounded task-ring traversal;
  fix task_count nontermination on a ring of dead tasks.
- Opt-in experimental **ABI4 protocol2 shared ARGB8888 surfaces**. The client
  writes back; COMPLETE publishes it as read-only front. The compositor reads
  the same physical frames rather than copying a pixel array through messages.
- Reference-counted frame ownership, alias protection, final-release zeroing,
  CPU alpha blending, and `pixels32.kapp` / `pixels64.kapp` CPL3 demos.
  Q exits; F deliberately writes front to exercise user page-fault cleanup.
  Find the demos in `sdk/isolated/build/` in the distribution or as separate
  downloads; add one to KvFS and open it with Program Manager.

## Limits and compatibility

Surfaces are currently **420×300**, clipped rather than resized with the window.
No resize handshake, dirty regions, GPU acceleration or measured FPS gain is
claimed. Dynamic kernel modules are **not implemented**.

Proc entries describe kernel tasks, not POSIX processes. VFS has no per-open
snapshot yet: read the entire file in one call for consistent contents.
ps/mem/Monitor and ring3 file services have not migrated to procfs.
Bare RAMFS cat, trusted ABI2/3 and ABI4 protocol1 remain supported; protocol2
requires explicit format2 opt-in. Breeze remains the desktop theme. Existing
FAT/WAL write policy and USB/AHCI/NTFS restrictions are unchanged.

The implementation uses ToaruOS architectural ideas, not incorporated upstream
source. ToaruOS is NCSA-licensed; see `docs/PROCFS-SHM-MODULES.ru.md` for provenance.

## Release acceptance

Publication is gated on tagged-source builds, host ASan/UBSan tests, image
validation, BIOS/UEFI CD/USB boot, desktop, procfs, CPU isolation, shared surfaces
(including front-write refusal and frame cleanup), protocol1 compatibility,
and quick GPT/NTFS host checks.

Long repeated editor/Workshop, AHCI/USB/WAL, Save As and GPT/MBR import matrices
are removed from the release critical path, not from the repository. Extended
regressions remain in ordinary build CI; its overall result is not a separate
publication gate. The smaller acceptance set runs against this exact tag.
This is not physical-hardware qualification or a performance claim.

## Artifacts

- `kvantos-3.5.1.tar.gz` — distribution, images, kernels, apps, SDK and demos;
- `kvantos-3.5.1-source.tar.gz` — tagged sources;
- `kvantos.iso`, `kvantos-direct.iso`, `kvantos-floppy.img`, `kvantos-disk.img`;
- `kvant.bin`, `kvant64.bin`, `kvantefi.efi`, `esp.img`;
- `pixels32.kapp`, `pixels64.kapp` — experimental shared-surface demos;
- `kvantos-3.5.1-SHA256SUMS.txt` — SHA-256 checksums.
