#ifndef VOS_SYSCALL_H
#define VOS_SYSCALL_H

#include "types.h"

/* Номера системных вызовов */
#define SYS_WRITE      0
#define SYS_EXIT       1
#define SYS_GETPID     2
#define SYS_SLEEP      3
#define SYS_FB_INFO    4  /* Получить информацию о framebuffer */
#define SYS_FB_MAP     5  /* Замапить framebuffer в userspace */
#define SYS_INPUT_POLL 6  /* Получить события ввода (мышь/клавиатура) */

void     syscall_init(void);
void     syscall_set_kernel_stack(uint64_t rsp0);
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

#endif
