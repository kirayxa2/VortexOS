/* =============================================================================
 * VortexOS libc — stdio.c
 * Форматирование одно (fmt_core с callback-выводом), поверх него printf
 * (чанками в SYS_WRITE, без обрезания) и vsnprintf (в буфер).
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vos.h>

/* ------------------------------ вывод ------------------------------------ */

int putchar(int c) {
    char ch = (char)c;
    vos_write(1, &ch, 1);
    return (uint8_t)c;
}

int puts(const char *s) {
    size_t n = strlen(s);
    vos_write(1, s, n);
    vos_write(1, "\n", 1);
    return (int)(n + 1);
}

/* --------------------------- ядро форматирования -------------------------- */

typedef struct emit_ctx {
    /* printf: буфер-чанк; snprintf: целевой буфер */
    char  *buf;
    size_t cap;     /* ёмкость buf */
    size_t len;     /* занято в buf */
    size_t total;   /* всего сгенерировано символов */
    int    to_fd;   /* 1 = сбрасывать чанки в SYS_WRITE */
} emit_ctx_t;

static void emit_flush(emit_ctx_t *e) {
    if (e->to_fd && e->len) {
        vos_write(1, e->buf, e->len);
        e->len = 0;
    }
}

static void emit_char(emit_ctx_t *e, char c) {
    e->total++;
    if (e->to_fd) {
        e->buf[e->len++] = c;
        if (e->len == e->cap) emit_flush(e);
    } else if (e->len + 1 < e->cap) {   /* место под завершающий 0 */
        e->buf[e->len++] = c;
    }
}

/* печать числа: u64 → строка с учётом базы/знака/ширины/флагов */
static void emit_num(emit_ctx_t *e, uint64_t v, int neg, int base,
                     int upper, int width, int zero_pad, int left) {
    char tmp[24];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digs[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    if (neg) tmp[n++] = '-';

    int pad = width > n ? width - n : 0;
    if (!left) {
        char pc = zero_pad ? '0' : ' ';
        /* при '0'-паддинге минус идёт ПЕРЕД нулями */
        if (zero_pad && neg) { emit_char(e, '-'); n--; }
        while (pad--) emit_char(e, pc);
        if (zero_pad && neg) n++; /* минус уже напечатан */
        for (int i = n - 1 - (zero_pad && neg ? 1 : 0); i >= 0; i--)
            emit_char(e, tmp[i]);
    } else {
        for (int i = n - 1; i >= 0; i--) emit_char(e, tmp[i]);
        while (pad--) emit_char(e, ' ');
    }
}

static void fmt_core(emit_ctx_t *e, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit_char(e, *fmt); continue; }
        fmt++;
        if (*fmt == '%') { emit_char(e, '%'); continue; }

        /* флаги */
        int left = 0, zero = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        /* ширина */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        /* точность (только для %s) */
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        /* длина */
        int longs = 0;
        while (*fmt == 'l') { longs++; fmt++; }
        if (*fmt == 'z') { longs = 1; fmt++; }

        switch (*fmt) {
        case 'c':
            emit_char(e, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0; while (s[n] && (prec < 0 || n < prec)) n++;
            int pad = width > n ? width - n : 0;
            if (!left) while (pad--) emit_char(e, ' ');
            for (int i = 0; i < n; i++) emit_char(e, s[i]);
            if (left) while (pad--) emit_char(e, ' ');
            break;
        }
        case 'd': case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
            emit_num(e, u, v < 0, 10, 0, width, zero, left);
            break;
        }
        case 'u': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            emit_num(e, v, 0, 10, 0, width, zero, left);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            emit_num(e, v, 0, 16, *fmt == 'X', width, zero, left);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            emit_char(e, '0'); emit_char(e, 'x');
            emit_num(e, v, 0, 16, 0, 0, 0, 0);
            break;
        }
        case 0:
            return;  /* '%' в конце строки */
        default:     /* неизвестный спецификатор — печатаем как есть */
            emit_char(e, '%');
            emit_char(e, *fmt);
            break;
        }
    }
}

/* ------------------------------ обёртки ----------------------------------- */

int vprintf(const char *fmt, va_list ap) {
    char chunk[128];
    emit_ctx_t e = { chunk, sizeof(chunk), 0, 0, 1 };
    fmt_core(&e, fmt, ap);
    emit_flush(&e);
    return (int)e.total;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    emit_ctx_t e = { buf, cap, 0, 0, 0 };
    fmt_core(&e, fmt, ap);
    if (cap) buf[e.len] = 0;
    return (int)e.total;   /* как в C99: сколько БЫ напечатали */
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
