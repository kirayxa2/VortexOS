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

/* Последнее ОТПРАВЛЕННОЕ в WM состояние кнопок. Нужно, чтобы дёргать
 * wm_handle_mouse_button() только при РЕАЛЬНОМ изменении кнопок, а не на
 * каждый PS/2-пакет (см. ниже — это убивало FPS). */
static uint8_t  last_button_state = 0;

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
    extern int  ipc_input_grabbed(void);
    extern void ipc_input_push_mouse(int dx, int dy, uint8_t buttons, int btn_changed);

    /* Если userspace WM забрал ввод (feat/userspace-wm) — события мыши уходят
     * сообщениями в его mailbox (движение коалесируется), kernel-WM спит. */
    if (ipc_input_grabbed()) {
        uint8_t btns = 0;
        if (mouse_state.left)   btns |= 0x01;
        if (mouse_state.right)  btns |= 0x02;
        if (mouse_state.middle) btns |= 0x04;
        int changed = (btns != last_button_state);
        if (changed) last_button_state = btns;
        if (dx != 0 || dy != 0 || changed)
            ipc_input_push_mouse(dx, dy, btns, changed);
        return;
    }

    if (dx != 0 || dy != 0) {
        wm_handle_mouse_move(dx, dy);
    }
    
    /* Уведомляем о кнопках ТОЛЬКО при изменении состояния.
     *
     * БАГ, из-за которого damage rectangles не давали эффекта и было ~5 FPS:
     * раньше wm_handle_mouse_button() вызывался на КАЖДЫЙ PS/2-пакет (в т.ч. на
     * чистое движение без нажатий), а он БЕЗУСЛОВНО ставит g_needs_redraw = 1.
     * => каждый тик PIT шёл по тяжёлому пути wm_render_all() (полный comp_clear
     * всего экрана + recomposite окон + полный comp_flip), а лёгкий damage-путь
     * курсора (comp_cursor_refresh, два мелких прямоугольника) НЕ запускался
     * НИКОГДА. Теперь при простом движении мыши кнопки не меняются → редроя
     * сцены нет → работает быстрый save-under путь курсора. */
    uint8_t button_state = 0;
    if (mouse_state.left)   button_state |= 0x01;
    if (mouse_state.right)  button_state |= 0x02;
    if (mouse_state.middle) button_state |= 0x04;
    if (button_state != last_button_state) {
        last_button_state = button_state;
        wm_handle_mouse_button(button_state);
    }
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
