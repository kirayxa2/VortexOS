/* =============================================================================
 * VortexOS libc — stdlib.h
 * malloc живёт в статическом heap'е в BSS процесса (см. src/malloc.c):
 * ELF-загрузчик ядра выделяет p_memsz целиком, так что отдельный syscall
 * для кучи не нужен. Размер задаётся VLIBC_HEAP_SIZE (по умолчанию 256 KiB).
 * ============================================================================= */
#ifndef _VLIBC_STDLIB_H
#define _VLIBC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **end, int base);

int   abs(int v);
long  labs(long v);

/* Псевдослучайные числа (xorshift64, сид по умолчанию — vos_uptime()). */
#define RAND_MAX 0x7FFFFFFF
int   rand(void);
void  srand(unsigned seed);

void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

#endif /* _VLIBC_STDLIB_H */
