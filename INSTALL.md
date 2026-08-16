# How to load your own application into KvantOS

*Read this in [Русский](INSTALL.ru.md).*

A short guide for the case "I already have a `.kapp` file".

---

## Way 1: embed it into the boot image (works everywhere)

Suitable for **VMware**, VirtualBox and a real computer. No hard disk needed.

```bash
cd /home/user/kvantos
./sdk/addapp.sh /path/to/snake.kapp
```

The script puts the application inside the ISO, writes it into the bootloader
menu and rebuilds the images. You get:

```
release/kvantos.iso          <- attach this to the machine
release/kvantos-floppy.img   <- the fallback option
```

Then in VMware: **VM → Settings → CD/DVD → Use ISO image** and pick
`kvantos.iso`. Boot it.

Inside the system:

```
kvant:/$ ls              <- your .kapp is already here
kvant:/$ guimenu         <- graphics mode
```

Press **G** (or the Programs icon) → your program is in the list → clicking the
row launches it.

Several applications at once:

```bash
./sdk/addapp.sh snake.kapp tetris.kapp paint.kapp
```

---

## Way 2: when you do need a disk

A disk is needed **only** so that applications can save their own files
(settings, documents). Launching them does not require one.

### In VMware

1. **VM → Settings → Add → Hard Disk → SCSI?** — no, choose **IDE**. This
   matters: the driver speaks IDE/ATA and the system will not see a SCSI disk.
2. The size — 64 MB or more, which is plenty.
3. Boot and format it once:

```
kvant:/$ disk            <- check that the disk is visible
kvant:/$ format          <- format it (ERASES everything on the disk)
kvant:/$ df              <- confirm the filesystem appeared
```

4. Move the application from the image onto the disk:

```
kvant:/$ install snake.kapp
kvant:/$ apps
```

It will now stay on the disk across reboots.

### In QEMU

```bash
qemu-system-i386 -cdrom release/kvantos.iso \
                 -hda release/kvantos-disk.img -m 256
```

---

## Checking that everything is right

A `.kapp` file must start with the `KAPP` signature:

```bash
head -c 4 snake.kapp        # should print: KAPP
```

If the signature is missing, the file never went through the packer. Build it
like this:

```bash
cd sdk
# put the source into sdk/apps/, then
make
# the finished application appears in sdk/build/
```

---

## Frequently asked questions

**The program is not in the list.** Check `ls` in the shell: if the file is not
there, the image was rebuilt without it — run `addapp.sh` again and make sure
you attached the **fresh** ISO, not an old one from the history.

**"No ATA disk found".** That is not an error: without a disk the applications
still run from the image. A disk is only needed to save files. In VMware add a
disk of type **IDE**, not SCSI.

**The application does not start and the window states a reason.** The most
common one is being built against a different ABI version: rebuild against the
current `include/kvapp.h`. The other reasons are listed in [APPS.md](APPS.md),
section "Error reference".

**How to write my own application.** The full guide is [APPS.md](APPS.md). It
covers the file format, every system function, examples and an error walkthrough.
Commented samples live in `sdk/apps/`: the clock, the notes editor and the snake.

---

## Installing the system itself onto a hard disk

This page is about applications. If you want KvantOS itself to live on a hard
disk and boot from it, see the "Installing onto a hard disk" section of the
[README](README.md): boot from the ISO or the floppy and run `setup --yes` (or
press the **Install** button in the Programs window).
