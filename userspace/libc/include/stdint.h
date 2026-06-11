/* =============================================================================
 * VortexOS libc — stdint.h
 * Минимальные фиксированные типы (хост x86_64: long = 64 бита).
 * Совпадают с typedef'ами в userspace/syscalls.h — C11 разрешает повтор
 * одинаковых typedef, так что оба заголовка можно включать вместе.
 * ============================================================================= */
#ifndef _VLIBC_STDINT_H
#define _VLIBC_STDINT_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;

typedef unsigned long      uintptr_t;
typedef long               intptr_t;

#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-2147483647 - 1)
#define INT64_MIN  (-9223372036854775807L - 1)

#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807L

#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615UL

#endif /* _VLIBC_STDINT_H */
