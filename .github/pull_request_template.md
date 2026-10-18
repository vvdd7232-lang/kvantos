## Изменение и границы

Что исправлено/добавлено; ссылка на roadmap/issue. Что остаётся OPEN.

## Проверки

Точные команды, версии toolchain/эмулятора, exit status и logs/result.json.
Отметьте только действительно выполненное; неприменимое явно объясните.

- [ ] Host/sanitizer и boundary/error cases
- [ ] BIOS32
- [ ] UEFI64
- [ ] Legacy ABI2/3 и opt-in ABI4 не смешаны по trust level
- [ ] Физические проверки: выполнены / не выполнялись (перечислить)

## Safety / совместимость

- [ ] Checked buffers, quotas, source/table budgets; нет silent truncation
- [ ] Private pages/files, cleanup/PMM, faults/yields остаются изолированными
- [ ] Default RO/explicit opt-in/WAL/generations/DMA fences не ослаблены
- [ ] Только disposable regular-file test images; пользовательские носители не изменены
- [ ] >128 строк остаётся предупреждением; документ не теряется при отказе
- [ ] Нет credentials, generated build images или ложного hardware/security proof

Публикация, push/tag/release и destructive device tests не подразумеваются этим PR.
