# M3.5d — диагностический checkpoint, 17.10.2026

**Не исправление исторического отказа и не готовое TT recovery.**
Зафиксированы ограниченные per-HC fault snapshots и воспроизводимые probe/replay
fixtures. Исходные пределы 1e9 raw TSC ticks / 250 frames / 2M polls сохранены.
Повтор WRITE, CLEAR_TT_BUFFER и восстановление failed HC не добавлены.

## Доказано программными проверками

- Прежние finite-delay controls (2 write IOPS): оба ядра безопасно отказали,
  сохранив caller/editor/history, reason FRAMES; 13-byte CSW мог ждать backend.
- Прежние параллельные x64 saves: 10/10 PASS. Это не объясняет первоначальный сбой.
- Повторная NORMAL release-сборка gcc-12 и BIOS CD/floppy + UEFI boot smoke: PASS.
- Host EHCI/hub/UHCI/OHCI/USB layout models: PASS; исходные compiler tests 73+73 PASS.
- Исправлен **только тестовый** ход FRINDEX: SS/CS early returns больше не
  останавливают часы. Реальный FS NYET проверен независимо с работающими frames
  и замороженным TSC (включая wrap), с остановленными frames и работающим TSC,
  с обоими замороженными clocks (poll budget). Caller/toggle не меняются.
- Стоковый QEMU EHCI isolation, BIOS32 и UEFI64: PASS; fence HC0, HC1 и RAM целы.
- Оба архивных slow32/slow64 образа повторно проверены каждым ядром через
  **стоковый OHCI**: 4/4 PASS — old namespace, default RO, fsck, SHA вне FAT.
  Это квалифицирует disk/WAL cold recovery, **не TT transport recovery**.
  Переменная USB_RECOVERY_CONTROLLER=ohci явно выбирает этот режим; EHCI остаётся
  прежним default и требует HS/TT fixture. Новую гигабайтную QEMU build не делали.

## Открыто

Первый x64 large SaveAs refusal M3.5c не воспроизведён с установленной причиной.
SS ACK → CS NYET / незавершённый CSW не различают медленное backend I/O, таймер
и физически загрязнённый TT. Synthetic QEMU overlay — не USB wire proof.
Sticky снимок может содержать нулевые/предыдущие QH поля для ранней ошибки.
Новые raw dumps появились после запуска прежних slow controls; их наличие в
старых fixtures не подразумевается.

Диагностические архивы до очистки: /home/user/workspace-checkpoints/*.tar.gz.
Текущие текстовые результаты: build/m35d/*.log (сжимаются при checkpoint).
Аппаратные USB/SATA/VirtualBox/companion проверки отложены, не закрыты.
