/* =============================================================================
 * VortexOS — kernel/drivers/wm.c
 * Простой window manager
 * ============================================================================= */

#include "wm.h"
#include "compositor.h"

static window_t windows[WM_MAX_WINDOWS];
static uint32_t next_window_id = 1;
static window_t *focused_window = 0;
static window_t *dragging_window = 0;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

void wm_init(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].id = 0;
        windows[i].visible = false;
    }
}

/* -------------------------------------------------------------------------
 * Создание/удаление окон
 * ---------------------------------------------------------------------- */

window_t *wm_create_window(const char *title, int x, int y, int w, int h) {
    /* Найти свободный слот */
    window_t *win = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].id == 0) {
            win = &windows[i];
            break;
        }
    }
    
    if (!win) return 0;
    
    win->id = next_window_id++;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->visible = false;
    win->focused = false;
    win->z_index = 0;
    win->paint = 0;
    win->on_click = 0;
    win->on_key = 0;
    win->on_close = 0;
    win->userdata = 0;
    
    /* Копируем заголовок */
    int i = 0;
    while (title[i] && i < 63) {
        win->title[i] = title[i];
        i++;
    }
    win->title[i] = 0;
    
    return win;
}

void wm_destroy_window(window_t *win) {
    if (!win) return;
    
    /* Debug: выводим какое окно уничтожается */
    extern void fb_puts(const char *);
    fb_puts("[WM] Destroying window: ");
    fb_puts(win->title);
    fb_puts("\n");
    
    if (win->on_close) win->on_close(win);
    if (focused_window == win) focused_window = 0;
    if (dragging_window == win) dragging_window = 0;
    win->id = 0;
    win->visible = false;
}

/* -------------------------------------------------------------------------
 * Управление окнами
 * ---------------------------------------------------------------------- */

void wm_show_window(window_t *win) {
    if (!win) return;
    win->visible = true;
    wm_focus_window(win);
}

void wm_hide_window(window_t *win) {
    if (!win) return;
    win->visible = false;
    if (focused_window == win) focused_window = 0;
}

void wm_move_window(window_t *win, int x, int y) {
    if (!win) return;
    
    /* Ограничиваем чтобы окно не уходило за границы экрана */
    compositor_t *comp = compositor_get();
    
    /* Минимум 20 пикселей заголовка должны быть видны */
    if (x < -win->w + 20) x = -win->w + 20;
    if (y < 0) y = 0;
    if (x > (int)comp->width - 20) x = comp->width - 20;
    if (y > (int)comp->height - WM_TITLE_BAR_HEIGHT - 32) y = comp->height - WM_TITLE_BAR_HEIGHT - 32;
    
    win->x = x;
    win->y = y;
}

void wm_resize_window(window_t *win, int w, int h) {
    if (!win) return;
    win->w = w;
    win->h = h;
}

void wm_focus_window(window_t *win) {
    if (!win) return;
    
    /* Снять фокус со всех окон */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].focused = false;
    }
    
    win->focused = true;
    focused_window = win;
    
    /* Поднять окно наверх (максимальный z_index) */
    int max_z = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].id != 0 && windows[i].z_index > max_z) {
            max_z = windows[i].z_index;
        }
    }
    win->z_index = max_z + 1;
}

/* -------------------------------------------------------------------------
 * Отрисовка
 * ---------------------------------------------------------------------- */

void wm_paint_window(window_t *win) {
    if (!win || !win->visible) return;
    
    compositor_t *comp = compositor_get();
    
    /* Тень под окном (как в VortexOS) */
    int shadow_offset = 4;
    comp_fill_rect(win->x + shadow_offset, win->y + shadow_offset, 
                   win->w, win->h, 0xFF000000 | 0x40404040); /* Полупрозрачная тень */
    
    /* Заголовок окна */
    uint32_t title_color = win->focused ? COLOR_VOS_ACCENT : COLOR_VOS_TITLE;
    comp_fill_rect(win->x, win->y, win->w, WM_TITLE_BAR_HEIGHT, title_color);
    
    /* Текст заголовка */
    comp_draw_string(win->x + 8, win->y + 4, win->title, COLOR_VOS_TEXT, title_color);
    
    /* Кнопка закрытия (крестик) */
    int close_x = win->x + win->w - 20;
    int close_y = win->y + 4;
    comp_fill_rect(close_x, close_y, 16, 16, COLOR_RED);
    comp_draw_line(close_x + 4, close_y + 4, close_x + 12, close_y + 12, COLOR_WHITE);
    comp_draw_line(close_x + 12, close_y + 4, close_x + 4, close_y + 12, COLOR_WHITE);
    
    /* Область содержимого */
    int content_y = win->y + WM_TITLE_BAR_HEIGHT;
    int content_h = win->h - WM_TITLE_BAR_HEIGHT;
    comp_fill_rect(win->x, content_y, win->w, content_h, COLOR_WHITE);
    
    /* Граница окна */
    comp_draw_rect(win->x, win->y, win->w, win->h, win->focused ? COLOR_VOS_BORDER : COLOR_DARKGRAY);
    
    /* Вызываем пользовательскую функцию отрисовки */
    if (win->paint) {
        win->paint(win);
    }
}

void wm_paint_all(void) {
    compositor_t *comp = compositor_get();
    
    /* Очищаем back buffer (рабочий стол) */
    comp_clear(COLOR_VOS_BG);
    
    /* Рисуем панель задач внизу */
    int taskbar_h = 32;
    comp_fill_rect(0, comp->height - taskbar_h, comp->width, taskbar_h, COLOR_VOS_PANEL);
    comp_draw_string(8, comp->height - taskbar_h + 8, "VortexOS", COLOR_VOS_TEXT, COLOR_VOS_PANEL);
    
    /* Рисуем окна в порядке z_index */
    for (int z = 0; z <= 1000; z++) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (windows[i].id != 0 && windows[i].z_index == z) {
                wm_paint_window(&windows[i]);
            }
        }
    }
    
    /* Курсор мыши рисуется в самом конце поверх всего */
    comp_draw_cursor();
    
    /* Копируем back buffer во framebuffer одним блоком — нет моргания! */
    comp_flip();
}

/* -------------------------------------------------------------------------
 * Обработка событий
 * ---------------------------------------------------------------------- */

void wm_handle_mouse_move(int dx, int dy) {
    compositor_t *comp = compositor_get();
    
    /* Инвертируем dy — мышь: вверх = положительное, экран: вверх = отрицательное */
    comp_update_mouse(dx, -dy, comp->mouse_buttons);
    
    /* Если тащим окно */
    if (dragging_window) {
        /* Проверяем что окно всё ещё существует */
        bool window_exists = false;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (&windows[i] == dragging_window && windows[i].id != 0) {
                window_exists = true;
                break;
            }
        }
        
        if (window_exists) {
            wm_move_window(dragging_window, comp->mouse_x - drag_offset_x, comp->mouse_y - drag_offset_y);
        } else {
            /* Окно было удалено — прекратить перетаскивание */
            dragging_window = 0;
        }
    }
}

void wm_handle_mouse_button(uint8_t buttons) {
    compositor_t *comp = compositor_get();
    uint8_t prev_buttons = comp->mouse_buttons;
    comp->mouse_buttons = buttons;
    
    /* Левая кнопка НАЖАТА (переход с 0 на 1) */
    if ((buttons & 0x01) && !(prev_buttons & 0x01)) {
        window_t *win = wm_find_window_at(comp->mouse_x, comp->mouse_y);
        
        if (win) {
            wm_focus_window(win);
            
            /* Проверяем клик по кнопке закрытия */
            int close_x = win->x + win->w - 20;
            int close_y = win->y + 4;
            if (comp->mouse_x >= close_x && comp->mouse_x < close_x + 16 &&
                comp->mouse_y >= close_y && comp->mouse_y < close_y + 16) {
                wm_destroy_window(win);
                return;
            }
            
            /* Клик по заголовку — начать перетаскивание */
            if (comp->mouse_y >= win->y && comp->mouse_y < win->y + WM_TITLE_BAR_HEIGHT) {
                dragging_window = win;
                drag_offset_x = comp->mouse_x - win->x;
                drag_offset_y = comp->mouse_y - win->y;
            } else {
                /* Клик внутри окна */
                int local_x = comp->mouse_x - win->x;
                int local_y = comp->mouse_y - (win->y + WM_TITLE_BAR_HEIGHT);
                if (win->on_click) {
                    win->on_click(win, local_x, local_y);
                }
            }
        }
    } 
    
    /* Левая кнопка ОТПУЩЕНА (переход с 1 на 0) */
    if (!(buttons & 0x01) && (prev_buttons & 0x01)) {
        /* Прекратить перетаскивание */
        dragging_window = 0;
    }
}

void wm_handle_key(char c) {
    if (focused_window && focused_window->on_key) {
        focused_window->on_key(focused_window, c);
    }
}

/* -------------------------------------------------------------------------
 * Утилиты
 * ---------------------------------------------------------------------- */

window_t *wm_find_window_at(int x, int y) {
    /* Ищем окно с максимальным z_index под курсором */
    window_t *found = 0;
    int max_z = -1;
    
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        window_t *win = &windows[i];
        if (win->id == 0 || !win->visible) continue;
        
        if (x >= win->x && x < win->x + win->w &&
            y >= win->y && y < win->y + win->h) {
            if (win->z_index > max_z) {
                max_z = win->z_index;
                found = win;
            }
        }
    }
    
    return found;
}

window_t *wm_get_focused_window(void) {
    return focused_window;
}
