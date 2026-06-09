/* =============================================================================
 * VortexOS — kernel/drivers/pit.c
 * Настройка PIT (Intel 8253/8254) и обработка IRQ0 (таймер).
 *
 * PIT работает на базовой частоте 1193182 Hz.
 * Делитель = 1193182 / нужная_частота.
 * При 100 Hz -> делитель 11931 -> реальная частота ~100.00 Hz.
 *
 * IRQ0 вызывает sched_tick() — это точка вытесняющего планирования.
 * ============================================================================= */

#include "pit.h"
#include "idt.h"

/* Порты PIT */
#define PIT_CHANNEL0  0x40   /* Channel 0 data port (R/W) */
#define PIT_CMD       0x43   /* Mode/Command register (W) */

/* Command byte:
 *   bits 7-6: 00  = channel 0
 *   bits 5-4: 11  = lobyte/hibyte
 *   bits 3-1: 010 = mode 2 (rate generator)
 *   bit    0: 0   = binary
 * -> 0b00110100 = 0x36
 */
#define PIT_CMD_RATE_GEN  0x36

static volatile uint64_t tick_count = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* -------------------------------------------------------------------------
 * Обработчик IRQ0
 * ---------------------------------------------------------------------- */
/* Минимальный вывод одного символа без fb_puts (не вызывает другие функции) */
static inline void pit_debug_char(char c) {
    extern void fb_putchar(char);
    fb_putchar(c);
}

static void pit_handler(interrupt_frame_t *frame) {
    (void)frame;
    tick_count++;

    /* Render отвязан от ввода (fix #4): перерисовываем экран здесь, с
     * троттлингом ~каждые 2 тика (=> ~50 FPS при 100 Hz), и только если
     * что-то менялось. Сколько бы пакетов ни сыпала мышь, recomposite
     * случится максимум 50 раз/сек, а не на каждый PS/2-пакет. */
    if ((tick_count & 1) == 0) {
        extern void wm_tick_render(void);
        wm_tick_render();
    }

    extern void sched_irq_tick(void);
    sched_irq_tick();
}

/* -------------------------------------------------------------------------
 * Инициализация PIT
 * ---------------------------------------------------------------------- */
void pit_init(uint32_t hz) {
    if (hz == 0) hz = 100;

    uint32_t divisor = 1193182 / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1)      divisor = 1;

    /* Отправляем команду: channel 0, lobyte/hibyte, mode 2 */
    outb(PIT_CMD, PIT_CMD_RATE_GEN);

    /* Делитель — сначала младший байт, затем старший */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Регистрируем обработчик IRQ0 */
    irq_register(0, pit_handler);
}

uint64_t pit_ticks(void) {
    return tick_count;
}
