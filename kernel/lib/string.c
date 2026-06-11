/* kernel/lib/string.c — базовые mem*-функции для freestanding-ядра.
 *
 * GCC даже с -ffreestanding/-fno-builtin может генерировать вызовы
 * memcpy/memset/memmove/memcmp (например, из __builtin_memcpy или при
 * копировании структур), поэтому ядро ОБЯЗАНО предоставить эти символы.
 */

#include "types.h"

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    /* Быстрый путь: оба указателя выровнены одинаково — копируем по 8 байт */
    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (((uintptr_t)d & 7) && n) {
            *d++ = *s++;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
    }
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t v = (uint8_t)value;

    while (((uintptr_t)d & 7) && n) {
        *d++ = v;
        n--;
    }
    uint64_t v64 = 0x0101010101010101ULL * v;
    uint64_t *d64 = (uint64_t *)d;
    while (n >= 8) {
        *d64++ = v64;
        n -= 8;
    }
    d = (uint8_t *)d64;
    while (n--)
        *d++ = v;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0)
        return dest;
    if (d < s) {
        return memcpy(dest, src, n);
    }
    /* Перекрытие с dest > src — копируем с конца */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}
