/* =============================================================================
 * VortexOS libc — test/test_host.c
 * Юнит-тест строковых функций, malloc и vsnprintf, собирается ХОСТОВЫМ gcc
 * (обычный Linux/MSYS2 бинарник): логика libc не зависит от ядра, поэтому
 * её можно гонять где угодно. Системные вещи (printf→SYS_WRITE) не трогаем.
 *
 * Запуск:  gcc -std=c11 -DVLIBC_HOST_TEST -I../include \
 *              test_host.c ../src/string.c ../src/malloc.c -o t && ./t
 * (vsnprintf тестируем через включение stdio.c с заглушкой vos_write)
 * ============================================================================= */
#include <stdarg.h>

/* --- заглушки вместо vos.h: тянем только то, что нужно stdio/stdlib --- */
#ifdef VLIBC_HOST_TEST
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned char uint8_t;
static long vos_write(int fd, const void *buf, unsigned long len) {
    (void)fd; (void)buf; return (long)len;
}
static uint64_t vos_uptime(void) { return 12345; }
#define _VLIBC_VOS_H   /* чтобы stdio.c не включил настоящий vos.h */
#define SYS_EXIT 1
static uint64_t syscall1(uint64_t n, uint64_t a) { (void)n; (void)a; return 0; }
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* настоящие реализации */
#include "../src/stdio.c"
#include "../src/stdlib.c"

/* хостовые printf/exit нам недоступны (мы их переопределили) — пишем через
 * write(2) напрямую */
extern long write(int, const void *, unsigned long);
static int failures = 0;
static void check(int ok, const char *name) {
    write(1, ok ? "[ok]   " : "[FAIL] ", 7);
    write(1, name, strlen(name));
    write(1, "\n", 1);
    if (!ok) failures++;
}

static int fmt_eq(const char *expect, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strcmp(buf, expect) == 0;
}

int main(void) {
    /* string.c */
    check(strlen("hello") == 5, "strlen");
    check(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0, "strcmp");
    check(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp");
    char b[16];
    strlcpy(b, "0123456789ABCDEF", sizeof(b));
    check(strlen(b) == 15 && b[15] == 0, "strlcpy clamps");
    char mv[8] = "abcdef";
    memmove(mv + 1, mv, 6);
    check(memcmp(mv, "aabcdef", 7) == 0, "memmove overlap");
    check(strchr("a.b.c", '.') != 0 && strrchr("a.b.c", '.')[1] == 'c', "strchr/strrchr");

    /* stdlib.c */
    check(atoi("-123") == -123 && atoi("  42") == 42, "atoi");
    check(strtol("0x1F", 0, 0) == 31 && strtol("777", 0, 8) == 511, "strtol bases");

    /* malloc.c */
    void *p1 = malloc(100), *p2 = malloc(200);
    check(p1 && p2 && p1 != p2, "malloc basic");
    check(((unsigned long)p1 % 16) == 0 && ((unsigned long)p2 % 16) == 0,
          "malloc 16-byte aligned");
    memset(p1, 0xAA, 100);
    free(p1);
    void *p3 = malloc(50);          /* должен переиспользовать дыру */
    check(p3 != 0, "malloc reuse after free");
    char *r = (char *)malloc(8);
    strcpy(r, "1234567");
    r = (char *)realloc(r, 4096);
    check(r && strcmp(r, "1234567") == 0, "realloc keeps data");
    free(r); free(p2); free(p3);
    void *big = malloc(512 * 1024);   /* больше дефолтной кучи (256 KiB) */
    check(big == 0, "malloc OOM returns NULL");
    void *all = malloc(200000);     /* после free всё слилось обратно */
    check(all != 0, "free coalesces");
    free(all);
    check(calloc((unsigned long)-1, 16) == 0, "calloc overflow guard");

    /* stdio.c: vsnprintf */
    check(fmt_eq("hello world", "%s %s", "hello", "world"), "%s");
    check(fmt_eq("-42 +0007 2a 2A", "%d +%04d %x %X", -42, 7, 42, 42), "%d %04d %x %X");
    check(fmt_eq("[  hi][hi  ]", "[%4s][%-4s]", "hi", "hi"), "width/left-align");
    check(fmt_eq("18446744073709551615", "%lu", (uint64_t)-1), "%lu 64-bit");
    check(fmt_eq("ab..z", "%.2s..%c", "abcdef", 'z'), "%.2s %c");
    check(fmt_eq("100%", "%d%%", 100), "%%");
    check(fmt_eq("(null)", "%s", (char *)0), "%s NULL");
    char small[8];
    int want = snprintf(small, sizeof(small), "0123456789");
    check(want == 10 && strcmp(small, "0123456") == 0, "snprintf truncation");

    write(1, failures ? "FAILURES!\n" : "ALL TESTS PASSED\n", failures ? 10 : 17);
    return failures ? 1 : 0;
}
