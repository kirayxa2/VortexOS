/* =============================================================================
 * VortexOS — kernel/drivers/mouse.c
 * Драйвер PS/2 мыши. IRQ12, 3-байтовые пакеты.
 * ============================================================================= */

#include "mouse.h"
#include "idt.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void ps2_wait_write(void) {
    while (inb(PS2_STATUS) & 0x02);
}
static void mouse_write(uint8_t data) {
    ps2_wait_write(); outb(PS2_CMD,  0xD4);
    ps2_wait_write(); outb(PS2_DATA, data);
}
/* Читаем с таймаутом — возвращаем 0xFF если нет данных */
static uint8_t mouse_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01) return inb(PS2_DATA);
    return 0xFF;
}

/* Текущее состояние мыши */
static mouse_state_t mouse_state = {0};

/* Буфер для 3-байтового пакета */
static uint8_t  pkt[3];
static uint8_t  pkt_idx = 0;

static void mouse_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t byte = inb(PS2_DATA);

    /* Первый байт должен иметь бит 3 = 1 (always-1 bit) */
    if (pkt_idx == 0 && !(byte & 0x08)) return;

    pkt[pkt_idx++] = byte;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    /* Разбираем пакет:
     * pkt[0]: флаги (кнопки, знаки движения)
     * pkt[1]: delta X
     * pkt[2]: delta Y
     */
    uint8_t flags = pkt[0];

    /* Кнопки */
    mouse_state.left   = (flags & 0x01) ? 1 : 0;
    mouse_state.right  = (flags & 0x02) ? 1 : 0;
    mouse_state.middle = (flags & 0x04) ? 1 : 0;

    /* Движение — знаковые числа (знак хранится в flags) */
    int16_t dx = (int16_t)pkt[1] - ((flags & 0x10) ? 256 : 0);
    int16_t dy = (int16_t)pkt[2] - ((flags & 0x20) ? 256 : 0);

    /* Y НЕ инвертируем — мышь и экран имеют одинаковую систему координат */
    mouse_state.x += dx;
    mouse_state.y += dy;  /* Было: mouse_state.y -= dy; */

    /* Простое ограничение (точное ограничение в compositor) */
    if (mouse_state.x < 0)    mouse_state.x = 0;
    if (mouse_state.y < 0)    mouse_state.y = 0;
    if (mouse_state.x > 2000) mouse_state.x = 2000;
    if (mouse_state.y > 2000) mouse_state.y = 2000;
    
    /* Уведомляем WM о движении мыши */
    extern void wm_handle_mouse_move(int dx, int dy);
    extern void wm_handle_mouse_button(uint8_t buttons);
    
    if (dx != 0 || dy != 0) {
        wm_handle_mouse_move(dx, dy);
    }
    
    /* Уведомляем о кнопках */
    uint8_t button_state = 0;
    if (mouse_state.left)   button_state |= 0x01;
    if (mouse_state.right)  button_state |= 0x02;
    if (mouse_state.middle) button_state |= 0x04;
    wm_handle_mouse_button(button_state);
}

void mouse_init(void) {
    /* Flush PS/2 output buffer */
    int limit = 128;
    while (limit-- > 0 && (inb(0x64) & 0x01))
        (void)inb(0x60);
    
    /* Включаем вспомогательное устройство (мышь) */
    ps2_wait_write(); outb(PS2_CMD, 0xA8);

    /* Включаем прерывания мыши (IRQ12): устанавливаем бит 1 в Command Byte
     * НО сохраняем бит 0 (IRQ1 для клавиатуры)! */
    ps2_wait_write(); outb(PS2_CMD, 0x20);
    uint8_t status = mouse_read();
    status |= 0x03;  /* Устанавливаем биты 0 и 1 — IRQ1 (keyboard) и IRQ2 (mouse) */
    status &= ~0x30; /* Очищаем биты 4 и 5 — включаем оба порта */
    ps2_wait_write(); outb(PS2_CMD, 0x60);
    ps2_wait_write(); outb(PS2_DATA, status);

    /* Сбрасываем мышь */
    mouse_write(0xFF);
    mouse_read(); /* ACK */
    mouse_read(); /* 0xAA */
    mouse_read(); /* device ID */

    /* Set Defaults */
    mouse_write(0xF6); mouse_read();

    /* Enable Data Reporting */
    mouse_write(0xF4); mouse_read();

    /* Регистрируем обработчик IRQ12 */
    irq_register(12, mouse_handler);

    /* Размаскируем IRQ12 в PIC2 и IRQ2 (cascade) в PIC1 */
    uint8_t mask = inb(0xA1);
    mask &= ~(1 << 4); /* IRQ12 = бит 4 в PIC2 */
    outb(0xA1, mask);
    mask = inb(0x21);
    mask &= ~(1 << 2); /* IRQ2 (cascade) */
    outb(0x21, mask);
}

mouse_state_t mouse_get_state(void) {
    return mouse_state;
}
