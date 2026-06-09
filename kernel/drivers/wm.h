#ifndef VOS_WM_H
#define VOS_WM_H

#include "types.h"

#define WM_MAX_WINDOWS 32
#define WM_TITLE_BAR_HEIGHT 24

typedef struct window {
    uint32_t id;
    char title[64];
    int x, y, w, h;
    bool visible;
    bool focused;
    int z_index;
    
    /* Содержимое окна (рисуется функцией) */
    void (*paint)(struct window *win);
    void (*on_click)(struct window *win, int x, int y);
    void (*on_key)(struct window *win, char c);
    void (*on_close)(struct window *win);
    
    void *userdata; /* Произвольные данные приложения */
} window_t;

/* Инициализация window manager */
void wm_init(void);

/* Создание/удаление окон */
window_t *wm_create_window(const char *title, int x, int y, int w, int h);
void wm_destroy_window(window_t *win);

/* Управление окнами */
void wm_show_window(window_t *win);
void wm_hide_window(window_t *win);
void wm_move_window(window_t *win, int x, int y);
void wm_resize_window(window_t *win, int w, int h);
void wm_focus_window(window_t *win);

/* Отрисовка */
void wm_paint_all(void);
void wm_paint_window(window_t *win);

/* Обработка событий */
void wm_handle_mouse_move(int dx, int dy);
void wm_handle_mouse_button(uint8_t buttons);
void wm_handle_key(char c);

/* Утилиты */
window_t *wm_find_window_at(int x, int y);
window_t *wm_get_focused_window(void);

#endif
