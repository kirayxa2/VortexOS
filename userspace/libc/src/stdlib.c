/* =============================================================================
 * VortexOS libc — stdlib.c (всё, кроме malloc — он в malloc.c)
 * ============================================================================= */
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <vos.h>

void exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)(int64_t)code);
    for (;;) {}   /* не вернёмся, но компилятору нужен noreturn-хвост */
}

void abort(void) {
    exit(134);    /* 128 + SIGABRT, по традиции */
}

long strtol(const char *s, char **end, int base) {
    while (isspace((uint8_t)*s)) s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    long v = 0;
    for (;; s++) {
        int d;
        if (isdigit((uint8_t)*s))      d = *s - '0';
        else if (isalpha((uint8_t)*s)) d = tolower((uint8_t)*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

int  abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* xorshift64 — быстро и достаточно для игр/демок */
static uint64_t rng_state = 0;

void srand(unsigned seed) {
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

int rand(void) {
    if (!rng_state) {
        uint64_t t = vos_uptime();
        rng_state = (t << 32) ^ (t * 0x9E3779B97F4A7C15ULL) ^ 0xDEADBEEFCAFEBABEULL;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (int)(rng_state & RAND_MAX);
}
