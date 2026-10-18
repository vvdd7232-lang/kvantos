# Экспериментальный изолированный SDK (ABI4 / format2)

Это **отдельный opt-in SDK**, а не legacy `kvapp.h` ABI2/3. Старые 11 приложений
и Мастерская по умолчанию пока исполняются в ring0. Этот каталог не включён
автоматически в default apps/ISO.

```sh
make -C sdk/isolated CC=gcc-12
```

Нужны GCC с `-m32/-m64`, GNU ld/objcopy/nm и Python 3. Выход:
`build/boxes32.kapp` для BIOS/i386 и `build/boxes64.kapp` для UEFI/x64.
Не менять архитектурный флаг вместо пересборки. Запуск через обычный Program
Manager из KvFS/RAM требует подходящего ядра с experimental loader. Тест
`tests/boot/isolated-app-smoke.py` создаёт свой disposable disk fixture;
это не пользовательская функция host-import.

## Старт и память

`_start(const kvu_boot *)`: версия 1, размер 32, generation, canvas width/height,
arch64/NX и reserved. Указатель read-only. Код RX, data/BSS и стек RW,
незаполненный зазор перед стеком; i386 non-PAE **без аппаратного NX**.
Нет libc, FPU/SIMD, TLS, прямого framebuffer или kernel API function pointers.
User virtual arena 0x00e00000..0x01000000; указатели в запросах — **offset**
от base, а не physical/kernel addresses. Незаконный доступ снимает приложение.

`int80`: AX=0 exit (BX=code,CX=0); AX=2 service (BX=request offset,CX=40);
AX=3 PIT ticks (BX=CX=0). На x64 верхние половины аргументов запрещены.
`kvuser.h` содержит минимальные inline wrappers, а не общий libc/runtime.

## Service protocol 1, ещё не frozen stable ABI

40-byte request: version=1,bytes=40,op,flags=0,generation,token,a,b,c,d.
Отрицательный результат — ошибка. Подробные коды в `include/user_service.h`.

| op | a,b,c,d | token / результат |
|---|---|---|
| WAIT=1 | event offset,48,0,0 | token=0; 48 или AGAIN |
| COMPLETE=2 | 0,0,0,0 | текущий event ID; 0 |
| DRAW=3 | rect array offset,count,0,0 | DRAW ID; visible command count |
| OPEN=4 | name offset,bytes incl. NUL,flags,0 | 0; private handle |
| READ=5 / WRITE=6 | handle,buffer offset,bytes,file offset | 0; byte count |
| CLOSE=7 | handle,0,0,0 | 0 |
| DELETE=8 | name offset,bytes incl. NUL,0,0 | 0 |
| TEXT=9 | text packet offset,24,0,0 | DRAW ID; accepted UTF-8 bytes (0 fully offscreen) |
| QUERY=10 | 0,0,0,0 | 0; capability mask: RECT=1,TEXT=2,STATUS=4,INFO=8 |
| STATUS=11 | string offset,bytes,0,0 | 0; 0 (clear: bytes=0) |
| INFO=12 | destination offset,16,0,0 | 0; 16 |

Event 48 байт: version,bytes,generation,id,type,delta_ms,width,height,x,y,key,button.
Типы DRAW=1,KEY=2,CLICK=3,TICK=4,CLOSE=5. После события обязателен COMPLETE;
WAIT/COMPLETE передают управление ядру. Поток может быть вытеснен и продолжен
в следующем GUI pump. DRAW/TICK coalesced; delta_ms не более 250.

Rect 24 байта: type (1 fill,2 outline),color,x,y,w,h. До 32 за запрос,
суммарно до 64 draw-команд за событие. Batch проверяется целиком до эффекта.
TEXT packet 24 байта: x,y,color,bg,string offset,string byte count.
Длина 1..127, **без NUL** на wire; корректный UTF-8 без NUL/ASCII controls,
без overlong/surrogates/truncation/>U+10FFFF. До 1024 text bytes на DRAW;
TEXT расходует одну из 64 draw-команд. bg=0xffffffff прозрачен, иначе 24-bit.
QUERY нужен до optional TEXT; старая реализация может вернуть UNKNOWN.
Глифы ограничены встроенным CP866 font, остальные Unicode scalar отображаются
fallback-глифом, не обещается полный Unicode shaping/font engine.

STATUS: 0..95 UTF-8 bytes без NUL, те же scalar/control проверки; ошибка
не изменяет прежний private status. Подпись ABI4/CPL3 рисует только ядро,
приложение не может заменить trust label своим status. INFO возвращает
четыре u32: version=1,bytes=16,PIT hz,language (0 EN / 1 RU). Это копия
readonly metadata, не указатель на состояние ядра и не RTC/file/device grant.

Приватные файлы: 8×1024 байт, 8 handles, имена 2..32 байта с NUL,
ASCII letters/digits/`._-`, не `.`/`..`. OPEN R=1,W=2,CREATE=4,TRUNC=8.
Это не общий RAMFS/KvFS/FAT. Никаких persistent file/device grants здесь нет.

Образец проверяет file write/read=42, рисует прямоугольники и `Привет`,
считает клики. Клавиша `p` намеренно задерживает один DRAW на 8 PIT ticks,
чтобы regression проверяла настоящее quantum yield и последующий replay.
Это diagnostic convenience, не обязательное управление desktop с клавиатуры.

Формат заголовка и границы: `include/user_image.h`,
`docs/M4.5-M4.6-RUNTIME.ru.md`; CPU/map/lifecycle hardening:
`docs/M4.7-RUNTIME-HARDENING.ru.md`.

## Дополнения M4.12

QUERY: LINE=16 (optional sink), LIST=32. LINE=13: a=packet offset,b=24,c=d=0,
DRAW token; packet `{i32 x0,y0,x1,y1;u32 color,reserved=0}`. Endpoints signed16,
canvas≤4096², integer clip, 1 visible / 0 fully outside, одна draw quota.
LIST=14: a=ordinal index,b=name40 writable offset,c=size4 writable offset,d=0,
token=0. 0 success / -9 missing. Оба непересекающихся назначения проверяются
до записи. Только приватные файлы, не global RAMFS/KvFS.

Изолированный compiler adapter теперь поддерживает line/file_list; только
beep/log пока отклоняются; clock добавлен в M4.13 ниже. DRAW/TEXT/STATUS/LINE errors завершают user app
с exit31/32/33/34, а не молча пропускаются. Heap 32KiB; доступные файлы 8×1024B.


## Дополнение M4.13: readonly CLOCK

CLOCK=15, QUERY cap64: `a=output offset,b=20,c=d=token=0`.
Ответ — пять u32 `{1,20,hour,minute,second}`, return20. Если нет валидного
kernel snapshot — -8/AGAIN без записи; h0..23/m,s0..59. Выход проверяется
целиком, request snapshot допускает пересечение с собственным output.
Источник — последнее штатное rtc_read(), скопированное при входе dispatcher;
нет CMOS I/O из syscall, setters/date/timezone/kernel pointers не экспортируются.
Это cached best-effort wall clock, не точный UTC/синхронный hardware sample.
RTC writes инвалидируют cache; следующий обычный RTC read восстанавливает его.
Новый compiler adapter требует caps127; clock(h,m,s) staged, nullable/aliased
outputs как legacy, invalid/unavailable result → user exit35. Старые adapters
с mask63 и SDK apps с mask15 продолжают работать. Поддержаны line/file_list/clock;
только beep/log compile errors. [Проверки](../../docs/M4.13-READONLY-CLOCK.ru.md).
