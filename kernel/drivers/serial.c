/* =============================================================================
 * VortexOS — kernel/drivers/serial.c
 * Минимальный polled-драйвер COM1 (0x3F8) для отладочного вывода.
 *
 * Зачем: с -vga virtio после SET_SCANOUT экран показывает только virtio-ресурс,
 * Limine framebuffer (куда пишет fb_puts) больше не виден. Если что-то падает
 * до первого present'а — на экране просто чёрный квадрат и НОЛЬ информации.
 * QEMU у нас всегда запускается с -serial stdio, так что зеркало kernel-консоли
 * в COM1 даёт полный лог в терминале хоста при любом состоянии экрана.
 *
 * Только передача (TX), полностью polled: безопасно звать из любого контекста —
 * IRQ-хендлеров, syscall-пути с маскированным IF, ранней инициализации.
 * ============================================================================= */

#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static int g_ok = 0;

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* все IRQ выключены — только polling   */
    outb(COM1 + 3, 0x80);   /* DLAB=1: доступ к делителю            */
    outb(COM1 + 0, 0x01);   /* делитель 1 → 115200 бод              */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB=0                          */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte threshold    */
    outb(COM1 + 4, 0x0B);   /* DTR | RTS | OUT2                     */

    /* loopback self-test: если порта нет (реальное железо без COM) — молчим */
    outb(COM1 + 4, 0x1E);   /* loopback */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) { g_ok = 0; return; }
    outb(COM1 + 4, 0x0B);   /* обратно в нормальный режим */
    g_ok = 1;
}

void serial_putchar(char c) {
    if (!g_ok) return;
    if (c == '\n') serial_putchar('\r');           /* терминалы хотят CRLF */
    /* ждём пустой TX (LSR бит 5) с guard'ом — никогда не виснем навсегда */
    uint32_t guard = 1000000;
    while (!(inb(COM1 + 5) & 0x20) && --guard) { __asm__ volatile("pause"); }
    outb(COM1 + 0, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putchar(*s++);
}
