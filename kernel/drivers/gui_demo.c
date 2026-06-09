/* =============================================================================
 * VortexOS — kernel/drivers/gui_demo.c
 * Демонстрация графического интерфейса
 * ============================================================================= */

#include "wm.h"
#include "compositor.h"

/* -------------------------------------------------------------------------
 * Простое окно с текстом
 * ---------------------------------------------------------------------- */

static void text_window_paint(window_t *win) {
    /* Область содержимого окна */
    int content_x = win->x;
    int content_y = win->y + WM_TITLE_BAR_HEIGHT;
    
    /* Рисуем текст */
    comp_draw_string(content_x + 16, content_y + 16, 
                     "VortexOS GUI Demo", 
                     COLOR_BLACK, COLOR_WHITE);
    comp_draw_string(content_x + 16, content_y + 36, 
                     "Window Manager works!", 
                     COLOR_VOS_ACCENT, COLOR_WHITE);
}

static void text_window_click(window_t *win, int x, int y) {
    /* Рисуем кружок в месте клика */
    int content_x = win->x;
    int content_y = win->y + WM_TITLE_BAR_HEIGHT;
    comp_fill_circle(content_x + x, content_y + y, 10, COLOR_RED);
}

/* -------------------------------------------------------------------------
 * Окно с кнопками
 * ---------------------------------------------------------------------- */

static void button_window_paint(window_t *win) {
    int content_x = win->x;
    int content_y = win->y + WM_TITLE_BAR_HEIGHT;
    
    /* Кнопка 1 */
    comp_fill_rect(content_x + 20, content_y + 20, 100, 30, COLOR_VOS_ACCENT);
    comp_draw_rect(content_x + 20, content_y + 20, 100, 30, COLOR_VOS_BORDER);
    comp_draw_string(content_x + 35, content_y + 28, "Button 1", COLOR_WHITE, COLOR_VOS_ACCENT);
    
    /* Кнопка 2 */
    comp_fill_rect(content_x + 20, content_y + 60, 100, 30, COLOR_GREEN);
    comp_draw_rect(content_x + 20, content_y + 60, 100, 30, COLOR_DARKGRAY);
    comp_draw_string(content_x + 35, content_y + 68, "Button 2", COLOR_BLACK, COLOR_GREEN);
}

static void button_window_click(window_t *win, int x, int y) {
    int content_y_rel = y; /* Относительно content area */
    
    /* Проверяем клик по кнопке 1 */
    if (x >= 20 && x < 120 && content_y_rel >= 20 && content_y_rel < 50) {
        /* Создаём новое окно */
        window_t *new_win = wm_create_window("New Window", 300, 300, 320, 200);
        if (new_win) {
            new_win->paint = text_window_paint;
            new_win->on_click = text_window_click;
            wm_show_window(new_win);
        }
    }
    
    /* Проверяем клик по кнопке 2 */
    if (x >= 20 && x < 120 && content_y_rel >= 60 && content_y_rel < 90) {
        /* Меняем заголовок окна */
        int i = 0;
        const char *new_title = "Button Clicked!";
        while (new_title[i] && i < 63) {
            win->title[i] = new_title[i];
            i++;
        }
        win->title[i] = 0;
    }
}

/* -------------------------------------------------------------------------
 * Главная задача GUI
 * ---------------------------------------------------------------------- */

static bool gui_dirty = true;  /* Флаг что нужна перерисовка */

void gui_demo_task(void) {
    extern void fb_puts(const char *);
    fb_puts("[GUI] Initializing compositor...\n");
    
    /* Получаем framebuffer из kmain */
    extern uint32_t *fb_addr;
    extern uint64_t fb_pitch;
    extern uint32_t fb_width;
    extern uint32_t fb_height;
    
    /* Используем реальный размер framebuffer */
    compositor_init(fb_addr, fb_width, fb_height, fb_pitch);
    wm_init();
    
    fb_puts("[GUI] Creating demo windows...\n");
    
    /* Создаём окно 1 */
    window_t *win1 = wm_create_window("Welcome", 100, 100, 400, 200);
    if (win1) {
        win1->paint = text_window_paint;
        win1->on_click = text_window_click;
        wm_show_window(win1);
    }
    
    /* Создаём окно 2 */
    window_t *win2 = wm_create_window("Buttons", 520, 120, 300, 180);
    if (win2) {
        win2->paint = button_window_paint;
        win2->on_click = button_window_click;
        wm_show_window(win2);
    }
    
    fb_puts("[GUI] Entering main loop...\n");
    
    extern uint64_t pit_ticks(void);
    uint64_t last_frame = pit_ticks();
    
    /* Главный цикл — ~30 FPS с double buffering (без моргания) */
    for (;;) {
        uint64_t now = pit_ticks();
        
        /* Перерисовываем 30 раз в секунду (каждые 3-4 тика при 100Hz) */
        if (now - last_frame >= 3) {
            wm_paint_all();
            last_frame = now;
        }
        
        /* Небольшая задержка чтобы не грузить CPU */
        __asm__ volatile("hlt");  /* Ждём следующего прерывания */
    }
}
