# Working test screenshots

Frames captured from QEMU while debugging. Diagnostic drafts: they include
failed attempts and shots of the bugs themselves.


## Applications and the disk

| File | What it shows |
|---|---|
| `app_apprun3_1_store.png` | The store before launching |
| `app_apprun3_2_clicked.png` | Clock launched from disk |
| `app_apprun3_3_launched.png` | The clock is running |
| `app_apprun3_4_dark.png` | Key D — the dark theme |
| `app_apprun3_5_nosec.png` | Key S — without the second hand |
| `app_appt_1_boot.png` | Booting with an ATA disk |
| `app_appt_2_disk.png` | The disk command: the disk was found |
| `app_appt_3_apps.png` | Bug: apps printed %-24s (kprintf has no alignment) |
| `app_appt_4_gui.png` | Graphics mode |
| `app_appt_5_store.png` | The Programs window with the list |
| `app_appt_6_store2.png` | The same list |
| `app_crashapp2_1_store.png` | crashapp2 1 store |
| `app_crashapp2_2_crashapp.png` | crashapp2 2 crashapp |
| `app_crashapp2_3_after_crash.png` | Fixed: the application was removed, the system is alive |
| `app_crashapp2_4_alive.png` | The shell responds after the crash |
| `app_crashapp_1_store.png` | crash1 store |
| `app_crashapp_2_crashapp.png` | crash2 crashapp |
| `app_crashapp_3_after_crash.png` | crash3 after crash |
| `app_crashapp_4_alive.png` | crash4 alive |
| `app_notes_1_notes.png` | notes 1 notes |
| `app_notes_2_typed.png` | Notes: typing text |
| `app_notes_3_saved.png` | Ctrl+S — saving to disk |
| `app_notes_4_reloaded.png` | The text was restored after a reboot |
| `app_persist_1_install.png` | install: the file was moved onto the disk |
| `app_persist_2_dls.png` | persist 2 dls |
| `app_persist_3_after_reboot.png` | The file is still there after a reboot |
| `app_persist_4_dcat.png` | dcat reads a file from the previous session |
| `app_persist_5_apps.png` | persist 5 apps |
| `app_reg_1_pc_disk.png` | Regression: pc + disk |
| `app_reg_2_pc_floppy_nodisk.png` | Regression: floppy without a disk |
| `app_reg_3_q35_disk.png` | Regression: q35 + disk |
| `app_reg_4_isapc_nodisk.png` | Regression: isapc without a disk |

## Earlier tests (frame rate, the editor)

See the files without the `app_` prefix.

