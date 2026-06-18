/* VortexOS — vedit: минимальный текстовый редактор на libvui.
 *
 * Управление:
 *   стрелки           — курсор
 *   обычные клавиши    — ввод
 *   Backspace          — удалить символ слева
 *   Enter              — новая строка
 *   Ctrl+S (==^S, 0x13)— сохранить в текущий путь
 *   Ctrl+O (==^O, 0x0F)— открыть (фокус уходит в input пути)
 *   Esc                — выйти из input пути
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vui.h>

#define W 720
#define H 520
#define MAXLINES   512
#define MAXCOLS    256
#define PATH_MAX_ 96

static vui_win_t *win;

/* Буфер: массив строк фиксированной ширины (динамически выделять
 * сложновато без полноценного libc; для hobby-редактора годится). */
static char  text[MAXLINES][MAXCOLS];
static int   line_len[MAXLINES];
static int   nlines = 1;
static int   cx = 0, cy = 0;     /* курсор */
static int   scroll = 0;         /* первая отображаемая строка */

static char  path[PATH_MAX_] = "/tmp/untitled.txt";
static int   focus_path = 0;     /* 1 = курсор в input пути (открытие/сохранение) */
static char  status[80];

static int   ch_w_cache = 8;     /* подгонится под шрифт после первого vui_text_width */

static void set_status(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(status) - 1) { status[i] = s[i]; i++; }
    status[i] = 0;
}

/* ----- работа с буфером ----- */
static void clamp_cursor(void) {
    if (cy < 0) cy = 0;
    if (cy >= nlines) cy = nlines - 1;
    if (cx < 0) cx = 0;
    if (cx > line_len[cy]) cx = line_len[cy];
}

static void insert_char(char c) {
    if (line_len[cy] >= MAXCOLS - 1) return;
    char *L = text[cy];
    int n = line_len[cy];
    for (int i = n; i > cx; i--) L[i] = L[i - 1];
    L[cx] = c;
    line_len[cy]++;
    cx++;
}

static void insert_newline(void) {
    if (nlines >= MAXLINES) return;
    /* сдвинуть строки вниз */
    for (int i = nlines; i > cy + 1; i--) {
        int len = line_len[i - 1];
        for (int k = 0; k < len; k++) text[i][k] = text[i - 1][k];
        line_len[i] = len;
    }
    /* отрезать хвост текущей строки на новую */
    int tail = line_len[cy] - cx;
    for (int k = 0; k < tail; k++) text[cy + 1][k] = text[cy][cx + k];
    line_len[cy + 1] = tail;
    line_len[cy] = cx;
    nlines++;
    cy++;
    cx = 0;
}

static void backspace_(void) {
    if (cx > 0) {
        char *L = text[cy];
        int n = line_len[cy];
        for (int i = cx - 1; i < n - 1; i++) L[i] = L[i + 1];
        line_len[cy]--;
        cx--;
    } else if (cy > 0) {
        /* слить с предыдущей строкой */
        int prev = line_len[cy - 1];
        int cur  = line_len[cy];
        if (prev + cur >= MAXCOLS - 1) return;
        for (int k = 0; k < cur; k++) text[cy - 1][prev + k] = text[cy][k];
        line_len[cy - 1] = prev + cur;
        for (int i = cy; i < nlines - 1; i++) {
            int len = line_len[i + 1];
            for (int k = 0; k < len; k++) text[i][k] = text[i + 1][k];
            line_len[i] = len;
        }
        nlines--;
        cy--;
        cx = prev;
    }
}

/* ----- I/O ----- */
static void clear_buffer(void) {
    nlines = 1;
    line_len[0] = 0;
    cx = cy = scroll = 0;
}

static int load_file(const char *p) {
    static char buf[MAXLINES * 64];
    int64_t n = vos_fs_read(p, 0, (uint8_t*)buf, sizeof(buf) - 1);
    clear_buffer();
    if (n <= 0) { set_status("New file"); return 0; }
    buf[n] = 0;
    int line = 0;
    int col = 0;
    for (int64_t i = 0; i < n && line < MAXLINES; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            line_len[line] = col;
            line++;
            col = 0;
            if (line < MAXLINES) line_len[line] = 0;
            continue;
        }
        if (col < MAXCOLS - 1) {
            text[line][col++] = c;
        }
    }
    if (line < MAXLINES) line_len[line] = col;
    nlines = line + 1;
    set_status("Loaded");
    return 1;
}

static int save_file(const char *p) {
    static char buf[MAXLINES * 64];
    int o = 0;
    for (int i = 0; i < nlines; i++) {
        int len = line_len[i];
        if (o + len + 1 >= (int)sizeof(buf)) break;
        for (int k = 0; k < len; k++) buf[o++] = text[i][k];
        buf[o++] = '\n';
    }
    /* unlink + create + write — простой путь, перезаписываем целиком. */
    vos_fs_unlink(p);
    if (vos_fs_create(p, 0) != 0) { set_status("Save: cannot create"); return 0; }
    int64_t w = vos_fs_write(p, 0, (const uint8_t*)buf, o);
    if (w != o) { set_status("Save: write error"); return 0; }
    set_status("Saved");
    return 1;
}

/* ----- рендер ----- */
#define TOP_H        28      /* header c кнопками */
#define BOTTOM_H     30      /* строка статуса */
#define PAD          8

static int content_x(void) { return PAD; }
static int content_y(void) { return TOP_H + 8; }
static int content_h(void) { return win->h - TOP_H - BOTTOM_H - 16; }

static void render(void) {
    vui_clear(win, 0xFF1A1E2A);

    /* Header */
    vui_rect(win, 0, 0, win->w, TOP_H, 0xFF12151D);
    vui_hline(win, 0, TOP_H, win->w, 0xFF2A2F44);
    vui_text(win, 10, (TOP_H - VUI_TEXT_H) / 2, "vedit", VUI_COL_FG);
    if (vui_button(win, win->w - 200, 3, 80, TOP_H - 6, "Open",
                   VUI_COL_BTN, VUI_COL_FG)) focus_path = 1;
    if (vui_button(win, win->w - 110, 3, 90, TOP_H - 6, "Save",
                   VUI_COL_OK, 0xFFFFFFFF)) {
        save_file(path);
    }

    /* Контент */
    int cx0 = content_x();
    int cy0 = content_y();
    int ch  = content_h();
    int line_h = VUI_TEXT_H + 2;
    int visible = ch / line_h;

    /* Auto-scroll к курсору */
    if (cy < scroll) scroll = cy;
    if (cy >= scroll + visible) scroll = cy - visible + 1;
    if (scroll < 0) scroll = 0;

    for (int row = 0; row < visible; row++) {
        int li = scroll + row;
        if (li >= nlines) break;
        char buf[MAXCOLS + 1];
        int n = line_len[li];
        for (int k = 0; k < n; k++) buf[k] = text[li][k];
        buf[n] = 0;
        vui_text(win, cx0, cy0 + row * line_h, buf, VUI_COL_FG);
    }

    /* Курсор */
    if (!focus_path) {
        int row = cy - scroll;
        if (row >= 0 && row < visible) {
            char buf[MAXCOLS + 1];
            int n = cx;
            if (n > line_len[cy]) n = line_len[cy];
            for (int k = 0; k < n; k++) buf[k] = text[cy][k];
            buf[n] = 0;
            int px = cx0 + vui_text_width(buf);
            vui_rect(win, px, cy0 + row * line_h, 2, VUI_TEXT_H, VUI_COL_ACCENT);
        }
    }

    /* Bottom: путь + статус */
    int by = win->h - BOTTOM_H;
    vui_hline(win, 0, by, win->w, 0xFF2A2F44);
    vui_rect(win, 0, by + 1, win->w, BOTTOM_H - 1, 0xFF12151D);

    /* path input */
    int pw = win->w / 2;
    int input_x = PAD;
    int input_y = by + 5;
    int input_h = BOTTOM_H - 10;
    vui_rect(win, input_x, input_y, pw, input_h,
             focus_path ? 0xFF1F2940 : 0xFF1A1E2A);
    vui_frame(win, input_x, input_y, pw, input_h,
              focus_path ? VUI_COL_ACCENT : VUI_COL_DIM);
    vui_text(win, input_x + 6, input_y + (input_h - VUI_TEXT_H) / 2,
             path, VUI_COL_FG);

    /* status */
    if (status[0]) {
        int tw = vui_text_width(status);
        vui_text(win, win->w - tw - PAD, input_y + (input_h - VUI_TEXT_H) / 2,
                 status, VUI_COL_DIM);
    }

    /* Положение в строке (cy:cx) — справа */
    char pos[24];
    snprintf(pos, sizeof(pos), "%d:%d", cy + 1, cx + 1);
    int pw2 = vui_text_width(pos);
    vui_text(win, win->w - pw2 - PAD, (TOP_H - VUI_TEXT_H) / 2, pos, VUI_COL_DIM);

    vui_flush(win);
}

/* ----- ввод ----- */
static void path_input_char(char c) {
    int n = (int)strlen(path);
    if (c == '\b' || c == 127) {
        if (n > 0) path[n - 1] = 0;
        return;
    }
    if (c == '\n' || c == '\r') {
        focus_path = 0;
        load_file(path);
        return;
    }
    if (c == 27) { focus_path = 0; return; }   /* Esc */
    if (c >= 32 && c < 127 && n < PATH_MAX_ - 1) {
        path[n] = c; path[n + 1] = 0;
    }
}

static void editor_char(char c) {
    switch (c) {
    case 0x13: save_file(path); return;             /* Ctrl+S */
    case 0x0F: focus_path = 1; return;              /* Ctrl+O */
    case 27:   focus_path = 0; return;              /* Esc */
    case 0x01: cx = 0; return;                       /* Ctrl+A → home */
    case 0x05: cx = line_len[cy]; return;            /* Ctrl+E → end */
    case '\b':
    case 127:  backspace_(); return;
    case '\n':
    case '\r': insert_newline(); return;
    }
    if (c >= 32 && c < 127) insert_char(c);
}

int main(void) {
    win = vui_open("Text Editor", W, H);
    if (!win) return 1;
    set_status("Ready");
    /* Если файл существует — открыть, иначе чистый буфер */
    load_file(path);
    render();

    vui_event_t ev;
    while (vui_wait_event(win, &ev)) {
        if (ev.type == VUI_EV_KEY && ev.pressed) {
            char c = ev.ch;
            if (focus_path) path_input_char(c);
            else            editor_char(c);
        }
        if (ev.type == VUI_EV_MOUSE) {
            /* Клик в input пути — переключить focus */
            int by = win->h - BOTTOM_H;
            if (ev.y >= by + 5 && ev.y < by + BOTTOM_H - 5 &&
                ev.x >= PAD && ev.x < PAD + win->w / 2 &&
                (ev.buttons & 1)) {
                focus_path = 1;
            } else if (ev.buttons & 1) {
                focus_path = 0;
            }
        }
        render();
    }
    vui_close(win);
    (void)ch_w_cache;
    return 0;
}
