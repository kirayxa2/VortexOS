#ifndef VOS_COMPOSITOR_H
#define VOS_COMPOSITOR_H

#include "types.h"

/* Цвета */
#define COLOR_BLACK       0xFF000000
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_GRAY        0xFFC0C0C0
#define COLOR_DARKGRAY    0xFF808080
#define COLOR_RED         0xFFFF0000
#define COLOR_GREEN       0xFF00FF00
#define COLOR_BLUE        0xFF0000FF
#define COLOR_CYAN        0xFF00FFFF
#define COLOR_MAGENTA     0xFFFF00FF
#define COLOR_YELLOW      0xFFFFFF00
#define COLOR_ORANGE      0xFFFF8000

/* Темная тема VortexOS */
#define COLOR_VOS_BG      0xFF1A1A2E  // Темный фон
#define COLOR_VOS_PANEL   0xFF2D2D30  // Панель
#define COLOR_VOS_TITLE   0xFF3E3E42  // Заголовок окна
#define COLOR_VOS_TEXT    0xFFE0E0E0  // Текст
#define COLOR_VOS_BORDER  0xFF007ACC  // Синяя граница
#define COLOR_VOS_ACCENT  0xFF0E639C  // Акцент

/* Структура для отрисовки */
typedef struct {
    uint32_t *fb_addr;     /* Framebuffer адрес (основной) */
    uint32_t *back_buffer; /* Задний буфер для double buffering */
    uint32_t  width;       /* Ширина экрана */
    uint32_t  height;      /* Высота экрана */
    uint32_t  pitch;       /* Bytes per line */
    int       mouse_x;     /* Позиция курсора */
    int       mouse_y;
    uint8_t   mouse_buttons; /* Состояние кнопок мыши */
} compositor_t;

/* Инициализация */
void compositor_init(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch);
compositor_t *compositor_get(void);

/* Примитивы */
void comp_put_pixel(int x, int y, uint32_t color);
/* Быстрый блит готового буфера (целыми строками) в back buffer.
 * dx,dy — позиция в экранных координатах; src — w*h пикселей ARGB. */
void comp_blit_buffer(int dx, int dy, int w, int h, const uint32_t *src);
void comp_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void comp_draw_rect(int x, int y, int w, int h, uint32_t color);
void comp_fill_rect(int x, int y, int w, int h, uint32_t color);
void comp_draw_circle(int cx, int cy, int radius, uint32_t color);
void comp_fill_circle(int cx, int cy, int radius, uint32_t color);

/* Текст (используем встроенный 8x16 шрифт из kmain.c) */
void comp_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void comp_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Курсор мыши */
void comp_draw_cursor(void);
void comp_update_mouse(int dx, int dy, uint8_t buttons);

/* Обновление экрана */
void comp_clear(uint32_t color);
void comp_refresh(void);
void comp_flip(void);  /* Копирует back buffer во front buffer */

#endif
