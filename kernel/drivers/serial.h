#ifndef SERIAL_H
#define SERIAL_H

/* =============================================================================
 * VortexOS — kernel/drivers/serial.h
 * COM1 (0x3F8) — отладочная консоль. QEMU запускается с -serial stdio,
 * поэтому всё, что уходит в COM1, видно в терминале хоста ДАЖЕ когда экран
 * чёрный (virtio scanout переключён, Limine fb больше не сканируется).
 * ============================================================================= */

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);

#endif
