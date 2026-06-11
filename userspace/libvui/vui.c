/* =============================================================================
 * VortexOS — libvui/vui.c (реализация, см. vui.h)
 * ============================================================================= */
#include "vui.h"
#include "font8x16.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------- damage ----------------------------------- */
static void damage_add(vui_win_t *w, int x, int y, int rw, int rh) {
    /* клип к окну */
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w->w) rw = w->w - x;
    if (y + rh > w->h) rh = w->h - y;
    if (rw <= 0 || rh <= 0) return;

    if (w->dmg_w <= 0) {
        w->dmg_x = x; w->dmg_y = y; w->dmg_w = rw; w->dmg_h = rh;
        return;
    }
    int x1 = w->dmg_x + w->dmg_w, y1 = w->dmg_y + w->dmg_h;
    if (x < w->dmg_x) w->dmg_x = x;
    if (y < w->dmg_y) w->dmg_y = y;
    if (x + rw > x1) x1 = x + rw;
    if (y + rh > y1) y1 = y + rh;
    w->dmg_w = x1 - w->dmg_x;
    w->dmg_h = y1 - w->dmg_y;
}

/* --------------------------- жизненный цикл -------------------------------- */
vui_win_t *vui_open(const char *title, int w, int h) {
    vui_win_t *win = (vui_win_t *)calloc(1, sizeof(vui_win_t));
    if (!win) return 0;
    win->wm_pid = vwm_wait_for_wm();
    win->w = w;
    win->h = h;
    win->win_id = vwm_create_window(win->wm_pid, title, w, h, &win->surf);
    if (!win->win_id) { free(win); return 0; }
    return win;
}

void vui_close(vui_win_t *win) {
    if (!win) return;
    vwm_destroy(win->wm_pid, win->win_id);
    free(win);
}

/* ------------------------------- события ----------------------------------- */
int vui_wait_event(vui_win_t *win, vui_event_t *ev) {
    vos_msg_t m;
    for (;;) {
        ev->type = VUI_EV_NONE;
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        if (m.w[1] != win->win_id) continue;   /* чужое окно (у нас одно) */

        switch (m.w[0]) {
        case VWM_EV_KEY:
            ev->type    = VUI_EV_KEY;
            ev->ch      = (char)m.w[2];
            ev->pressed = (int)m.w[3];
            return 1;
        case VWM_EV_MOUSE:
            ev->type    = VUI_EV_MOUSE;
            ev->x       = (int)(int64_t)m.w[2];
            ev->y       = (int)(int64_t)m.w[3];
            ev->buttons = (int)m.w[4];
            if (ev->buttons & 1) {             /* ЛКМ — запомнить для виджетов */
                win->click_x = ev->x;
                win->click_y = ev->y;
                win->click_pending = 1;
            }
            return 1;
        case VWM_EV_RESIZE:
            win->w = (int)m.w[2];
            win->h = (int)m.w[3];
            ev->type = VUI_EV_RESIZE;
            ev->w = win->w;
            ev->h = win->h;
            return 1;
        case VWM_EV_CLOSE:
            ev->type = VUI_EV_CLOSE;
            return 0;
        default:
            break;                              /* неизвестное — игнор */
        }
    }
}

/* ------------------------------- рисование --------------------------------- */
void vui_rect(vui_win_t *win, int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= win->h) continue;
        uint32_t *row = win->surf + (uint32_t)py * win->w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= win->w) continue;
            row[px] = color;
        }
    }
    damage_add(win, x, y, w, h);
}

void vui_clear(vui_win_t *win, uint32_t color) {
    vui_rect(win, 0, 0, win->w, win->h, color);
}

void vui_hline(vui_win_t *win, int x, int y, int w, uint32_t color) {
    vui_rect(win, x, y, w, 1, color);
}

void vui_vline(vui_win_t *win, int x, int y, int h, uint32_t color) {
    vui_rect(win, x, y, 1, h, color);
}

void vui_frame(vui_win_t *win, int x, int y, int w, int h, uint32_t color) {
    vui_hline(win, x, y, w, color);
    vui_hline(win, x, y + h - 1, w, color);
    vui_vline(win, x, y, h, color);
    vui_vline(win, x + w - 1, y, h, color);
}

void vui_text(vui_win_t *win, int x, int y, const char *s, uint32_t color) {
    int cx = x;
    const char *p = s;
    while (*p) {
        uint8_t idx = (uint8_t)*p;
        if (idx >= 128) idx = '?';
        const unsigned char *glyph = vos_font[idx];
        for (int row = 0; row < 16; row++) {
            int py = y + row;
            if (py < 0 || py >= win->h) continue;
            uint8_t bits = glyph[row];
            if (!bits) continue;
            uint32_t *line = win->surf + (uint32_t)py * win->w;
            for (int col = 0; col < 8; col++) {
                if (!(bits & (0x80 >> col))) continue;
                int px = cx + col;
                if (px >= 0 && px < win->w) line[px] = color;
            }
        }
        cx += 8;
        p++;
    }
    damage_add(win, x, y, cx - x, VUI_TEXT_H);
}

int vui_text_width(const char *s) {
    return (int)strlen(s) * 8;
}

void vui_flush(vui_win_t *win) {
    if (win->dmg_w <= 0) return;
    vwm_commit(win->wm_pid, win->win_id,
               win->dmg_x, win->dmg_y, win->dmg_w, win->dmg_h);
    win->dmg_w = 0;
    win->dmg_h = 0;
}

/* --------------------------- immediate-mode виджеты ------------------------ */
int vui_click_in(vui_win_t *win, int x, int y, int w, int h) {
    return win->click_pending &&
           win->click_x >= x && win->click_x < x + w &&
           win->click_y >= y && win->click_y < y + h;
}

int vui_button(vui_win_t *win, int x, int y, int w, int h,
               const char *label, uint32_t bg, uint32_t fg) {
    int hit = vui_click_in(win, x, y, w, h);
    if (hit) win->click_pending = 0;            /* клик «съеден» */

    vui_rect(win, x, y, w, h, hit ? VUI_COL_BTN_HOT : bg);
    vui_frame(win, x, y, w, h, VUI_COL_ACCENT);
    int tw = vui_text_width(label);
    vui_text(win, x + (w - tw) / 2, y + (h - VUI_TEXT_H) / 2, label, fg);
    return hit;
}

int vui_checkbox(vui_win_t *win, int x, int y, const char *label, int checked) {
    const int box = 14;
    int w = box + 8 + vui_text_width(label);
    int hit = vui_click_in(win, x, y - 1, w, box + 2);
    if (hit) win->click_pending = 0;

    vui_rect(win, x, y, box, box, VUI_COL_PANEL);
    vui_frame(win, x, y, box, box, VUI_COL_ACCENT);
    if (checked ^ hit)                          /* hit → показать уже новое */
        vui_rect(win, x + 3, y + 3, box - 6, box - 6, VUI_COL_OK);
    vui_text(win, x + box + 8, y - 1, label, VUI_COL_FG);
    return hit;
}

void vui_progress(vui_win_t *win, int x, int y, int w, int h, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    vui_rect(win, x, y, w, h, VUI_COL_PANEL);
    vui_frame(win, x, y, w, h, VUI_COL_DIM);
    int fill = (w - 4) * percent / 100;
    if (fill > 0) vui_rect(win, x + 2, y + 2, fill, h - 4, VUI_COL_ACCENT);
}

void vui_label(vui_win_t *win, int x, int y, const char *s, uint32_t color) {
    vui_text(win, x, y, s, color);
}
