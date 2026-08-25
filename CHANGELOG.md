# Changelog

## 2.0.0 "Quantum"

### System

* **Setting the clock.** The CMOS real-time clock is no longer read-only:
  `time set HH:MM:SS` and `date set DD.MM.YYYY` write the clock and the
  calendar back to the RTC, honouring the BCD/binary format the chip is
  configured for and validating ranges (leap years included).
* **Persistent settings.** Choices made in a session now survive the
  reboot: they are stored as a plain `settings.cfg` on the KvFS disk and
  loaded right after the disk is mounted. The interface language
  (`lang ru` / `lang en`) is the first persisted setting; the store is
  ready for more keys. Without a disk everything degrades gracefully.
* Version bump everywhere: the kernel reports **2.0.0 "Quantum"**
  (`about`, `uname`), the boot menu and the release archive are renamed
  accordingly (`kvantos-2.0.0-quantum.tar.gz`).

### Shell (kvsh)

New text utilities, all working on ramfs files:

* `sort FILE` - print the lines of a file sorted;
* `uniq FILE` - collapse adjacent duplicate lines;
* `tac FILE` - print a file line by line, backwards;
* `basename PATH` / `dirname PATH` - split a path;
* `repeat N TEXT` - print text N times (capped at 200).

### Applications

Ten applications instead of eight:

* **Mines** - Minesweeper: 16x12 field, 24 mines, flags, a keyboard
  cursor for mouseless machines, flood opening of empty areas;
* **2048** - the sliding-tiles puzzle with score and best-score tracking.

### Boot images

* The ISO build no longer fights the 0x78000-byte core image limit of
  i386-pc: the kernel, the menu and the applications live on the ISO as
  ordinary files while every GRUB module stays embedded in core.img.
* A GRUB-free fallback appeared: `make iso-direct` builds a bootable ISO
  with a tiny self-contained Multiboot loader (boot/direct.asm) and the
  vendored pure-Python pycdlib - useful where the GRUB toolchain is
  unavailable.

## 0.1.0 "Photon"

The first public release: the kernel, the kvsh shell, the graphics
desktop with windows and a file manager, KvFS/FAT32/NTFS support, eight
applications and installation onto a hard disk (`setup`).
