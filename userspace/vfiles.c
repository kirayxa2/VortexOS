/* =============================================================================
 * VortexOS — userspace/vfiles.c
 * vfiles — GUI-файлменеджер, клиент userspace window manager'а (/bin/vwm).
 *
 * Как vdemo, но по-взрослому: листает каталоги через SYS_FS_READDIR,
 * получает клики мыши через VWM_EV_MOUSE (vwm форвардит ЛКМ в содержимое
 * окна), рисует в shm-поверхность и шлёт COMMIT.
 *
 * Управление:
 *   - клик        — выбрать строку
 *   - двойной клик— войти в каталог / запустить файл (ELF из /bin)
 *   - строка ".." / Backspace — на уровень вверх
 *   - стрелки ▲▼ справа — прокрутка
 * ============================================================================= */

#include "vos_abi.h"
#include "font8x16.h"

#define START_W 460
#define START_H 340

/* --- палитра (в духе vwm/vdemo) --- */
#define COL_BG      0xFF20202E   /* фон списка                 */
#define COL_BAR     0xFF16161F   /* путь-бар и статус-бар      */
#define COL_ACCENT  0xFF007ACC   /* акценты                    */
#define COL_FG      0xFFE0E0E0   /* текст                      */
#define COL_DIM     0xFF9090A8   /* вторичный текст            */
#define COL_SEL     0xFF2E4A6E   /* выбранная строка           */
#define COL_FOLDER  0xFFE8B64C   /* иконка папки               */
#define COL_FILE    0xFFB8C2D0   /* иконка файла               */
#define COL_SCROLL  0xFF34344A   /* фон скроллбара             */

/* --- раскладка --- */
#define PATHBAR_H   26
#define STATUS_H    22
#define ROW_H       20
#define SCROLL_W    18
#define PAD         10

#define MAX_ENTRIES 128
#define MAX_PATH    200
#define DBLCLICK_TICKS 40        /* как у иконок рабочего стола в vwm */

static uint64_t  wm_pid = 0;
static uint64_t  win_id = 0;
static uint32_t *surf = 0;
static int win_w = START_W, win_h = START_H;

static char cur_path[MAX_PATH] = "/";
static vos_dirent_t entries[MAX_ENTRIES];
static int n_entries = 0;        /* включая ".." (entries[0], если не корень) */
static int has_dotdot = 0;
static int selected = -1;
static int scroll = 0;           /* первая видимая строка */
static int last_click_row = -1;
static uint64_t last_click_tick = 0;

/* ----------------------------- строки ------------------------------------ */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *dst, const char *src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int u_to_dec(uint32_t v, char *out) {     /* возвращает длину */
    char tmp[12]; int n = 0;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* ----------------------------- рисование --------------------------------- */
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
            if (py >= 0 && py < win_h) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int px = cx + col;
                    if (px >= 0 && px < win_w)
                        surf[(uint32_t)py * win_w + px] = fg;
                }
            }
        }
        cx += 8;
        s++;
    }
}

/* мини-иконки 12x10 в строке списка */
static void draw_icon_folder(int x, int y) {
    draw_rect(x, y,     5, 2, COL_FOLDER);     /* язычок */
    draw_rect(x, y + 2, 12, 8, COL_FOLDER);    /* корпус */
}
static void draw_icon_file(int x, int y) {
    draw_rect(x + 1, y, 9, 10, COL_FILE);
    draw_rect(x + 3, y + 3, 5, 1, COL_BG);
    draw_rect(x + 3, y + 5, 5, 1, COL_BG);
}
/* треугольники ▲/▼ для скроллбара (в шрифте таких глифов нет) */
static void draw_tri(int cx, int y, int up, uint32_t c) {
    for (int r = 0; r < 5; r++) {
        int w = up ? (2 * r + 1) : (9 - 2 * r);
        draw_rect(cx - w / 2, y + r, w, 1, c);
    }
}

/* ----------------------------- геометрия --------------------------------- */
static int list_top(void)     { return PATHBAR_H; }
static int list_h(void)       { return win_h - PATHBAR_H - STATUS_H; }
static int visible_rows(void) { int v = list_h() / ROW_H; return v < 1 ? 1 : v; }

static void clamp_scroll(void) {
    int maxs = n_entries - visible_rows();
    if (maxs < 0) maxs = 0;
    if (scroll > maxs) scroll = maxs;
    if (scroll < 0) scroll = 0;
}

/* ----------------------------- данные ------------------------------------ */
static void load_dir(void) {
    n_entries = 0;
    selected = -1;
    scroll = 0;
    last_click_row = -1;
    has_dotdot = (cur_path[1] != 0);          /* не корень */
    if (has_dotdot) {
        scpy(entries[0].name, "..", 32);
        entries[0].type = VOS_DT_DIR;
        entries[0].size = 0;
        n_entries = 1;
    }
    for (uint64_t i = 0; n_entries < MAX_ENTRIES; i++) {
        if (vos_fs_readdir(cur_path, i, &entries[n_entries]) != 0) break;
        n_entries++;
    }
}

static void path_enter(const char *name) {     /* войти в подкаталог */
    int n = slen(cur_path);
    if (n > 1) {
        if (n < MAX_PATH - 1) { cur_path[n] = '/'; cur_path[n + 1] = 0; n++; }
    }
    int i = 0;
    while (name[i] && n < MAX_PATH - 1) cur_path[n++] = name[i++];
    cur_path[n] = 0;
}
static void path_up(void) {                    /* на уровень вверх */
    int n = slen(cur_path);
    while (n > 1 && cur_path[n - 1] != '/') n--;
    if (n > 1) n--;                            /* убрать и сам '/' */
    cur_path[n == 0 ? 1 : n] = 0;
    if (n == 0) { cur_path[0] = '/'; cur_path[1] = 0; }
}

/* ----------------------------- рендер ------------------------------------ */
static void render(void) {
    int lt = list_top(), lh = list_h(), vis = visible_rows();
    int lw = win_w - SCROLL_W;

    /* путь-бар */
    draw_rect(0, 0, win_w, PATHBAR_H, COL_BAR);
    draw_rect(0, PATHBAR_H - 1, win_w, 1, COL_ACCENT);
    draw_text(PAD, (PATHBAR_H - 16) / 2, cur_path, COL_FG);

    /* список */
    draw_rect(0, lt, lw, lh, COL_BG);
    for (int r = 0; r < vis; r++) {
        int idx = scroll + r;
        if (idx >= n_entries) break;
        int y = lt + r * ROW_H;
        if (idx == selected)
            draw_rect(0, y, lw, ROW_H, COL_SEL);
        if (entries[idx].type == VOS_DT_DIR) draw_icon_folder(PAD, y + 5);
        else                                 draw_icon_file(PAD, y + 5);
        draw_text(PAD + 20, y + 2, entries[idx].name,
                  entries[idx].type == VOS_DT_DIR ? COL_FG : COL_FILE);
        /* размер справа (только файлы) */
        if (entries[idx].type == VOS_DT_FILE) {
            char sz[12];
            int len = u_to_dec(entries[idx].size, sz);
            draw_text(lw - PAD - len * 8, y + 2, sz, COL_DIM);
        }
    }

    /* скроллбар: ▲, трек с бегунком, ▼ */
    int sx = win_w - SCROLL_W;
    draw_rect(sx, lt, SCROLL_W, lh, COL_SCROLL);
    draw_tri(sx + SCROLL_W / 2, lt + 7, 1, COL_DIM);            /* ▲ */
    draw_tri(sx + SCROLL_W / 2, lt + lh - 12, 0, COL_DIM);      /* ▼ */
    if (n_entries > vis) {
        int track = lh - 2 * ROW_H;
        int th = track * vis / n_entries;
        if (th < 8) th = 8;
        int ty = lt + ROW_H + (track - th) * scroll / (n_entries - vis);
        draw_rect(sx + 4, ty, SCROLL_W - 8, th, COL_ACCENT);
    }

    /* статус-бар */
    int sy = win_h - STATUS_H;
    draw_rect(0, sy, win_w, STATUS_H, COL_BAR);
    draw_rect(0, sy, win_w, 1, COL_ACCENT);
    {
        char buf[48]; int n = 0;
        n += u_to_dec((uint32_t)(n_entries - has_dotdot), buf);
        const char *tail = " items";
        for (int i = 0; tail[i] && n < 47; i++) buf[n++] = tail[i];
        buf[n] = 0;
        draw_text(PAD, sy + 3, buf, COL_DIM);
    }
    if (selected >= 0 && selected < n_entries)
        draw_text(PAD + 9 * 8 + 16, sy + 3, entries[selected].name, COL_FG);

    vwm_commit(wm_pid, win_id, 0, 0, win_w, win_h);
}

/* ----------------------------- открытие ---------------------------------- */
static void open_entry(int idx) {
    if (idx < 0 || idx >= n_entries) return;
    if (has_dotdot && idx == 0) { path_up(); load_dir(); render(); return; }
    if (entries[idx].type == VOS_DT_DIR) {
        path_enter(entries[idx].name);
        load_dir();
        render();
        return;
    }
    /* файл: пробуем запустить (ELF из /bin запустится, мусор ядро отбросит) */
    char full[MAX_PATH + 36];
    scpy(full, cur_path, MAX_PATH);
    int n = slen(full);
    if (n > 1 && n < (int)sizeof(full) - 1) full[n++] = '/';
    int i = 0;
    while (entries[idx].name[i] && n < (int)sizeof(full) - 1)
        full[n++] = entries[idx].name[i++];
    full[n] = 0;
    if (n > 1 && full[1] == '/') { /* корень: "//x" → "/x" */
        for (int k = 1; full[k]; k++) full[k] = full[k + 1];
    }
    vos_spawn(full);
}

/* ----------------------------- ввод -------------------------------------- */
static void on_mouse(int mx, int my, int buttons) {
    if (!(buttons & 1)) return;                    /* реагируем на нажатие */
    int lt = list_top(), lh = list_h(), vis = visible_rows();

    /* скроллбар */
    if (mx >= win_w - SCROLL_W) {
        if (my >= lt && my < lt + ROW_H)            scroll--;
        else if (my >= lt + lh - ROW_H && my < lt + lh) scroll++;
        else if (my >= lt && my < lt + lh) {        /* клик по треку — страница */
            int track_mid = lt + lh / 2;
            scroll += (my < track_mid) ? -vis : vis;
        } else return;
        clamp_scroll();
        render();
        return;
    }

    /* строки списка */
    if (my >= lt && my < lt + lh) {
        int row = scroll + (my - lt) / ROW_H;
        if (row >= n_entries) {
            if (selected != -1) { selected = -1; render(); }
            return;
        }
        uint64_t now = vos_uptime();
        int dbl = (row == last_click_row &&
                   (now - last_click_tick) <= DBLCLICK_TICKS);
        last_click_row = row;
        last_click_tick = now;
        if (dbl) { last_click_row = -1; open_entry(row); return; }
        if (selected != row) { selected = row; render(); }
    }
}

/* ----------------------------- main -------------------------------------- */
void _start(void) {
    wm_pid = vwm_wait_for_wm();
    win_id = vwm_create_window(wm_pid, "vfiles", START_W, START_H, &surf);
    if (!win_id) {
        puts("vfiles: failed to create window\n");
        exit(1);
    }
    load_dir();
    render();

    vos_msg_t m;
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        switch (m.w[0]) {
        case VWM_EV_MOUSE:
            if (m.w[1] == win_id)
                on_mouse((int)(int64_t)m.w[2], (int)(int64_t)m.w[3],
                         (int)m.w[4]);
            break;
        case VWM_EV_KEY:
            if (m.w[1] == win_id && m.w[3]) {
                char ch = (char)m.w[2];
                if (ch == '\b' && has_dotdot) {        /* Backspace = вверх */
                    path_up(); load_dir(); render();
                } else if (ch == 'r') {                /* r = обновить */
                    load_dir(); render();
                }
            }
            break;
        case VWM_EV_RESIZE:
            if (m.w[1] == win_id) {
                win_w = (int)m.w[2];
                win_h = (int)m.w[3];
                clamp_scroll();
                render();
            }
            break;
        case VWM_EV_CLOSE:
            exit(0);
            break;
        default:
            break;
        }
    }
}
