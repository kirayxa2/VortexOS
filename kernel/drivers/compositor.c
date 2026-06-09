/* =============================================================================
 * VortexOS — kernel/drivers/compositor.c
 * Базовый compositor для графической оболочки
 * ============================================================================= */

#include "compositor.h"
#include "virtio_gpu.h"

static compositor_t comp;

/* Встроенный 8x16 bitmap шрифт объявлен в kmain.c */
extern const uint8_t font[128][16];

/* Динамический back buffer - будет установлен в compositor_init */
static uint32_t *back_buffer_storage = 0;
static uint32_t bb_width = 0;
static uint32_t bb_height = 0;

/* -------------------------------------------------------------------------
 * VSync — устранение разрывов (tearing)
 *
 * Разрывы возникают потому, что мы копируем back buffer в scanout-буфер
 * (Limine linear framebuffer = то, что прямо сейчас читает видеолуч) в
 * произвольный момент. Если копия идёт, пока луч в видимой области, верх кадра
 * успевает получить новые пиксели, низ — старые → горизонтальный «разрыв».
 *
 * Лечится синхронизацией копии с вертикальным гашением (vblank): ждём начала
 * vblank, и только потом копируем. Аппаратного page-flip без GPU-драйвера нет,
 * поэтому ждём vblank программно через VGA Input Status Register 1 (порт 0x3DA),
 * бит 3 = идёт вертикальная развёртка.
 *
 * ВАЖНО: на части реального UEFI/GOP-железа legacy-регистр 0x3DA может быть не
 * подключён. Чтобы это никогда не вешало ядро, оба цикла ограничены таймаутом —
 * если vblank «не приходит», просто выходим и копируем как раньше. */
static int vsync_enabled = 1;   /* можно выключить, если на железе хуже */

static inline uint8_t vga_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void vsync_wait(void) {
    if (!vsync_enabled) return;
    uint32_t guard;
    /* Если мы уже внутри vblank — дождёмся его конца (иначе поймаем хвост). */
    guard = 1000000;
    while ((vga_inb(0x3DA) & 0x08) && --guard) { }
    if (!guard) { vsync_enabled = 0; return; }   /* регистр мёртв → больше не ждём */
    /* Ждём начала следующего vblank — лучший момент для копии. */
    guard = 1000000;
    while (!(vga_inb(0x3DA) & 0x08) && --guard) { }
    if (!guard) vsync_enabled = 0;
}

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

uint32_t comp_get_pixel(int x, int y) {
    if (x < 0 || x >= (int)bb_width || y < 0 || y >= (int)bb_height)
        return 0;
    return comp.back_buffer[y * bb_width + x];
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
 * Damage rectangles (#2)
 *
 * Вместо того чтобы на каждый кадр копировать ВЕСЬ back buffer во front buffer
 * (целый экран -> огромные разрывы/tearing при движении курсора), мы копируем
 * только изменившиеся прямоугольники. Полный кадр (recomposite окон) помечает
 * весь экран как damage; движение курсора — только два мелких прямоугольника
 * (старое и новое место спрайта).
 * ---------------------------------------------------------------------- */
typedef struct { int x, y, w, h; } comp_rect_t;
#define MAX_DAMAGE 32
static comp_rect_t damage_rects[MAX_DAMAGE];
static int  damage_count = 0;
static bool damage_full  = false;   /* перерисовать весь экран */

void comp_damage_reset(void) {
    damage_count = 0;
    damage_full  = false;
}

void comp_damage_full(void) {
    damage_full = true;
}

void comp_damage_add(int x, int y, int w, int h) {
    if (damage_full) return;
    /* Клип к размеру буфера */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)bb_width)  w = (int)bb_width  - x;
    if (y + h > (int)bb_height) h = (int)bb_height - y;
    if (w <= 0 || h <= 0) return;
    /* Слишком много прямоугольников — дешевле перелить весь экран. */
    if (damage_count >= MAX_DAMAGE) { damage_full = true; return; }
    damage_rects[damage_count].x = x;
    damage_rects[damage_count].y = y;
    damage_rects[damage_count].w = w;
    damage_rects[damage_count].h = h;
    damage_count++;
}

/* Копирует один прямоугольник из back buffer во front buffer (с клипом и
 * учётом pitch). Один тугой проход по строке — компилятор векторизует. */
static void blit_region_to_front(int x, int y, int w, int h) {
    if (!comp.back_buffer || !comp.fb_addr) return;
    uint32_t fb_stride = comp.pitch / 4;
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= (int)comp.height || yy >= (int)bb_height) continue;
        int x0 = x, ww = w;
        if (x0 < 0) { ww += x0; x0 = 0; }
        if (x0 + ww > (int)comp.width) ww = (int)comp.width - x0;
        if (x0 + ww > (int)bb_width)   ww = (int)bb_width   - x0;
        if (ww <= 0) continue;
        uint32_t       *d = &comp.fb_addr[(uint32_t)yy * fb_stride + x0];
        const uint32_t *s = &comp.back_buffer[(uint32_t)yy * bb_width + x0];
        for (int i = 0; i < ww; i++) d[i] = s[i];
    }
}

/* Выводит на экран только damage-прямоугольники (или весь экран, если стоит
 * флаг damage_full). После — список сбрасывается. */
void comp_present(void) {
    /* --- Путь virtio-gpu: настоящий аппаратный present без разрывов --------
     * Здесь comp.fb_addr — это backing GPU-ресурса. Копируем damage из back
     * buffer в backing, затем TRANSFER_TO_HOST_2D + RESOURCE_FLUSH на тот же
     * прямоугольник. RESOURCE_FLUSH показывает кадр атомарно — vblank-busy-wait
     * не нужен (его и убираем, чтобы не стопорить IRQ). */
    if (virtio_gpu_active()) {
        if (damage_full) {
            comp_flip();
            virtio_gpu_flush(0, 0, (int)comp.width, (int)comp.height);
            comp_damage_reset();
            return;
        }
        for (int i = 0; i < damage_count; i++) {
            blit_region_to_front(damage_rects[i].x, damage_rects[i].y,
                                 damage_rects[i].w, damage_rects[i].h);
            virtio_gpu_flush(damage_rects[i].x, damage_rects[i].y,
                             damage_rects[i].w, damage_rects[i].h);
        }
        comp_damage_reset();
        return;
    }

    /* --- Путь Limine framebuffer (software vsync, как раньше) -------------- */
    /* Ждём vblank ОДИН раз перед любой записью в scanout-буфер — так копия
     * (damage-прямоугольники или весь flip) ложится в гашение, без разрывов. */
    vsync_wait();
    if (damage_full) {
        comp_flip();
        comp_damage_reset();
        return;
    }
    for (int i = 0; i < damage_count; i++) {
        blit_region_to_front(damage_rects[i].x, damage_rects[i].y,
                             damage_rects[i].w, damage_rects[i].h);
    }
    comp_damage_reset();
}

/* -------------------------------------------------------------------------
 * Save-under софт-курсор (#1) + damage (#2) — без мигания
 *
 * Курсор компонуется ПРЯМО В back buffer поверх готовой сцены, с сохранением
 * перекрытых пикселей (save-under). За счёт этого:
 *   • Полный comp_flip больше НЕ стирает курсор (он часть кадра) — нет мигания.
 *   • Движение курсора = unblit(старое) + blit(новое) в back buffer, затем
 *     present только двух мелких damage-прямоугольников. Каждый прямоугольник
 *     уходит во front buffer ОДНОЙ записью (фон+курсор уже скомпонованы), без
 *     промежуточного «стерли фон → рисуем курсор», т.е. без разрывов/мигания.
 *
 * Инвариант: между кадрами back buffer чистый (без курсора) ВЕЗДЕ, кроме места,
 * где курсор сейчас «вкомпонован», а cursor_saveunder хранит фон под ним.
 * ---------------------------------------------------------------------- */
#define CURSOR_SPRITE_W 12
#define CURSOR_SPRITE_H 18

static uint32_t cursor_saveunder[CURSOR_SPRITE_W * CURSOR_SPRITE_H];
static int  cursor_drawn_x = 0;
static int  cursor_drawn_y = 0;
static bool cursor_in_back = false;

/* Сохраняет фон под курсором и рисует спрайт-стрелку прямо в back buffer. */
static void cursor_blit_back(void) {
    int x = comp.mouse_x;
    int y = comp.mouse_y;
    /* save-under: запоминаем фон, который перекроет курсор */
    for (int dy = 0; dy < CURSOR_SPRITE_H; dy++) {
        for (int dx = 0; dx < CURSOR_SPRITE_W; dx++) {
            int px = x + dx, py = y + dy;
            uint32_t v = 0;
            if (px >= 0 && px < (int)bb_width && py >= 0 && py < (int)bb_height)
                v = comp.back_buffer[(uint32_t)py * bb_width + px];
            cursor_saveunder[dy * CURSOR_SPRITE_W + dx] = v;
        }
    }
    /* спрайт-стрелка (11x17 + чёрный контур) — рисуем в back buffer */
    for (int dy = 0; dy < 17; dy++) {
        int width = (dy < 11) ? (dy + 1) : (17 - dy);
        for (int dx = 0; dx < width; dx++) comp_put_pixel(x + dx, y + dy, COLOR_WHITE);
        if (width < 11) comp_put_pixel(x + width, y + dy, COLOR_BLACK);
    }
    cursor_drawn_x = x;
    cursor_drawn_y = y;
    cursor_in_back = true;
}

/* Восстанавливает фон из save-under (стирает курсор из back buffer). */
static void cursor_unblit_back(void) {
    if (!cursor_in_back) return;
    int x = cursor_drawn_x, y = cursor_drawn_y;
    for (int dy = 0; dy < CURSOR_SPRITE_H; dy++) {
        for (int dx = 0; dx < CURSOR_SPRITE_W; dx++) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < (int)bb_width && py >= 0 && py < (int)bb_height)
                comp.back_buffer[(uint32_t)py * bb_width + px] =
                    cursor_saveunder[dy * CURSOR_SPRITE_W + dx];
        }
    }
    cursor_in_back = false;
}

/* Полный кадр: сцена только что перекомпонована (comp_clear затёр всё, включая
 * курсор), поэтому просто вкомпоновываем курсор поверх свежего фона. Вызывать
 * ДО comp_present()/comp_damage_full(). */
void comp_cursor_compose(void) {
    cursor_in_back = false;      /* старый save-under недействителен после clear */
    cursor_blit_back();
    /* Помечаем прямоугольник курсора как damage — чтобы частичный present
     * (wm_render_region) скопировал свежий курсор во front buffer, даже если он
     * у края/вне перерисованного окна. На полном кадре damage_full перекроет. */
    comp_damage_add(cursor_drawn_x, cursor_drawn_y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
}

/* Снять курсор из back buffer ДО частичной перекомпоновки окна (wm_render_region).
 *
 * Зачем: при частичном рендере comp_clear НЕ затирает старый спрайт курсора (он
 * может быть вне перерисовываемого окна). Если просто бросить cursor_in_back=false
 * и заново вкомпоновать курсор, то cursor_blit_back сохранит в save-under ПИКСЕЛИ
 * САМОГО КУРСОРА (он ещё в буфере), и при следующем движении мыши они «штампуются»
 * обратно → белые следы / эффект «стёрки» по тексту.
 *
 * Поэтому здесь восстанавливаем фон из save-under (back buffer снова чистый на
 * старом месте) и помечаем старый прямоугольник как damage, чтобы present починил
 * пиксели и вне перерисовываемого окна. Вызывать в НАЧАЛЕ wm_render_region, до
 * перерисовки фона/окон. Не использовать после comp_clear (save-under протух). */
void comp_cursor_take_down(void) {
    if (!cursor_in_back) return;
    comp_damage_add(cursor_drawn_x, cursor_drawn_y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
    cursor_unblit_back();
}

/* Движение только курсора (сцена окон НЕ менялась): убираем курсор со старого
 * места, ставим на новое, помечаем оба прямоугольника как damage и выводим. */
void comp_cursor_refresh(void) {
    int  ox  = cursor_drawn_x;
    int  oy  = cursor_drawn_y;
    bool had = cursor_in_back;

    cursor_unblit_back();   /* back buffer снова чистый на старом месте */
    cursor_blit_back();     /* курсор вкомпонован на новом месте */

    if (had) comp_damage_add(ox, oy, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
    comp_damage_add(cursor_drawn_x, cursor_drawn_y, CURSOR_SPRITE_W, CURSOR_SPRITE_H);
    comp_present();
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
