# Безопасное TT recovery — дизайн, не реализация

После неоднозначного split/BOT timeout нельзя считать запись невыполненной:
устройство могло принять DATA/записать носитель и ещё не вернуть CSW.
CLEAR_TT_BUFFER очищает транзакционный буфер хаба, а не отменяет SCSI WRITE.
Не отправлять новый SS на тот же endpoint до завершения старого состояния.

## Требуемая машина состояний

1. ACTIVE → QUARANTINED: отозвать write grant/generation, запретить новые запросы
   всему затронутому TT (single TT — всем его children), закрыть публикацию leaf.
2. Retire DMA: disable async schedule → подтверждённый ASS=0 и валидированный
   writeback; при невозможности остановить DMA — текущий whole-HC fence, BME off,
   pinned private pool до reboot. BME off не является IOMMU security boundary.
3. Зафиксировать неопределённую disk transaction/WAL; никаких повторов WRITE.
4. Только после доказанного retirement возможны ограниченные hub-class requests
   через независимый безопасный EP0: CLEAR_TT_BUFFER с правильными hub/port,
   device address, endpoint, direction, transfer type. Для single/multi TT нужны
   разные scope/port semantics; обычный data-QH путь на dead HC непригоден.
5. Подтвердить транспортное состояние device отдельно: контролируемый reset /
   BOT reset + halt/toggle handling и новая enumeration epoch; одно очищение TT
   не доказывает завершение command queue и состояние SCSI media cache.
6. Полный mount/recovery WAL перед публикацией, validation geometry/identity,
   default RO. Новый write opt-in только явным действием, старый grant не оживлять.
7. Повторная ошибка в любом шаге → bounded fail-closed, без recursive cleanup.

## Предусловия реализации и acceptance

- Квалифицированная модель hub CLEAR/ошибок/CSW late completion, отличающая
  endpoint buffer от незавершённой команды и от DMA retirement.
- Отдельный reclaim/restart path и ownership private DMA pools; нынешний
  reboot-only dead HC нельзя просто пометить alive и повторно использовать pool.
- Fault cuts перед/после каждого шага, unplug/reconnect/address reuse,
  shared TT siblings / другой HC, неотвечающий hub, late DMA и late CSW.
- old-or-complete-new namespace + WAL/recovery/default RO + SHA вне раздела;
  никакого silent write replay. BIOS32 и UEFI64, затем physical qualification.

**Текущий выбор:** сохранять whole-HC fence. Исторический Save64 refusal остаётся
OPEN; неподтверждённый диагноз не является основанием включать очистку или retry.
