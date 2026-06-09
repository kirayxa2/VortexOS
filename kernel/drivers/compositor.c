/* =============================================================================
 * VortexOS — kernel/drivers/compositor.c
 * Базовый compositor для графической оболочки
 * ============================================================================= */

#include "compositor.h"

static compositor_t comp;

/* Встроенный 8x16 bitmap шрифт объявлен в kmain.c */
extern const uint8_t font[128][16];

/* Динамический back buffer - будет установлен в compositor_init */
static uint32_t *back_buffer_storage = 0;
static uint32_t bb_width = 0;
static uint32_t bb_height = 0;

void compositor_init(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch) {
    extern void fb_puts(const char *);
    extern void fb_puthex(uint64_t);
    
    comp.fb_addr = fb;
    comp.width = w;
    comp.height = h;
    comp.pitch = pitch;
    comp.mouse_x = w / 2;
    comp.mouse_y = h / 2;
    comp.mouse_buttons = 0;
    
    bb_width = w;
    bb_height = h;
    
    fb_puts("[COMPOSITOR] Initializing with resolution ");
    fb_puthex(w);
    fb_puts("x");
    fb_puthex(h);
    fb_puts("\n");
    
    /* Выделяем back buffer через PMM */
    extern uint64_t pmm_alloc(void);
    extern void vmm_map(void*, uint64_t, uint64_t, uint64_t);
    extern void *vmm_kernel_pml4;
    
    uint64_t buffer_size = (uint64_t)w * h * 4;  // 4 bytes per pixel
    uint64_t num_pages = (buffer_size + 4095) / 4096;
    
    fb_puts("[COMPOSITOR] Allocating ");
    fb_puthex(num_pages);
    fb_puts(" pages (");
    fb_puthex(buffer_size);
    fb_puts(" bytes) for back buffer\n");
    
    uint64_t vaddr = 0xFFFFFFFF90000000ULL;  // Virtual address for back buffer
    
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (phys == 0) {
            fb_puts("[COMPOSITOR] ERROR: PMM allocation failed!\n");
            // Fallback to NULL - will crash but at least we know why
            back_buffer_storage = 0;
            comp.back_buffer = 0;
            return;
        }
        vmm_map(vmm_kernel_pml4, vaddr + i * 4096, phys, 0x03);  // PRESENT | WRITABLE
    }
    
    back_buffer_storage = (uint32_t *)vaddr;
    comp.back_buffer = back_buffer_storage;
    
    fb_puts("[COMPOSITOR] Back buffer allocated at ");
    fb_puthex(vaddr);
    fb_puts("\n");
    
    /* Очищаем экран */
    comp_clear(COLOR_VOS_BG);
}

compositor_t *compositor_get(void) {
    return &comp;
}

/* -------------------------------------------------------------------------
 * Примитивы
 * ---------------------------------------------------------------------- */

void comp_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)bb_width || y < 0 || y >= (int)bb_height)
        return;
    /* Рисуем в back buffer */
    comp.back_buffer[y * bb_width + x] = color;
}

/* Быстрый блит буфера окна целыми строками.
 * В отличие от попиксельного comp_put_pixel (cross-TU вызов + 4 проверки
 * границ на КАЖДЫЙ пиксель), здесь все проверки вынесены за внутренний цикл,
 * а сам цикл копирования компилятор векторизует на -O2. */
void comp_blit_buffer(int dx, int dy, int w, int h, const uint32_t *src) {
    if (!comp.back_buffer || !src) return;
    for (int row = 0; row < h; row++) {
        int y = dy + row;
        if (y < 0 || y >= (int)bb_height) continue;

        int x0 = dx, sx0 = 0, ww = w;
        if (x0 < 0) { sx0 = -x0; ww += x0; x0 = 0; }       /* клип слева */
        if (x0 + ww > (int)bb_width) ww = (int)bb_width - x0; /* клип справа */
        if (ww <= 0) continue;

        uint32_t       *d = &comp.back_buffer[(uint32_t)y * bb_width + x0];
        const uint32_t *s = &src[(uint32_t)row * w + sx0];
        for (int i = 0; i < ww; i++) d[i] = s[i];
    }
}

void comp_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    /* Bresenham's line algorithm */
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        comp_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void comp_draw_rect(int x, int y, int w, int h, uint32_t color) {
    comp_draw_line(x, y, x + w - 1, y, color);           /* Верх */
    comp_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color); /* Низ */
    comp_draw_line(x, y, x, y + h - 1, color);           /* Лево */
    comp_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color); /* Право */
}

void comp_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            comp_put_pixel(x + dx, y + dy, color);
        }
    }
}

void comp_draw_circle(int cx, int cy, int radius, uint32_t color) {
    /* Midpoint circle algorithm */
    int x = 0;
    int y = radius;
    int d = 1 - radius;
    
    while (x <= y) {
        comp_put_pixel(cx + x, cy + y, color);
        comp_put_pixel(cx + y, cy + x, color);
        comp_put_pixel(cx - x, cy + y, color);
        comp_put_pixel(cx - y, cy + x, color);
        comp_put_pixel(cx + x, cy - y, color);
        comp_put_pixel(cx + y, cy - x, color);
        comp_put_pixel(cx - x, cy - y, color);
        comp_put_pixel(cx - y, cy - x, color);
        
        if (d < 0) {
            d = d + 2 * x + 3;
        } else {
            d = d + 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void comp_fill_circle(int cx, int cy, int radius, uint32_t color) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                comp_put_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Текст
 * ---------------------------------------------------------------------- */

void comp_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = '?';
    
    const uint8_t *glyph = font[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            comp_put_pixel(x + col, y + row, color);
        }
    }
}

void comp_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += 16;
        } else {
            comp_draw_char(cx, y, *s, fg, bg);
            cx += 8;
        }
        s++;
    }
}

/* -------------------------------------------------------------------------
 * Курсор мыши
 * ---------------------------------------------------------------------- */

void comp_draw_cursor(void) {
    int x = comp.mouse_x;
    int y = comp.mouse_y;
    
    /* Простой курсор-стрелка 11x17 пикселей */
    for (int dy = 0; dy < 17; dy++) {
        int width = (dy < 11) ? (dy + 1) : (17 - dy);
        for (int dx = 0; dx < width; dx++) {
            comp_put_pixel(x + dx, y + dy, COLOR_WHITE);
        }
        /* Контур черный */
        if (width < 11) {
            comp_put_pixel(x + width, y + dy, COLOR_BLACK);
        }
    }
}

void comp_update_mouse(int dx, int dy, uint8_t buttons) {
    comp.mouse_x += dx;
    comp.mouse_y += dy;
    
    /* Ограничиваем экраном */
    if (comp.mouse_x < 0) comp.mouse_x = 0;
    if (comp.mouse_y < 0) comp.mouse_y = 0;
    if (comp.mouse_x >= (int)comp.width)  comp.mouse_x = comp.width - 1;
    if (comp.mouse_y >= (int)comp.height) comp.mouse_y = comp.height - 1;
    
    comp.mouse_buttons = buttons;
}

/* -------------------------------------------------------------------------
 * Утилиты
 * ---------------------------------------------------------------------- */

void comp_clear(uint32_t color) {
    for (uint32_t i = 0; i < bb_width * bb_height; i++) {
        comp.back_buffer[i] = color;
    }
}

void comp_refresh(void) {
    /* Ничего не делаем — курсор рисуется в wm_paint_all() */
}

void comp_flip(void) {
    /* Копируем back buffer в front buffer.
     * Программный аналог page-flip: настоящего переключения указателя VRAM без
     * GPU-драйвера нет, но копию делаем тугим 32-битным циклом (его -O2
     * векторизует / превращает в memcpy), а не попиксельно с пересчётом индексов. */
    uint32_t copy_w = comp.width < bb_width ? comp.width : bb_width;
    uint32_t copy_h = comp.height < bb_height ? comp.height : bb_height;
    uint32_t fb_stride = comp.pitch / 4;

    if (fb_stride == bb_width && copy_w == bb_width) {
        /* Строки идут вплотную в обоих буферах — копируем одним проходом. */
        uint32_t total = copy_h * bb_width;
        uint32_t *d = comp.fb_addr;
        const uint32_t *s = comp.back_buffer;
        for (uint32_t i = 0; i < total; i++) d[i] = s[i];
        return;
    }

    /* Общий случай (pitch != width*4): копируем построчно. */
    for (uint32_t y = 0; y < copy_h; y++) {
        uint32_t *d = &comp.fb_addr[y * fb_stride];
        const uint32_t *s = &comp.back_buffer[y * bb_width];
        for (uint32_t x = 0; x < copy_w; x++) d[x] = s[x];
    }
}
