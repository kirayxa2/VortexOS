/* =============================================================================
 * VortexOS libc — stdio.h
 * Вывод идёт в SYS_WRITE (fd=1 → ядро печатает на serial/консоль).
 * Поддержка printf: %c %s %d %i %u %x %X %p %% ; длины: l, ll, z;
 * флаги: '-' '0', ширина поля, точность для %s (%.20s).
 * ============================================================================= */
#ifndef _VLIBC_STDIO_H
#define _VLIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int putchar(int c);
int puts(const char *s);                /* добавляет '\n', как положено */

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);

int snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif /* _VLIBC_STDIO_H */
