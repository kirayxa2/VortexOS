/* =============================================================================
 * VortexOS libc — vos.h
 * «Системный» заголовок: всё ABI VortexOS (syscalls.h + vos_abi.h) плюс
 * пара удобных обёрток. Включай его в libc-приложениях вместо прямого
 * включения syscalls.h.
 *
 * ВАЖНО: libc-приложения собираются с -D__VLIBC__ — этот макрос выключает
 * static inline puts()/exit() в syscalls.h (их предоставляет libc).
 * ============================================================================= */
#ifndef _VLIBC_VOS_H
#define _VLIBC_VOS_H

#include "vos_abi.h"   /* сам включает syscalls.h */

static inline uint64_t vos_getpid(void)      { return syscall0(SYS_GETPID); }
static inline void     vos_sleep(uint64_t t) { syscall1(SYS_SLEEP, t); }  /* тики PIT, 100 Гц */
static inline void     vos_sleep_ms(uint64_t ms) { syscall1(SYS_SLEEP, ms / 10 ? ms / 10 : 1); }
static inline int64_t  vos_write(int fd, const void *buf, uint64_t len) {
    return (int64_t)syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, len);
}

#endif /* _VLIBC_VOS_H */
