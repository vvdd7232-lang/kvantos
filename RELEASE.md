# KvantOS 3.1.1 — C Workshop

October 14, 2026. Corrective release after 3.1.0: **the C Workshop is now actually included in both kernel images**.

## Opening it

Boot the new **v3.1.1 `kvantos.iso`**, enter `guimenu`, then press **B** or open the **C Workshop** desktop icon. Icons wrap into a second column on shorter screens. The built-in `hello.c` is ready to build with **F5** or rebuild/run with **F6**. Clicking inside the generated application's window moves its square through a native C callback.

## Added

- An independent source editor with line numbers, scrolling, Home/End/Delete/Tab and no shared Notepad document state.
- Native compilation of a limited C subset **inside the OS**, without a host GCC, network or external tools.
- **i386 and x86-64 backends**, selected by the running kernel. The 64-bit backend implements native-width pointers, SysV callback wrappers and aligned API/variadic calls.
- **Ctrl+S** saves, **F2** opens by filename, **Ctrl+U** clears the filename, **F1** shows help and **Ctrl+N** restores the template.
- RAM source/application storage plus persistent copies on mounted KvFS. F2 reopens persisted source after a reboot. Diskless mode explicitly warns that files are temporary.
- Compilation error line reporting; F6 never runs stale output in place of a failed build.
- Atomic RAMFS replacement and safe handling of exhausted window slots on `.kapp` launches.

See [the Workshop guide](sdk/WORKSHOP.md).

## Validation

- **73 generated-code execution checks per backend**, 146 total. x86-64 tests use a stack above 4 GiB and verify pointers, SysV alignment and callee-saved registers.
- **40,000 input mutations per backend**, 80,000 total, under ASan/UBSan, plus atomic RAMFS save/OOM regressions.
- Both QEMU BIOS/i386 and UEFI/x86-64 guests: compilation, native execution/clicks, independent editors, error paths, KvFS persistence across a full reboot and Russian diskless build/run.
- Both guests additionally compile/run locals, double pointers, heap allocation, an eight-argument function and variadic `format`.
- BIOS/UEFI CD boot, BIOS/UEFI USB boot and native Maze 3D regressions from 3.1.0 remain enabled.

GCC 12 is used to build this release. Tests are shipped with the sources and run by the release workflow before publishing.

## Downloads and running

- **`kvantos.iso`**: recommended BIOS/UEFI image and raw-USB hybrid.
- **`kvantos-3.1.1.tar.gz`**: full distribution with both ISOs, floppy, kernels, EFI loader, ESP, empty KvFS disk, eleven SDK apps per architecture, docs and SHA256SUMS. The Workshop is built into the kernel, not shipped as a twelfth SDK app.
- **`kvantos-direct.iso`**: GRUB-free BIOS/UEFI CD/DVD fallback, not a raw-USB hybrid.
- **`kvantos-3.1.1-source.tar.gz`**: matching sources; **`kvantos-3.1.1-SHA256SUMS.txt`**: download checksums.

Extract the full distribution. BIOS example:

```sh
qemu-system-i386 -m 256 -boot d -cdrom kvantos.iso \
  -drive file=kvantos-disk.img,format=raw,if=ide
```

UEFI example (Secure Boot disabled):

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

Use `lang ru` for Russian. Run `sha256sum -c SHA256SUMS` inside the extracted distribution to verify its files.

## Limitations

Not full ISO C or GCC: no preprocessing, structures, typedefs, floating point, standard library, array declarations or complete type checking. Existing `sys->...` SDK examples need adaptation. Source is portable between the two backends; generated `.kapp` binaries are architecture-specific.

This is an experimental OS. Applications run in **ring 0 without full isolation**: invalid pointers or infinite loops can corrupt memory or hang the system. Read INSTALL.md and back up data before real-disk installation.

License: MIT.
