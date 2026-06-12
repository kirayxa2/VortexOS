/* =============================================================================
 * VortexOS — userspace/vsh.c
 * vsh — Vortex Shell. Полностью userspace-приложение (ring3): рисует своё окно
 * через WM-syscalls и читает клавиатуру через wm_get_event (type=4, key).
 *
 * Это НЕ часть ядра — ядро лишь доставляет нажатые клавиши в окно с фокусом
 * (keyboard.c -> wm_handle_key -> очередь событий окна). Терминал сам ведёт
 * буфер строк, эхо ввода, разбор команд и отрисовку.
 *
 * Внешние команды (/bin/ls, cat, ...) запускаются через SYS_SPAWN_EX с
 * stdout-пайпом: вывод утилиты приходит IPC-сообщениями VOS_MSG_STDOUT,
 * завершение — VOS_MSG_CHILD_EXIT. cd/pwd — встроенные.
 * ============================================================================= */

#include "vos_abi.h"    /* включает syscalls.h */

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

/* ---- cwd / prompt ---- */
static char cwd[64]      = "/";
static char prompt[72]   = "/> ";
static int  prompt_len   = 3;

/* ---- запущенная утилита (stdout-пайп через SYS_SPAWN_EX) ---- */
static uint64_t child_pid = 0;     /* 0 = промпт активен */
static char  outbuf[COLS + 1];     /* недопечатанная строка stdout child'а */
static int   outlen = 0;

static void prompt_rebuild(void) {
    int n = 0;
    while (cwd[n] && n < 64) { prompt[n] = cwd[n]; n++; }
    prompt[n++] = '>';
    prompt[n++] = ' ';
    prompt[n] = 0;
    prompt_len = n;
}

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

/* --------------------- stdout child'а --------------------- */
/* Поток байт из ядра (VOS_MSG_STDOUT): собираем строки. */
static void stream_putc(char c) {
    if (c == '\n') { outbuf[outlen] = 0; sb_push_raw(outbuf); outlen = 0; return; }
    if (c == '\t') {
        int adv = 4 - (outlen & 3);
        while (adv-- && outlen < COLS - 1) outbuf[outlen++] = ' ';
    } else if (c >= 32 && c < 127) {
        outbuf[outlen++] = c;
    }
    if (outlen >= COLS) { outbuf[outlen] = 0; sb_push_raw(outbuf); outlen = 0; }
}

/* ---- цепочки `a && b && c`: сегменты выполняются последовательно,
 * ненулевой exit-код обрывает остаток (как в настоящем sh) ---- */
static char chain_pending[INPUT_MAX + 1];
static void chain_continue(void);

static void child_exited(uint64_t code) {
    if (outlen) { outbuf[outlen] = 0; sb_push_raw(outbuf); outlen = 0; }
    if (code) {
        char buf[24]; u_to_dec(code, buf);
        char out[48]; int n = 0;
        const char *p = "exit code: "; while (*p) out[n++] = *p++;
        p = buf; while (*p) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
    }
    child_pid = 0;
    if (code) chain_pending[0] = 0;   /* a && b: ошибка — обрыв */
    else chain_continue();
}

/* --------------------- отрисовка --------------------- */
static void term_render(void) {
    /* фон всего окна */
    wm_draw_rect(g_win, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Поведение как в нормальном терминале / kitty: строка ввода (prompt) идёт
     * СРАЗУ ПОД последней строкой вывода, а не приколочена к низу окна. Пока
     * вывод+промпт влезают (sb_count+1 <= ROWS) — рисуем от верха, промпт сразу
     * под историей. Когда экран заполнился — скроллим (показываем хвост так,
     * чтобы промпт оказался в самой нижней строке). */
    int total = sb_count + 1;                   /* история + строка ввода */
    int first;
    if (total <= ROWS) {
        first = 0;                              /* всё влезает — от верха */
    } else {
        first = sb_count - (ROWS - 1);          /* хвост: последние ROWS-1 + ввод */
    }

    int row = 0;
    for (int idx = first; idx < sb_count; idx++, row++) {
        int slot = (sb_start + idx) % MAXLINES;
        wm_draw_string(g_win, 0, row * CH_H, sb[slot], COL_FG);
    }

    /* строка ввода — прямо следующей строкой после последнего вывода */
    int iy = row * CH_H;

    if (child_pid) {
        /* Утилита работает: вместо промпта — её недопечатанная строка */
        outbuf[outlen] = 0;
        wm_draw_string(g_win, 0, iy, outbuf, COL_FG);
        wm_draw_rect(g_win, outlen * CH_W, iy, CH_W, CH_H, COL_CURSOR);
    } else {
        wm_draw_string(g_win, 0, iy, prompt, COL_PROMPT);
        int px = prompt_len * CH_W;
        /* показываем хвост ввода, если он не влезает */
        int maxin = COLS - prompt_len - 1;        /* -1 под курсор */
        if (maxin < 1) maxin = 1;
        int off = (inlen > maxin) ? inlen - maxin : 0;
        char ib[INPUT_MAX + 1];
        int k = 0;
        for (int i = off; i < inlen; i++) ib[k++] = input[i];
        ib[k] = 0;
        wm_draw_string(g_win, px, iy, ib, COL_FG);
        /* курсор-блок */
        int cx = px + k * CH_W;
        wm_draw_rect(g_win, cx, iy, CH_W, CH_H, COL_CURSOR);
    }

    wm_flush(g_win);
}

/* --------------------- команды --------------------- */
static void cmd_help(void) {
    term_print("vsh -- Vortex Shell. Commands:");
    term_print("  help            this list");
    term_print("  clear           clear screen");
    term_print("  cd <dir>, pwd   change / show current directory");
    term_print("  echo <text>     print text");
    term_print("  ver             VortexOS version");
    term_print("  about           about this terminal");
    term_print("  pid             terminal PID");
    term_print("  fb              framebuffer resolution");
    term_print("  cols            terminal grid size");
    term_print("Everything else runs from /bin with args:");
    term_print("  ls -l /bin | cat /etc/motd | mkdir /tmp/a");
    term_print("  cat cp mv rm find mkdir touch echo pwd stat head wc");
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

static int run_one(const char *line) {
    /* пропускаем ведущие пробелы */
    while (*line == ' ') line++;

    if (line[0] == 0) return 0;                    /* пустая команда */

    if (s_eq(line, "help")) { cmd_help(); return 0; }
    if (s_eq(line, "clear") || s_eq(line, "cls")) { sb_count = 0; sb_start = 0; return 0; }
    if (s_eq(line, "ver") || s_eq(line, "version")) { term_print("VortexOS -- vsh 0.2 (userspace)"); return 0; }
    if (s_eq(line, "about")) {
        term_print("vsh: userspace shell for VortexOS.");
        term_print("Window and input via WM-syscalls (ring3).");
        return 0;
    }
    if (s_eq(line, "pid")) { print_kv("pid: ", syscall0(SYS_GETPID)); return 0; }
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
        return 0;
    }
    if (s_eq(line, "cols")) {
        char cb[24], rb[24]; u_to_dec(COLS, cb); u_to_dec(ROWS, rb);
        char out[48]; int n = 0;
        const char *p = "grid: "; while (*p) out[n++] = *p++;
        p = cb; while (*p) out[n++] = *p++; out[n++] = 'x';
        p = rb; while (*p) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
        return 0;
    }
    if (s_starts(line, "echo ")) { term_print(line + 5); return 0; }
    if (s_eq(line, "echo")) { term_print(""); return 0; }

    /* ---- pwd ---- */
    if (s_eq(line, "pwd")) { term_print(cwd); return 0; }

    /* ---- cd ---- */
    if (s_eq(line, "cd") || s_starts(line, "cd ")) {
        const char *arg = (line[2] == ' ') ? line + 3 : "";
        while (*arg == ' ') arg++;
        if (!*arg) arg = "/";
        char abs[256];
        vos_abspath(cwd, arg, abs, sizeof(abs));
        if (vos_chdir(abs) != 0) { term_print("cd: no such directory"); return 1; }
        vos_getcwd(cwd, sizeof(cwd));
        prompt_rebuild();
        return 0;
    }

    /* ---- setuid <uid> [gid]: сбросить привилегии шелла (назад нельзя).
     * Дети наследуют креды при spawn — удобно проверять права VortexFS. ---- */
    if (s_starts(line, "setuid ")) {
        const char *a = line + 7;
        while (*a == ' ') a++;
        uint32_t uid = 0, gid = 0;
        int ok = (*a >= '0' && *a <= '9');
        while (*a >= '0' && *a <= '9') uid = uid * 10 + (uint32_t)(*a++ - '0');
        while (*a == ' ') a++;
        if (*a) {
            ok = ok && (*a >= '0' && *a <= '9');
            while (*a >= '0' && *a <= '9') gid = gid * 10 + (uint32_t)(*a++ - '0');
        } else gid = uid;
        if (!ok) { term_print("usage: setuid <uid> [gid]"); return 1; }
        if (vos_setuid(uid, gid) != 0) { term_print("setuid: not root"); return 1; }
        term_print("ok, privileges dropped");
        return 0;
    }

    /* ---- внешняя команда: /bin/<имя> через SYS_SPAWN_EX с stdout-пайпом ---- */
    int64_t pid = vos_spawn_ex(line, 1);   /* flag 1 = pipe stdout */
    if (pid < 0) {
        char out[COLS + 1]; int n = 0;
        const char *p = "vsh: "; while (*p && n < COLS) out[n++] = *p++;
        for (int i = 0; line[i] && line[i] != ' ' && n < COLS - 20; i++)
            out[n++] = line[i];
        p = ": command not found"; while (*p && n < COLS) out[n++] = *p++;
        out[n] = 0;
        term_print(out);
        return 1;
    }
    child_pid = (uint64_t)pid;
    outlen = 0;
    return 2;
}

/* эхо введённой строки в scrollback как "<prompt><line>" */
/* Продолжить цепочку && (вызывается после успешного завершения сегмента) */
static void run_command(const char *line);
static void chain_continue(void) {
    char next[INPUT_MAX + 1];
    int i = 0;
    if (!chain_pending[0]) return;
    while (chain_pending[i]) { next[i] = chain_pending[i]; i++; }
    next[i] = 0;
    chain_pending[0] = 0;
    run_command(next);
}

/* Точка входа: отрезает первый сегмент до &&, остаток — в chain_pending.
 * Сегмент исполняет run_one: 0 = ок, 1 = ошибка (обрыв цепочки),
 * 2 = внешняя команда запущена (продолжение в child_exited). */
static void run_command(const char *line) {
    char seg[INPUT_MAX + 1];
    int n = 0, rest = -1;
    while (*line == ' ') line++;
    for (int i = 0; line[i] && n < INPUT_MAX; i++) {
        if (line[i] == '&' && line[i + 1] == '&') { rest = i + 2; break; }
        seg[n++] = line[i];
    }
    seg[n] = 0;
    while (n && seg[n - 1] == ' ') seg[--n] = 0;
    if (rest >= 0) {
        int j = 0;
        while (line[rest] == ' ') rest++;
        while (line[rest + j] && j < INPUT_MAX) {
            chain_pending[j] = line[rest + j]; j++;
        }
        chain_pending[j] = 0;
    }
    int st = n ? run_one(seg) : 0;
    if (st == 2) return;                              /* ждём ребёнка */
    if (st != 0) { chain_pending[0] = 0; return; }    /* обрыв цепочки */
    chain_continue();
}
static void echo_input_line(void) {
    char out[COLS + 1]; int n = 0;
    const char *p = prompt; while (*p && n < COLS) out[n++] = *p++;
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

/* --------------------- IPC: выгрести stdout/exit от child'а -------------- */
static int drain_ipc(void) {
    vos_msg_t m;
    int dirty = 0;
    while (vos_ipc_recv(&m, VOS_IPC_NOWAIT)) {
        if (m.w[0] == VOS_MSG_STDOUT && child_pid && m.w[7] == child_pid) {
            const char *d = (const char *)&m.w[2];
            int len = (int)m.w[1];
            if (len > VOS_STDOUT_CHUNK) len = VOS_STDOUT_CHUNK;
            for (int i = 0; i < len; i++) stream_putc(d[i]);
            dirty = 1;
        } else if (m.w[0] == VOS_MSG_CHILD_EXIT && child_pid && m.w[7] == child_pid) {
            child_exited(m.w[1]);
            dirty = 1;
        }
    }
    return dirty;
}

void _start(void) {
    g_win = wm_create_window("vsh -- Vortex Shell", WIN_X, WIN_Y, WIN_W, WIN_H);
    if (!g_win) {
        puts("vsh: failed to create window\n");
        exit(1);
    }

    /* Инициализация cwd/prompt */
    if (vos_getcwd(cwd, sizeof(cwd)) <= 0) { cwd[0] = '/'; cwd[1] = 0; }
    prompt_rebuild();

    term_print("VortexOS vsh 0.2 -- userspace terminal");
    term_print("Type 'help' for the command list.");
    term_print("");
    term_render();

    /* Ядро всегда копирует 8 x uint32 (32 байта) в буфер события, а wm_event_t
     * меньше — поэтому выделяем полные 32 байта, чтобы не словить переполнение
     * стека, и накладываем структуру сверху. */
    uint32_t evraw[8];
    wm_event_t *ev = (wm_event_t *)evraw;
    for (;;) {
        int dirty = 0;

        if (child_pid) {
            /* Утилита работает — поллим WM-события и IPC (stdout-пайп).
             * Спим между итерациями чтобы не жечь CPU. */
            while (wm_get_event(g_win, ev)) {
                /* Пока child работает, ввод игнорируем (нет stdin v1) */
            }
            dirty = drain_ipc();
            if (dirty) term_render();
            syscall1(SYS_SLEEP, 1);   /* ~10 мс */
        } else {
            /* Промпт — блокируем на WM-событии, CPU не тратим */
            if (!wm_wait_event(g_win, ev)) break;

            if (ev->type == 4 && ev->key_pressed) {
                on_char((char)(ev->key_code & 0xFF));
                dirty = 1;
            }
            /* выгребаем все накопившиеся события за проход */
            while (wm_get_event(g_win, ev)) {
                if (ev->type == 4 && ev->key_pressed) {
                    on_char((char)(ev->key_code & 0xFF));
                    dirty = 1;
                }
            }
            /* Если только что запустили child (run_command сделал spawn),
             * проверяем IPC — может уже пришёл вывод */
            if (child_pid) dirty |= drain_ipc();

            if (dirty) term_render();
        }
    }

    exit(0);
}
