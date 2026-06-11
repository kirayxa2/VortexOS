/* =============================================================================
 * VortexOS — libvui: мини-тулкит GUI поверх протокола /bin/vwm
 *
 * Что даёт по сравнению с голым vos_abi.h:
 *   - vui_open()/vui_close(): окно одним вызовом (shm + CREATE + CREATED);
 *   - рисование с авто-damage: библиотека копит грязный прямоугольник,
 *     vui_flush() коммитит ровно его (не весь кадр);
 *   - vui_wait_event(): IPC-сообщения vwm → нормальные события,
 *     resize обрабатывается сам (w/h в структуре всегда актуальны);
 *   - immediate-mode виджеты: кнопка, чекбокс, прогрессбар, метка.
 *     Паттерн как в Dear ImGui: каждый кадр перерисовываешь UI заново,
 *     vui_button() сам говорит «по мне кликнули».
 *
 * Требует libc (malloc, string) — собирать с -D__VLIBC__ -Ilibc/include.
 *
 * Каркас приложения:
 *     vui_win_t *w = vui_open("My App", 400, 300);
 *     render(w);                              // рисуем + vui_flush(w)
 *     vui_event_t ev;
 *     while (vui_wait_event(w, &ev)) {        // 0 = пора выходить (CLOSE)
 *         if (ev.type == VUI_EV_MOUSE || ev.type == VUI_EV_RESIZE ||
 *             ev.type == VUI_EV_KEY) render(w);
 *     }
 *     vui_close(w);
 * ============================================================================= */
#ifndef VUI_H
#define VUI_H

#include <stdint.h>
#include "vos.h"

/* ------------------------------- палитра --------------------------------- */
/* Дефолтная тема в духе vwm/vdemo/vfiles — пользуйся или задавай свои цвета. */
#define VUI_COL_BG       0xFF20202E
#define VUI_COL_PANEL    0xFF16161F
#define VUI_COL_ACCENT   0xFF007ACC
#define VUI_COL_FG       0xFFE0E0E0
#define VUI_COL_DIM      0xFF9090A8
#define VUI_COL_BTN      0xFF2E4A6E
#define VUI_COL_BTN_HOT  0xFF3C6090
#define VUI_COL_OK       0xFF3CB371
#define VUI_COL_WARN     0xFFE8B64C
#define VUI_COL_ERR      0xFFD9534F

/* ------------------------------- события --------------------------------- */
enum {
    VUI_EV_NONE = 0,
    VUI_EV_KEY,        /* ch (ascii), pressed */
    VUI_EV_MOUSE,      /* x, y (коорд. содержимого), buttons (0 = отпустили) */
    VUI_EV_RESIZE,     /* w, h — новые размеры (в vui_win_t уже обновлены) */
    VUI_EV_CLOSE,      /* пользователь закрыл окно (vui_wait_event вернёт 0) */
};

typedef struct {
    int  type;
    char ch;           /* VUI_EV_KEY */
    int  pressed;      /* VUI_EV_KEY: 1 = нажата */
    int  x, y;         /* VUI_EV_MOUSE */
    int  buttons;      /* VUI_EV_MOUSE: бит 0 = ЛКМ */
    int  w, h;         /* VUI_EV_RESIZE */
} vui_event_t;

/* -------------------------------- окно ------------------------------------ */
typedef struct {
    uint64_t  wm_pid;
    uint64_t  win_id;
    uint32_t *surf;     /* shm-поверхность, stride = w */
    int       w, h;     /* текущие размеры содержимого */

    /* накопленный damage (валиден, если dmg_w > 0) */
    int dmg_x, dmg_y, dmg_w, dmg_h;

    /* состояние клика для immediate-mode виджетов */
    int click_x, click_y, click_pending;
} vui_win_t;

/* --------------------------- окно: жизненный цикл ------------------------- */
vui_win_t *vui_open(const char *title, int w, int h);   /* 0 = ошибка */
void       vui_close(vui_win_t *win);                   /* DESTROY + free */

/* Блокирующее ожидание события (задача спит). Возвращает 0 на VWM_EV_CLOSE,
 * иначе 1. RESIZE сам обновляет win->w/h; клик ЛКМ запоминается для виджетов. */
int vui_wait_event(vui_win_t *win, vui_event_t *ev);

/* ------------------------------- рисование -------------------------------- */
void vui_clear(vui_win_t *win, uint32_t color);
void vui_rect(vui_win_t *win, int x, int y, int w, int h, uint32_t color);
void vui_frame(vui_win_t *win, int x, int y, int w, int h, uint32_t color);
void vui_hline(vui_win_t *win, int x, int y, int w, uint32_t color);
void vui_vline(vui_win_t *win, int x, int y, int h, uint32_t color);
void vui_text(vui_win_t *win, int x, int y, const char *s, uint32_t color);
int  vui_text_width(const char *s);     /* пикселей (8 на символ) */
#define VUI_TEXT_H 16

/* Закоммитить накопленный damage в vwm (и сбросить его). */
void vui_flush(vui_win_t *win);

/* ------------------------- immediate-mode виджеты ------------------------- */
/* Рисует кнопку и возвращает 1, если последний клик попал в неё
 * (клик при этом «съедается»). Вызывать при каждой перерисовке. */
int  vui_button(vui_win_t *win, int x, int y, int w, int h,
                const char *label, uint32_t bg, uint32_t fg);

/* Чекбокс: рисует бокс + подпись, возвращает 1 при клике (тогда инвертируй
 * своё состояние и перерисуй). checked — текущее состояние. */
int  vui_checkbox(vui_win_t *win, int x, int y, const char *label, int checked);

/* Прогрессбар, percent 0..100. */
void vui_progress(vui_win_t *win, int x, int y, int w, int h, int percent);

/* Метка (просто текст, для симметрии API). */
void vui_label(vui_win_t *win, int x, int y, const char *s, uint32_t color);

/* Попал ли последний клик в прямоугольник (без «съедания» клика). */
int  vui_click_in(vui_win_t *win, int x, int y, int w, int h);

#endif /* VUI_H */
