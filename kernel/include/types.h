#ifndef VOS_TYPES_H
#define VOS_TYPES_H

/* =============================================================================
 * VortexOS — kernel/include/types.h
 * Базовые типы ядра. Используем stdint.h из компилятора — не переопределяем.
 * ============================================================================= */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Вспомогательные макросы */
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN_UP(x, align)  (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))

/* Атрибуты компилятора */
#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define INLINE      __attribute__((always_inline)) inline

#endif /* VOS_TYPES_H */
