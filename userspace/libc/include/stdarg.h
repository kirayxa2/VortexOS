/* =============================================================================
 * VortexOS libc — stdarg.h (через builtin'ы GCC)
 * ============================================================================= */
#ifndef _VLIBC_STDARG_H
#define _VLIBC_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(v, last) __builtin_va_start(v, last)
#define va_arg(v, type)   __builtin_va_arg(v, type)
#define va_end(v)         __builtin_va_end(v)
#define va_copy(d, s)     __builtin_va_copy(d, s)

#endif /* _VLIBC_STDARG_H */
