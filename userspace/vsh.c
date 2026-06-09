/* =============================================================================
 * VortexOS — userspace/vsh.c
 * vsh — Vortex Shell. Полностью userspace-приложение (ring3): рисует своё окно
 * через WM-syscalls и читает клавиатуру через wm_get_event (type=4, key).
 *
 * Это НЕ часть ядра — ядро лишь доставляет нажатые клавиши в окно с фокусом
 * (keyboard.c -> wm_handle_key -> очередь событий окна). Терминал сам ведёт
 * буфер строк, эхо ввода, разбор команд и отрисовку.
 * ============================================================================= */

#include "syscalls.h"

/* ---- Геометрия окна и текстовой сетки ---- */
#define WIN_X      80
#define WIN_Y      80
#define WIN_W      720
#define WIN_H      432
#define CH_W       8                 /* ширина глифа шрифта 8x16 */
#define CH_H       16                /* высота строки */
#define COLS       (WIN_W / CH_W)    /* 90 символов в строке */
#define ROWS       (WIN_H / CH_H)    /* 27 строк на экране */
#define MAXLINES   256               /* глубина scrollback */
#define INPUT_MAX  255

/* ---- Цвета (ARGB 0xFF......) ---- */
#define COL_BG     0xFF12121C
#define COL_FG     0xFFD8D8E0
#define COL_PROMPT 0xFF5EE6A0
#define COL_CURSOR 0xFFD8D8E0
#define COL_DIM    0xFF8888A0

/* ---- Состояние терминала ---- */
static char   sb[MAXLINES][COLS + 1]; /* кольцевой scrollback готовых строк */
static int    sb_count = 0;           /* сколько всего строк добавлено */
static int    sb_start = 0;           /* индекс самой старой строки в кольце */

static char   input[INPUT_MAX + 1];
static int    inlen = 0;

static uint64_t g_win = 0;

/* --------------------- мини-libc --------------------- */
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Совпадает ли начало s с pfx? */
static int s_starts(const char *s, const char *pfx) {
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}

static void u_to_dec(uint64_t v, char *out) {
    char tmp[24]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

/* --------------------- scrollback --------------------- */
/* Кладём одну ГОТОВУЮ строку (уже не длиннее COLS) в кольцевой буфер. */
static void sb_push_raw(const char *line) {
    int slot = (sb_start + sb_count) % MAXLINES;
    if (sb_count == MAXLINES) {
        slot = sb_start;                       /* перезаписываем самую старую */
        sb_start = (sb_start + 1) % MAXLINES;
    } else {
        sb_count++;
    }
    int i = 0;
    while (i < COLS && line[i]) { sb[slot][i] = line[i]; i++; }
    sb[slot][i] = 0;
}

/* Печать строки с переносом по словам/ширине и разбором '\n'. */
static void term_print(const char *s) {
    char cur[COLS + 1]; int n = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == 0) { cur[n] = 0; sb_push_raw(cur); break; }
        if (c == '\n') { cur[n] = 0; sb_push_raw(cur); n = 0; continue; }
        if (c == '\t') {                      /* таб -> до 4 пробелов */
            int adv = 4 - (n & 3);
            while (adv-- && n < COLS) cur[n++] = ' ';
        } else if (c >= 32 && c < 127) {
            cur[n++] = c;
        }
        if (n >= COLS) { cur[n] = 0; sb_push_raw(cur); n = 0; }
    }
}

/* --------------------- отрисовка --------------------- */
static void term_render(void) {
    /* фон всего окна */
    wm_draw_rect(g_win, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Снизу всегда строка ввода (prompt + input). Над ней — хвост scrollback. */
    int visible_history = ROWS - 1;
    int first = sb_count - visible_history;
    if (first < 0) first = 0;

    int row = 0;
    for (int idx = first; idx < sb_count; idx++, row++) {
        int slot = (sb_start + idx) % MAXLINES;
        wm_draw_string(g_win, 0, row * CH_H, sb[slot], COL_FG);
    }

    /* строка ввода в самом низу */
    int iy = (ROWS - 1) * CH_H;
    wm_draw_string(g_win, 0, iy, "vortex> ", COL_PROMPT);
    int px = 8 * CH_W;                          /* после "vortex> " */
    /* показываем хвост ввода, если он не влезает */
    int shown = inlen;
    int off = 0;
    int maxin = COLS - 8 - 1;                    /* -1 под курсор */
    if (shown > maxin) { off = shown - maxin; }
    char ib[INPUT_MAX + 1];
    int k = 0;
    for (int i = off; i < inlen; i++) ib[k++] = input[i];
    ib[k] = 0;
    wm_draw_string(g_win, px, iy, ib, COL_FG);
    /* курсор-блок */
    int cx = px + k * CH_W;
    wm_draw_rect(g_win, cx, iy, CH_W, CH_H, COL_CURSOR);

    wm_flush(g_win);
}

/* --------------------- команды --------------------- */
static void cmd_help(void) {
    term_print("vsh — Vortex Shell. Доступные команды:");
    term_print("  help            этот список");
    term_print("  clear           очистить экран");
    term_print("  echo <текст>    напечатать текст");
    term_print("  ver             версия VortexOS");
    term_print("  about           о терминале");
    term_print("  pid             PID процесса терминала");
    term_print("  fb              разрешение framebuffer");
    term_print("  cols            размер сетки терминала");
}

static void run_command(const char *line) {
    /* пропускаем ведущие пробелы */
    while (*line == ' ') line++;

    if (line[0] == 0) return;                    /* пустая команда */

    if (s_eq(line, "help")) { cmd_help(); return; }
    if (s_eq(line, "clear") || s_eq(line, "cls")) { sb_count = 0; sb_start = 0; return; }
    if (s_eq(line, "ver") || s_eq(line, "version")) { term_print("VortexOS — vsh 0.1 (userspace)"); return; }
    if (s_eq(line, "about")) {
        term_print("vsh: userspace-терминал VortexOS.");
        term_print("Окно и ввод идут через WM-syscalls (ring3).");
        return;
    }
    if (s_eq(line, "pid")) {
        char buf[24]; u_to_dec(syscall0(SYS_GETPID), buf);
        char out[40]; int n = 0;
        const char *p = "pid: "; while (*p) out[n++] = *p++;
        p = buf; while (*p) out[n++] = *p++; out[n] = 0;
        term_print(out);
        return;
    }
    if (s_eq(line, "fb")) {
        struct { uint64_t phys; uint32_t w, h, pitch, bpp; } info;
        syscall1(SYS_FB_INFO, (uint64_t)&info);
        char wbuf[24], hbuf[24];
        u_to_dec(info.w, wbuf); u_to_dec(info.h, hbuf);
        char out[64]; int n = 0;
        const char *p = "framebuffer: "; while (*p) out[n++] = *p++;
        p = wbuf; while (*p) out[n++] = *p++;
        out[n++] = 'x';
        p = hbuf; while (*p) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
        return;
    }
    if (s_eq(line, "cols")) {
        char cb[24], rb[24]; u_to_dec(COLS, cb); u_to_dec(ROWS, rb);
        char out[48]; int n = 0;
        const char *p = "grid: "; while (*p) out[n++] = *p++;
        p = cb; while (*p) out[n++] = *p++; out[n++] = 'x';
        p = rb; while (*p) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
        return;
    }
    if (s_starts(line, "echo ")) { term_print(line + 5); return; }
    if (s_eq(line, "echo")) { term_print(""); return; }

    /* неизвестная команда */
    char out[COLS + 1]; int n = 0;
    const char *p = "vsh: "; while (*p && n < COLS) out[n++] = *p++;
    for (int i = 0; line[i] && n < COLS - 18; i++) out[n++] = line[i];
    p = ": команда не найдена"; while (*p && n < COLS) out[n++] = *p++;
    out[n] = 0;
    term_print(out);
}

/* эхо введённой строки в scrollback как "vortex> <line>" */
static void echo_input_line(void) {
    char out[COLS + 1]; int n = 0;
    const char *p = "vortex> "; while (*p && n < COLS) out[n++] = *p++;
    for (int i = 0; i < inlen && n < COLS; i++) out[n++] = input[i];
    out[n] = 0;
    sb_push_raw(out);
}

/* --------------------- ввод --------------------- */
static void on_char(char c) {
    if (c == '\n' || c == '\r') {
        input[inlen] = 0;
        echo_input_line();
        run_command(input);
        inlen = 0;
        return;
    }
    if (c == '\b' || c == 127) {               /* backspace */
        if (inlen > 0) inlen--;
        return;
    }
    if (c == '\t') {                            /* таб -> пробел */
        if (inlen < INPUT_MAX) input[inlen++] = ' ';
        return;
    }
    if (c >= 32 && c < 127) {
        if (inlen < INPUT_MAX) input[inlen++] = c;
    }
}

void _start(void) {
    g_win = wm_create_window("vsh — Vortex Shell", WIN_X, WIN_Y, WIN_W, WIN_H);
    if (!g_win) {
        puts("vsh: failed to create window\n");
        exit(1);
    }

    term_print("VortexOS vsh 0.1 — userspace terminal");
    term_print("Наберите 'help' для списка команд.");
    term_print("");
    term_render();

    /* Ядро всегда копирует 8 x uint32 (32 байта) в буфер события, а wm_event_t
     * меньше — поэтому выделяем полные 32 байта, чтобы не словить переполнение
     * стека, и накладываем структуру сверху. */
    uint32_t evraw[8];
    wm_event_t *ev = (wm_event_t *)evraw;
    for (;;) {
        int got = 0;
        /* выгребаем все накопленные события за проход */
        while (wm_get_event(g_win, ev)) {
            got = 1;
            if (ev->type == 4 && ev->key_pressed) {
                on_char((char)(ev->key_code & 0xFF));
            }
        }
        if (got) term_render();
        else { for (volatile int i = 0; i < 20000; i++) __asm__ volatile("pause"); }
    }
}
