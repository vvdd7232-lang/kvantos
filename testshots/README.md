# Рабочие скриншоты тестов

Кадры из QEMU, снятые во время отладки. Черновики диагностики:
есть и неудачные попытки, и снимки багов.


## Приложения и диск

| Файл | Что на нём |
|---|---|
| `app_apprun3_1_store.png` | Магазин перед запуском |
| `app_apprun3_2_clicked.png` | Часы запущены с диска |
| `app_apprun3_3_launched.png` | Часы работают |
| `app_apprun3_4_dark.png` | Клавиша D — тёмная тема |
| `app_apprun3_5_nosec.png` | Клавиша S — без секундной стрелки |
| `app_appt_1_boot.png` | Загрузка с диском ATA |
| `app_appt_2_disk.png` | Команда disk: диск найден |
| `app_appt_3_apps.png` | Баг: apps печатал %-24s (kprintf без выравнивания) |
| `app_appt_4_gui.png` | Графический режим |
| `app_appt_5_store.png` | Окно «Программы» со списком |
| `app_appt_6_store2.png` | Тот же список |
| `app_crashapp2_1_store.png` | crashapp2 1 store |
| `app_crashapp2_2_crashapp.png` | crashapp2 2 crashapp |
| `app_crashapp2_3_after_crash.png` | Исправлено: приложение снято, система жива |
| `app_crashapp2_4_alive.png` | Оболочка отвечает после сбоя |
| `app_crashapp_1_store.png` | crash1 store |
| `app_crashapp_2_crashapp.png` | crash2 crashapp |
| `app_crashapp_3_after_crash.png` | crash3 after crash |
| `app_crashapp_4_alive.png` | crash4 alive |
| `app_notes_1_notes.png` | notes 1 notes |
| `app_notes_2_typed.png` | Заметки: ввод текста |
| `app_notes_3_saved.png` | Ctrl+S — сохранение на диск |
| `app_notes_4_reloaded.png` | После перезагрузки текст восстановлен |
| `app_persist_1_install.png` | install: файл перенесён на диск |
| `app_persist_2_dls.png` | persist 2 dls |
| `app_persist_3_after_reboot.png` | После перезагрузки файл на месте |
| `app_persist_4_dcat.png` | dcat читает файл из прошлого сеанса |
| `app_persist_5_apps.png` | persist 5 apps |
| `app_reg_1_pc_disk.png` | Регрессия: pc + диск |
| `app_reg_2_pc_floppy_nodisk.png` | Регрессия: дискета без диска |
| `app_reg_3_q35_disk.png` | Регрессия: q35 + диск |
| `app_reg_4_isapc_nodisk.png` | Регрессия: isapc без диска |

## Ранние тесты (кадры, блокнот)

См. файлы без префикса `app_`.

