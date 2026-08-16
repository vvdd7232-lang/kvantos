/* PS/2 клавиатура, набор скан-кодов 1, кольцевой буфер */
#include "kernel.h"

#define BUF_SIZE 256
static volatile char buf[BUF_SIZE];
static volatile u32 head = 0, tail = 0;
static int shift = 0, ctrl = 0, caps = 0, extended = 0;
static void kbd_sync_leds(void);   /* определена ниже */

static const char map_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0
};

static const char map_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0
};

static void push(char c) {
    u32 n = (head + 1) % BUF_SIZE;
    if (n != tail) { buf[head] = c; head = n; }
}

static void kbd_cb(registers_t *r) {
    (void)r;

    /* Порт 0x60 читаем ТОЛЬКО когда в буфере действительно есть байт.
       Иначе возникала гонка: таймерный опрос kbd_poll() успевал забрать
       скан-код раньше, а этот обработчик читал уже пустой порт и получал
       тот же байт повторно - символы двоились ("gui" -> "ggui").
       Чтение при пустом буфере возвращает прошлое значение. */
    if (!(inb(0x64) & 0x01)) return;

    u8 sc = inb(0x60);

    /* Ответы самого контроллера, а не нажатия клавиш. Раньше 0xFA (ACK)
       на команду смены светодиодов попадал в общий разбор как скан-код
       и ломал ввод. */
    if (sc == 0xFA || sc == 0xFE || sc == 0xEE || sc == 0x00 || sc == 0xFF)
        return;

    if (sc == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        if (!(sc & 0x80)) {
            switch (sc) {
                case 0x48: push((char)(u8)KEY_UP); return;
                case 0x50: push((char)(u8)KEY_DOWN); return;
                case 0x4B: push((char)(u8)KEY_LEFT); return;
                case 0x4D: push((char)(u8)KEY_RIGHT); return;
            }
        }
        return;
    }

    if (sc & 0x80) {
        u8 code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift = 0;
        if (code == 0x1D) ctrl = 0;
        return;
    }

    switch (sc) {
        case 0x2A: case 0x36: shift = 1; return;
        case 0x1D: ctrl = 1; return;
        case 0x3A:                       /* CapsLock */
            caps = !caps;
            kbd_sync_leds();
            return;
    }

    if (sc >= 128) return;
    /* CapsLock действует ТОЛЬКО на буквы. Раньше здесь было shift^caps
       для всех клавиш, из-за чего при включённом CapsLock цифра 1
       давала "!", а Shift+1 - наоборот "1". */
    char lo = map_lower[sc];
    int letter = (lo >= 'a' && lo <= 'z');
    int up = letter ? (shift ^ caps) : shift;
    char c = up ? map_upper[sc] : lo;
    if (!c) return;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
    }
    push(c);
}

/* Управление светодиодами клавиатуры.
   Это единственный способ показать этап загрузки на ноутбуке,
   у которого нет COM-порта, а экран ещё/уже не работает.
   Биты: 0 - ScrollLock, 1 - NumLock, 2 - CapsLock. */
static void kbd_wait_write(void) {
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) return;      /* буфер ввода свободен */
}

/* Забираем ACK (0xFA) сами: иначе он уйдёт в IRQ-обработчик. */
static void kbd_wait_ack(void) {
    for (u32 i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {               /* есть байт в буфере вывода */
            u8 r = inb(0x60);
            if (r == 0xFA || r == 0xFE) return;
        }
    }
}

/* Отражаем состояние CapsLock на индикаторе клавиатуры. */
static void kbd_sync_leds(void) {
    kbd_set_leds((u8)(caps ? 0x04 : 0x00));
}

/* Страховочный опрос. На некоторых ноутбуках IRQ1 не доходит до PIC
   (нестандартная маршрутизация, режим эмуляции USB-клавиатуры в BIOS).
   Тогда байты копятся в буфере вывода, а обработчик молчит. Эту функцию
   дёргает таймер: если IRQ работает, буфер всегда пуст и она бесплатна. */
void kbd_poll(void) {
    /* Опрос идёт из обработчика таймера и может вклиниться между
       проверкой статуса и чтением 0x60 в обработчике клавиатуры -
       тогда один и тот же скан-код обработается дважды (символ
       двоился). При 1000 Гц вероятность такой гонки выросла в 10 раз,
       поэтому чтение делаем неделимым. */
    u32 fl = irq_save();
    for (int i = 0; i < 8; i++) {
        u8 st = inb(0x64);
        if (!(st & 0x01)) break;       /* буфера нет */
        if (st & 0x20) break;          /* байт мыши, не наш */
        kbd_cb(0);
    }
    irq_restore(fl);
}

void kbd_set_leds(u8 mask) {
    u32 fl = irq_save();          /* обмен командой должен быть неделим */
    kbd_wait_write();
    outb(0x60, 0xED);
    kbd_wait_ack();
    kbd_wait_write();
    outb(0x60, (u8)(mask & 0x07));
    kbd_wait_ack();
    irq_restore(fl);
}

/* Полная инициализация контроллера i8042.
   Раньше здесь стояла только установка обработчика IRQ1. В QEMU это
   работало, потому что эмулятор отдаёт контроллер уже включённым и с
   трансляцией скан-кодов. Реальный i8042 (ноутбуки, Samsung RV410)
   после передачи управления от BIOS может остаться с запрещённым
   портом, выключенной трансляцией или неразобранным байтом в буфере -
   тогда IRQ1 не приходит вовсе и ввода нет. */
void keyboard_init(void) {
    head = tail = 0;
    irq_install_handler(1, kbd_cb);

    u32 fl = irq_save();

    /* 1. Выгребаем мусор, оставшийся от BIOS. Пока в буфере вывода
          есть байт, IRQ1 больше не придёт. */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    /* 2. Включаем первый порт PS/2 (BIOS мог его отключить). */
    kbd_wait_write();
    outb(0x64, 0xAE);

    /* 3. Читаем байт конфигурации контроллера. */
    kbd_wait_write();
    outb(0x64, 0x20);
    u8 cfg = 0;
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) { cfg = inb(0x60); break; }

    cfg |=  0x01;      /* бит 0: разрешить прерывание IRQ1 */
    cfg &= (u8)~0x10;  /* бит 4: снять запрет тактирования клавиатуры */
    cfg |=  0x40;      /* бит 6: трансляция в набор скан-кодов 1 -
                          именно его ожидают наши таблицы map_lower/upper */

    kbd_wait_write();
    outb(0x64, 0x60);
    kbd_wait_write();
    outb(0x60, cfg);

    /* 4. Разрешаем клавиатуре передавать скан-коды (0xF4).
          Без этой команды устройство молчит. */
    kbd_wait_write();
    outb(0x60, 0xF4);
    kbd_wait_ack();

    /* 5. Ещё раз чистим буфер: ответы на команды нам не нужны. */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    irq_restore(fl);
}

int kbd_getchar_nb(void) {
    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return (int)(u8)c;
}

char kbd_getchar(void) {
    int c;
    while ((c = kbd_getchar_nb()) < 0) {
        /* До запуска планировщика task_yield() возвращается сразу,
           и цикл жёг процессор на 100%. Ждём прерывание по-настоящему.
           hlt безопасен только при разрешённых прерываниях - иначе
           обошлись бы вечным сном. */
        u32 fl;
        __asm__ volatile("pushfl; popl %0" : "=r"(fl));
        if (fl & 0x200) __asm__ volatile("hlt");
        else            __asm__ volatile("pause");
        task_yield();
    }
    return (char)c;
}
