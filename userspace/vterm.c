/* =============================================================================
 * VortexOS — userspace/vterm.c
 * vterm — терминал для userspace window manager'а (/vwm). Порт vsh.c на новую
 * архитектуру: окно создаётся через IPC к vwm (не через WM-syscalls ядра!),
 * текст рисуется ЛОКАЛЬНО в свою shm-поверхность (шрифт — font8x16.h),
 * клавиатура приходит сообщениями VWM_EV_KEY, кадр публикуется vwm_commit.
 * Ядро в отрисовке не участвует вообще.
 * ============================================================================= */

#include "vos_abi.h"
#include "font8x16.h"

/* ---- Геометрия (стартовая; окно можно ресайзить — сетка пересчитается) ---- */
#define START_W    720
#define START_H    432
#define CH_W       8
#define CH_H       16
#define MAXCOLS    128               /* максимум при resize (1024/8) */
#define MAXLINES   256               /* глубина scrollback */
#define INPUT_MAX  255

/* ---- Цвета (ARGB 0xFF......) ---- */
#define COL_BG     0xFF12121C
#define COL_FG     0xFFD8D8E0
#define COL_PROMPT 0xFF5EE6A0
#define COL_CURSOR 0xFFD8D8E0

/* ---- Окно ---- */
static uint64_t  wm_pid = 0;
static uint64_t  win_id = 0;
static uint32_t *surf = 0;           /* shm-поверхность, stride = win_w */
static int win_w = START_W, win_h = START_H;
static int cols, rows;

/* ---- Состояние терминала ---- */
static char sb[MAXLINES][MAXCOLS + 1];
static int  sb_count = 0;
static int  sb_start = 0;
static char input[INPUT_MAX + 1];
static int  inlen = 0;

/* --------------------- мини-libc --------------------- */
static int s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
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

/* --------------------- локальное рисование в shm --------------------- */
static void draw_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= win_h) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= win_w) continue;
            surf[(uint32_t)py * win_w + px] = c;
        }
    }
}
static void draw_text(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        uint8_t idx = (uint8_t)*s;
        if (idx >= 128) idx = '?';
        const unsigned char *glyph = vos_font[idx];
        for (int row = 0; row < 16; row++) {
            int py = y + row;
            if (py < 0 || py >= win_h) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80 >> col))) continue;
                int px = cx + col;
                if (px < 0 || px >= win_w) continue;
                surf[(uint32_t)py * win_w + px] = fg;
            }
        }
        cx += CH_W;
        s++;
    }
}

/* --------------------- scrollback --------------------- */
static void sb_push_raw(const char *line) {
    int slot = (sb_start + sb_count) % MAXLINES;
    if (sb_count == MAXLINES) {
        slot = sb_start;
        sb_start = (sb_start + 1) % MAXLINES;
    } else {
        sb_count++;
    }
    int i = 0;
    while (i < cols && i < MAXCOLS && line[i]) { sb[slot][i] = line[i]; i++; }
    sb[slot][i] = 0;
}
static void term_print(const char *s) {
    char cur[MAXCOLS + 1]; int n = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == 0) { cur[n] = 0; sb_push_raw(cur); break; }
        if (c == '\n') { cur[n] = 0; sb_push_raw(cur); n = 0; continue; }
        if (c == '\t') {
            int adv = 4 - (n & 3);
            while (adv-- && n < cols) cur[n++] = ' ';
        } else if (c >= 32 && c < 127) {
            cur[n++] = c;
        }
        if (n >= cols || n >= MAXCOLS) { cur[n] = 0; sb_push_raw(cur); n = 0; }
    }
}

/* --------------------- отрисовка --------------------- */
static void term_render(void) {
    draw_rect(0, 0, win_w, win_h, COL_BG);

    /* Промпт идёт сразу под выводом; при переполнении показываем хвост. */
    int total = sb_count + 1;
    int first = (total <= rows) ? 0 : sb_count - (rows - 1);

    int row = 0;
    for (int idx = first; idx < sb_count; idx++, row++) {
        int slot = (sb_start + idx) % MAXLINES;
        draw_text(0, row * CH_H, sb[slot], COL_FG);
    }

    int iy = row * CH_H;
    draw_text(0, iy, "vortex> ", COL_PROMPT);
    int px = 8 * CH_W;
    int maxin = cols - 8 - 1;
    int off = (inlen > maxin) ? inlen - maxin : 0;
    char ib[INPUT_MAX + 1];
    int k = 0;
    for (int i = off; i < inlen; i++) ib[k++] = input[i];
    ib[k] = 0;
    draw_text(px, iy, ib, COL_FG);
    draw_rect(px + k * CH_W, iy, CH_W, CH_H, COL_CURSOR);

    vwm_commit(wm_pid, win_id, 0, 0, win_w, win_h);
}

/* --------------------- команды --------------------- */
static void cmd_help(void) {
    term_print("vterm - userspace WM terminal. Commands:");
    term_print("  help            this list");
    term_print("  clear           clear screen");
    term_print("  echo <text>     print text");
    term_print("  ver             VortexOS version");
    term_print("  about           about this terminal");
    term_print("  pid             terminal PID");
    term_print("  wm              window manager PID");
    term_print("  fb              framebuffer resolution");
    term_print("  cols            terminal grid size");
    term_print("  run <name>      spawn ELF from /bin (e.g. run vdemo)");
    term_print("  uptime          PIT ticks since boot");
}

static void print_kv(const char *key, uint64_t v) {
    char num[24]; u_to_dec(v, num);
    char out[64]; int n = 0;
    while (*key) out[n++] = *key++;
    const char *p = num;
    while (*p) out[n++] = *p++;
    out[n] = 0;
    term_print(out);
}

static void run_command(const char *line) {
    while (*line == ' ') line++;
    if (line[0] == 0) return;

    if (s_eq(line, "help")) { cmd_help(); return; }
    if (s_eq(line, "clear") || s_eq(line, "cls")) { sb_count = 0; sb_start = 0; return; }
    if (s_eq(line, "ver") || s_eq(line, "version")) {
        term_print("VortexOS - vterm 0.1 (userspace WM)");
        return;
    }
    if (s_eq(line, "about")) {
        term_print("vterm: terminal for the userspace window manager.");
        term_print("Draws itself into a shm surface, events via IPC.");
        term_print("The kernel never touches the pixels.");
        return;
    }
    if (s_eq(line, "pid")) { print_kv("pid: ", syscall0(SYS_GETPID)); return; }
    if (s_eq(line, "wm"))  { print_kv("wm pid: ", wm_pid); return; }
    if (s_eq(line, "uptime")) { print_kv("uptime ticks: ", vos_uptime()); return; }
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
        char cb[24], rb[24]; u_to_dec((uint64_t)cols, cb); u_to_dec((uint64_t)rows, rb);
        char out[48]; int n = 0;
        const char *p = "grid: "; while (*p) out[n++] = *p++;
        p = cb; while (*p) out[n++] = *p++; out[n++] = 'x';
        p = rb; while (*p) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
        return;
    }
    if (s_starts(line, "run ")) {
        const char *path = line + 4;
        while (*path == ' ') path++;
        if (!*path) { term_print("run: usage: run <name|/path>"); return; }
        /* без слэша ядро само ищет в /bin (elf_open_exec) — как $PATH */
        uint64_t pid = vos_spawn(path);
        if (pid == (uint64_t)-1) term_print("run: spawn failed");
        else print_kv("spawned pid: ", pid);
        return;
    }
    if (s_starts(line, "echo ")) { term_print(line + 5); return; }
    if (s_eq(line, "echo")) { term_print(""); return; }

    char out[MAXCOLS + 1]; int n = 0;
    const char *p = "vterm: "; while (*p && n < cols) out[n++] = *p++;
    for (int i = 0; line[i] && n < cols - 20; i++) out[n++] = line[i];
    p = ": command not found"; while (*p && n < cols) out[n++] = *p++;
    out[n] = 0;
    term_print(out);
}

static void echo_input_line(void) {
    char out[MAXCOLS + 1]; int n = 0;
    const char *p = "vortex> "; while (*p && n < cols) out[n++] = *p++;
    for (int i = 0; i < inlen && n < cols && n < MAXCOLS; i++) out[n++] = input[i];
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
    if (c == '\b' || c == 127) {
        if (inlen > 0) inlen--;
        return;
    }
    if (c == '\t') {
        if (inlen < INPUT_MAX) input[inlen++] = ' ';
        return;
    }
    if (c >= 32 && c < 127) {
        if (inlen < INPUT_MAX) input[inlen++] = c;
    }
}

/* --------------------- main --------------------- */
void _start(void) {
    wm_pid = vwm_wait_for_wm();
    win_id = vwm_create_window(wm_pid, "vterm - Vortex Shell",
                               START_W, START_H, &surf);
    if (!win_id) {
        puts("vterm: failed to create window\n");
        exit(1);
    }
    cols = win_w / CH_W;
    rows = win_h / CH_H;

    term_print("VortexOS vterm 0.1 - userspace WM terminal");
    term_print("Type 'help' for the command list.");
    term_print("");
    term_render();

    vos_msg_t m;
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        int dirty = 0;
        for (;;) {
            switch (m.w[0]) {
            case VWM_EV_KEY:
                if (m.w[1] == win_id && m.w[3]) {
                    on_char((char)(m.w[2] & 0xFF));
                    dirty = 1;
                }
                break;
            case VWM_EV_RESIZE:
                if (m.w[1] == win_id) {
                    win_w = (int)m.w[2];
                    win_h = (int)m.w[3];
                    cols = win_w / CH_W;
                    if (cols > MAXCOLS) cols = MAXCOLS;
                    rows = win_h / CH_H;
                    dirty = 1;
                }
                break;
            case VWM_EV_CLOSE:
                exit(0);
                break;
            default:
                break;
            }
            if (!vos_ipc_recv(&m, VOS_IPC_NOWAIT)) break;  /* выгребли всё */
        }
        if (dirty) term_render();
    }
}
