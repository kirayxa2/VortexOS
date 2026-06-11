#ifndef SYSCALLS_H
#define SYSCALLS_H

typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef int int32_t;

/* Syscall numbers */
#define SYS_WRITE      0
#define SYS_EXIT       1
#define SYS_GETPID     2
#define SYS_SLEEP      3
#define SYS_FB_INFO    4
#define SYS_FB_MAP     5
#define SYS_INPUT_POLL 6
#define SYS_WM_CREATE_WINDOW  10
#define SYS_WM_DRAW_RECT      11
#define SYS_WM_DRAW_STRING    12
#define SYS_WM_FLUSH          13
#define SYS_WM_GET_EVENT      14
#define SYS_WM_WAIT_EVENT     15
#define SYS_BLOCK             16

/* Syscall wrapper */
static inline uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8 __asm__("r8") = a5;
    register uint64_t r9 __asm__("r9") = a6;
    __asm__ volatile(
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    return syscall6(num, a1, a2, a3, 0, 0, 0);
}

static inline uint64_t syscall1(uint64_t num, uint64_t a1) {
    return syscall6(num, a1, 0, 0, 0, 0, 0);
}

static inline uint64_t syscall0(uint64_t num) {
    return syscall6(num, 0, 0, 0, 0, 0, 0);
}

/* Window Manager structures */
typedef struct {
    uint32_t type;      // 0=none, 1=mouse_move, 2=mouse_down, 3=mouse_up, 4=key
    int32_t mouse_x;
    int32_t mouse_y;
    uint8_t mouse_buttons;
    uint16_t key_code;
    uint8_t key_pressed;
} wm_event_t;

/* Window Manager API */
static inline uint64_t wm_create_window(const char *title, int32_t x, int32_t y, int32_t w, int32_t h) {
    return syscall6(SYS_WM_CREATE_WINDOW, (uint64_t)title, x, y, w, h, 0);
}

static inline void wm_draw_rect(uint64_t win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    syscall6(SYS_WM_DRAW_RECT, win, x, y, w, h, color);
}

static inline void wm_draw_string(uint64_t win, int32_t x, int32_t y, const char *str, uint32_t color) {
    syscall6(SYS_WM_DRAW_STRING, win, x, y, (uint64_t)str, color, 0);
}

static inline void wm_flush(uint64_t win) {
    syscall1(SYS_WM_FLUSH, win);
}

static inline int wm_get_event(uint64_t win, wm_event_t *event) {
    return (int)syscall3(SYS_WM_GET_EVENT, win, (uint64_t)event, 0);
}

/* Блокирующее ожидание события: задача СПИТ (0% CPU), пока не придёт событие,
 * вместо busy-poll. «Как у взрослых ОС». */
static inline int wm_wait_event(uint64_t win, wm_event_t *event) {
    return (int)syscall3(SYS_WM_WAIT_EVENT, win, (uint64_t)event, 0);
}

/* Припарковать процесс навсегда (0% CPU). Для приложений, которым больше нечего
 * делать после отрисовки окна. */
static inline void block_forever(void) {
    syscall0(SYS_BLOCK);
}

/* Helper functions.
 * __VLIBC__ определяют libc-приложения (см. userspace/libc/): настоящие
 * puts()/exit() там предоставляет libc, а эти inline-версии выключаются,
 * чтобы не конфликтовать. Старые приложения без libc работают как раньше. */
#ifndef __VLIBC__
static inline void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)s, len);
}

static inline void exit(int code) {
    syscall1(SYS_EXIT, code);
}
#endif /* !__VLIBC__ */

#endif
