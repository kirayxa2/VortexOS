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

/* Флаг «пора рисовать кадр». PIT (IRQ0) лишь ВЫСТАВЛЯЕТ его с троттлингом
 * ~каждые 2 тика (=> ~50 FPS при 100 Hz). Сам рендер делает отдельная задача
 * "render" (см. wm_render_task), а НЕ обработчик прерывания — поэтому кадр
 * рисуется с ВКЛЮЧЁННЫМИ прерываниями и не стопорит планировщик и другие IRQ. */
static volatile int render_request = 0;

/* Забрать запрос на отрисовку (атомарно «прочитать и сбросить»). Возвращает 1,
 * если с прошлого вызова PIT просил перерисовку. Зовётся из задачи рендера. */
int pit_consume_render_request(void) {
    if (!render_request) return 0;
    render_request = 0;
    return 1;
}

/* Подсмотреть запрос на отрисовку БЕЗ сброса. Зовётся планировщиком
 * (pick_next), чтобы дать задаче рендера приоритет, когда есть что рисовать. */
int pit_render_pending(void) {
    return render_request;
}

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

    /* Render отвязан от ввода (fix #4) И вынесен из IRQ в отдельную задачу
     * (fix #1): здесь, в обработчике прерывания, мы НИЧЕГО не рисуем — лишь с
     * троттлингом ~каждые 2 тика (=> ~50 FPS при 100 Hz) ставим флаг. Реальную
     * отрисовку делает задача "render" (wm_render_task) с включёнными
     * прерываниями. Сколько бы пакетов ни сыпала мышь, recomposite случится
     * максимум 50 раз/сек, а не на каждый PS/2-пакет. */
    if ((tick_count & 1) == 0) {
        render_request = 1;
        /* Просим планировщик пересмотреть выбор задачи ПРЯМО СЕЙЧАС: на возврате
         * из IRQ sched_pick() увидит флаг render_request и отдаст процессор
         * задаче рендера (см. pick_next), вытеснив busy-loop userspace-задачу
         * (например, poll-цикл vsh, priority 10 = 100 мс квант). Без этого
         * курсор замирал на весь квант чужой задачи — отсюда «фриз раз в ~секунду»
         * при перетаскивании. Теперь рендер вклинивается каждые 2 тика (~20 мс =>
         * 50 FPS), и максимальная задержка курсора ограничена 20 мс. */
        extern int vos_need_resched;
        vos_need_resched = 1;
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
