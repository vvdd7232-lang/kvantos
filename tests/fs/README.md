# Filesystem driver tests

The FAT32/FAT16 and NTFS drivers reach the disk through exactly two
functions, `ata_read` and `ata_write`. That makes them testable on an
ordinary Linux machine: `harness.c` implements those two against a file
and the very same driver sources are compiled for the host. Finding a
bug takes a second instead of a minute-long boot in an emulator.

```sh
make image     # build a disk image with FAT32 + NTFS (uses sudo for mkntfs)
make check     # run the assertions
```

The image is deliberately built with the *system* tools — `mkfs.vfat`
and `mkntfs` — so the drivers are validated against filesystems produced
by somebody else's implementation rather than our own.

What the tests cover:

* autodetection through the MBR partition table
* FAT32: long names, subdirectories, multi-cluster reads, reads at an
  offset, writing files up to 300 KB, `mkdir`, deleting, refusing to
  delete a non-empty directory, and not leaking clusters when a file is
  overwritten repeatedly
* NTFS: resident and non-resident `$DATA`, a 900 KiB file compared
  byte-for-byte against the original, a 120-entry B-tree directory,
  UTF-16 names, and that every write is refused

After a run, `fsck.vfat -n` on the FAT partition should report no errors
and the NTFS partition should be byte-for-byte unchanged.

One subtlety worth keeping in mind: `ata_read` returns **0 on success**,
not 1. An early version of this harness had it backwards; the tests all
passed, and the system then failed to find a single volume when it was
actually booted.
