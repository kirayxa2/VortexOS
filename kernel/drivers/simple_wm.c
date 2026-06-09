#include "simple_wm.h"
#include "heap.h"
#include "compositor.h"
#include "pmm.h"
#include "vmm.h"

static wm_window_t windows[MAX_WINDOWS];
static uint32_t next_win_id = 1;
static bool compositor_initialized = false;
static uint64_t last_cursor_update = 0;

/* Окно, которое сейчас получает ввод с клавиатуры (focus). Клавиатурный IRQ
 * (keyboard.c -> wm_handle_key) кладёт нажатые символы в очередь событий этого
 * окна; userspace читает их через wm_get_event (type=4, key). По умолчанию фокус
 * получает последнее созданное окно и окно, по которому кликнули. */
static uint64_t g_focused_win_id = 0;

/* --- Render отвязан от ввода (fix #4) ---
 * IRQ мыши больше НЕ вызывает wm_render_all() напрямую — он только обновляет
 * координаты/перетаскивание и ставит флаг g_needs_redraw. Реальная отрисовка
 * идёт с троттлингом из таймера (PIT, wm_tick_render), максимум ~50 раз/сек,
 * сколько бы пакетов мышь ни сыпала. g_rendering защищает от реентерабельности
 * (таймерный IRQ vs. wm_flush из задачи). */
static volatile int g_needs_redraw = 0;
static volatile int g_rendering = 0;
/* Курсор сдвинулся, но сцена окон НЕ менялась — обновляем только спрайт курсора
 * (save-under), без полного recomposite. См. comp_cursor_refresh / fix #1. */
static volatile int g_cursor_moved = 0;

/* Состояние перетаскивания */
static struct {
    bool dragging;
    uint64_t win_id;
    int32_t offset_x;  // Offset мыши относительно окна
    int32_t offset_y;
    int32_t rendered_x; // Позиция окна, которая сейчас на экране (последний вывод)
    int32_t rendered_y;
} drag_state = {0};

/* НЕ используем статические буферы - выделим через PMM при создании окна */
static uint32_t *window_buffers[MAX_WINDOWS] = {0};

/* Был ли уже хотя бы один полный кадр (фон рабочего стола + все окна) выведен
 * в back buffer. До этого wm_flush обязан сделать полный render, иначе частичная
 * (region) перерисовка оставит остальной экран неинициализированным. */
static bool g_scene_presented = false;

/* Частичная перерисовка прямоугольника (определена ниже). */
static void wm_render_region(int rx, int ry, int rw, int rh);

/* Виртуальный адрес для маппинга окон (в пространстве ядра) */
#define WINDOW_BUFFER_VADDR_BASE 0xFFFFFFFF92000000ULL

void wm_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].id = 0;
    }
    compositor_initialized = false;
    last_cursor_update = 0;
    drag_state.dragging = false;
    drag_state.win_id = 0;
}

static void ensure_compositor_init(void) {
    if (compositor_initialized) return;
    
    extern uint32_t *fb_addr;
    extern uint32_t fb_width, fb_height;
    extern uint64_t fb_pitch;
    
    compositor_init(fb_addr, fb_width, fb_height, fb_pitch);
    compositor_initialized = true;
}

uint64_t wm_create_window(const char *title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pid) {
    // Lazy init compositor
    ensure_compositor_init();
    
    // Find free slot
    wm_window_t *win = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].id == 0) {
            win = &windows[i];
            break;
        }
    }
    if (!win) {
        return 0;
    }
    
    win->id = next_win_id++;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->owner_pid = pid;
    win->visible = 1;
    win->dirty = 1;
    win->event_head = 0;
    win->event_tail = 0;

    /* Новое окно получает фокус ввода с клавиатуры. */
    g_focused_win_id = win->id;
    
    // Copy title from userspace byte by byte
    int i = 0;
    while (i < 63) {
        uint8_t c = ((const uint8_t *)title)[i];
        if (c == 0) break;
        win->title[i] = c;
        i++;
    }
    win->title[i] = 0;
    
    // Используем PMM для выделения буфера окна
    if (w * h > MAX_WINDOW_PIXELS) {
        return 0;
    }
    
    // Найти индекс окна
    int win_index = win - windows;
    
    // Вычислить размер в байтах и количество страниц
    uint64_t buffer_size = (uint64_t)w * h * 4;  // 4 bytes per pixel (ARGB)
    uint64_t num_pages = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Вычислить виртуальный адрес для этого окна
    uint64_t vaddr = WINDOW_BUFFER_VADDR_BASE + (win_index * MAX_WINDOW_PIXELS * 4);
    
    // Выделить физические страницы и замаппить их
    extern pte_t *vmm_kernel_pml4;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc();
        if (phys == 0) {
            // Освободить уже выделенные страницы
            for (uint64_t j = 0; j < i; j++) {
                uint64_t phys_to_free = vmm_virt_to_phys(vmm_kernel_pml4, vaddr + j * PAGE_SIZE);
                if (phys_to_free) {
                    vmm_unmap(vmm_kernel_pml4, vaddr + j * PAGE_SIZE);
                    pmm_free(phys_to_free);
                }
            }
            return 0;
        }
        
        // Маппим в kernel PML4 с флагами PRESENT | WRITABLE (без USER, это kernel memory)
        vmm_map(vmm_kernel_pml4, vaddr + i * PAGE_SIZE, phys, VMM_PRESENT | VMM_WRITABLE);
    }
    
    // Сохранить указатель
    window_buffers[win_index] = (uint32_t *)vaddr;
    win->pixels = window_buffers[win_index];
    
    // Очистить буфер
    for (int j = 0; j < w * h; j++) {
        win->pixels[j] = 0xFF2A2A3E;  // Dark gray
    }
    
    /* Сразу отрисовываем чтобы окно появилось */
    wm_render_all();
    
    return win->id;
}

void wm_destroy_window(uint64_t win_id) {
    extern pte_t *vmm_kernel_pml4;
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].id == win_id) {
            wm_window_t *win = &windows[i];
            
            // Освободить PMM страницы
            if (window_buffers[i] != 0) {
                uint64_t buffer_size = (uint64_t)win->w * win->h * 4;
                uint64_t num_pages = (buffer_size + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t vaddr = (uint64_t)window_buffers[i];
                
                for (uint64_t j = 0; j < num_pages; j++) {
                    uint64_t phys = vmm_virt_to_phys(vmm_kernel_pml4, vaddr + j * PAGE_SIZE);
                    if (phys) {
                        vmm_unmap(vmm_kernel_pml4, vaddr + j * PAGE_SIZE);
                        pmm_free(phys);
                    }
                }
                
                window_buffers[i] = 0;
            }
            
            windows[i].id = 0;
            return;
        }
    }
}

static wm_window_t* find_window(uint64_t win_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].id == win_id) return &windows[i];
    }
    return 0;
}

void wm_draw_rect(uint64_t win_id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    wm_window_t *win = find_window(win_id);
    if (!win || !win->pixels) return;
    
    for (int32_t dy = 0; dy < h; dy++) {
        for (int32_t dx = 0; dx < w; dx++) {
            int32_t px = x + dx;
            int32_t py = y + dy;
            if (px >= 0 && px < win->w && py >= 0 && py < win->h) {
                win->pixels[py * win->w + px] = color;
            }
        }
    }
    win->dirty = 1;
}

void wm_draw_string(uint64_t win_id, int32_t x, int32_t y, const char *str, uint32_t color) {
    wm_window_t *win = find_window(win_id);
    if (!win || !win->pixels) return;
    
    int32_t cur_x = x;
    
    // Copy string from userspace and draw
    for (int idx = 0; idx < 256; idx++) {  // Max 256 chars
        uint8_t c = ((const uint8_t *)str)[idx];
        if (c == 0) break;
        if (c >= 128) c = '?';
        
        /* Используем встроенный шрифт из compositor */
        extern const uint8_t font[128][16];
        const uint8_t *glyph = font[c];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    int32_t px = cur_x + col;
                    int32_t py = y + row;
                    if (px >= 0 && px < win->w && py >= 0 && py < win->h) {
                        win->pixels[py * win->w + px] = color;
                    }
                }
            }
        }
        cur_x += 8;
    }
    win->dirty = 1;
}

void wm_flush(uint64_t win_id) {
    wm_window_t *win = find_window(win_id);
    if (!win) return;
    win->dirty = 1;
    /* Первый кадр обязан быть полным (нужно инициализировать фон рабочего стола
     * и back buffer целиком). Дальше перерисовываем ТОЛЬКО область этого окна
     * (damage-rect): копия во front buffer маленькая => укладывается в окно
     * вертикального гашения (vsync) => почти нет tearing, и это во много раз
     * быстрее полного flip всего экрана на каждом wm_flush (важно для часто
     * перерисовывающихся окон вроде терминала). */
    if (!g_scene_presented) {
        wm_render_all();
        return;
    }
    wm_render_region(win->x, win->y, win->w, win->h + 20);
}

void wm_render_all(void) {
    /* Защита от реентерабельности: если рендер уже идёт (например, таймерный IRQ
     * прилетел посреди wm_flush из задачи) — не входим повторно, а лишь помечаем,
     * что нужна свежая перерисовка. */
    if (g_rendering) { g_needs_redraw = 1; return; }
    g_rendering = 1;

    /* Используем compositor для отрисовки */
    comp_clear(0xFF1A1A2E);  // Dark background
    
    // Render all visible windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        if (win->id == 0 || !win->visible || !win->pixels) continue;
        
        // Draw title bar using compositor
        comp_fill_rect(win->x, win->y, win->w, 20, 0xFF3A3A5E);
        
        // Draw title text
        comp_draw_string(win->x + 5, win->y + 2, win->title, 0xFFE0E0E0, 0xFF3A3A5E);
        
        // Draw window content from offscreen buffer (быстрый блит целыми строками)
        comp_blit_buffer(win->x, win->y + 20, win->w, win->h, win->pixels);
    }

    /* Вкомпоновываем курсор прямо в back buffer (save-under, #1), чтобы полный
     * вывод кадра не стирал его и не мигал. */
    comp_cursor_compose();

    /* Полный кадр: весь экран — damage. comp_present сделает один comp_flip. */
    comp_damage_full();
    comp_present();

    g_scene_presented = true;  /* фон + окна теперь корректны в back buffer */

    g_cursor_moved = 0;  /* полный кадр уже перерисовал курсор */

    /* Полный кадр нарисовал все окна на их текущих местах — синхронизируем
     * «нарисованную» позицию перетаскиваемого окна, чтобы следующий частичный
     * кадр посчитал damage-объединение от правильного старого положения. */
    if (drag_state.dragging) {
        wm_window_t *dw = find_window(drag_state.win_id);
        if (dw) { drag_state.rendered_x = dw->x; drag_state.rendered_y = dw->y; }
    }

    g_rendering = 0;
}

/* -------------------------------------------------------------------------
 * Частичная перерисовка (damage region) — путь «как у настоящих композиторов».
 *
 * KWin / wlroots / Weston НИКОГДА не перерисовывают и не копируют весь экран на
 * каждый кадр. Они накапливают damage (изменившиеся прямоугольники) и трогают
 * ТОЛЬКО их: перерисовывают фон+окна внутри damage-региона и копируют во front
 * buffer только эти пиксели. При перетаскивании окна damage = объединение
 * старого и нового положения окна, а не весь экран.
 *
 * Раньше wm_render_all() на КАЖДЫЙ кадр делал:
 *   comp_clear(весь экран ~width*height) + recomposite + comp_flip(весь экран).
 * При 1080p это ~8 МБ записи в back buffer + ~8 МБ копии в (WC) framebuffer
 * каждый кадр => ~5 FPS, и damage-rect «не давал разницы», т.к. полный кадр
 * всё равно помечал весь экран как damage. Здесь мы перерисовываем и копируем
 * лишь маленький прямоугольник (размер окна + смещение), а не весь экран.
 * ---------------------------------------------------------------------- */
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* Пересекается ли окно (вместе с title bar высотой 20) с прямоугольником? */
static int win_intersects(const wm_window_t *win, int rx, int ry, int rw, int rh) {
    int wx = win->x, wy = win->y, ww = win->w, wh = win->h + 20;
    if (wx >= rx + rw || wx + ww <= rx) return 0;
    if (wy >= ry + rh || wy + wh <= ry) return 0;
    return 1;
}

/* Перерисовывает ТОЛЬКО прямоугольник (rx,ry,rw,rh): чистит фон в нём,
 * перекомпоновывает окна, которые его задевают, вкомпоновывает курсор и
 * копирует во front buffer только этот регион. Остальной back buffer остаётся
 * валидным с прошлого кадра (double buffer персистентный). */
static void wm_render_region(int rx, int ry, int rw, int rh) {
    if (g_rendering) { g_needs_redraw = 1; return; }
    if (rw <= 0 || rh <= 0) return;
    g_rendering = 1;

    /* Фон только в регионе. */
    comp_fill_rect(rx, ry, rw, rh, 0xFF1A1A2E);

    /* Перерисовываем окна, задевающие регион (рисуем целиком — примитивы сами
     * клипуют к экрану; пиксели вне региона в back buffer и так корректны). */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        if (win->id == 0 || !win->visible || !win->pixels) continue;
        if (!win_intersects(win, rx, ry, rw, rh)) continue;
        comp_fill_rect(win->x, win->y, win->w, 20, 0xFF3A3A5E);
        comp_draw_string(win->x + 5, win->y + 2, win->title, 0xFFE0E0E0, 0xFF3A3A5E);
        comp_blit_buffer(win->x, win->y + 20, win->w, win->h, win->pixels);
    }

    /* Курсор поверх (он внутри региона при перетаскивании за title bar). */
    comp_cursor_compose();
    g_cursor_moved = 0;

    /* Во front buffer уходит только этот прямоугольник. */
    comp_damage_reset();
    comp_damage_add(rx, ry, rw, rh);
    comp_present();

    g_rendering = 0;
}

/* Вызывается из таймерного IRQ (PIT) с троттлингом. Рисует только если
 * что-то изменилось и compositor уже инициализирован. */
void wm_tick_render(void) {
    if (!compositor_initialized) return;

    if (g_needs_redraw) {
        g_needs_redraw = 0;
        /* Перетаскивание окна — частичная перерисовка только объединения
         * старого и нового положения окна (как делают настоящие композиторы),
         * вместо полного clear+flip всего экрана. */
        if (drag_state.dragging) {
            wm_window_t *win = find_window(drag_state.win_id);
            if (win) {
                int ox = drag_state.rendered_x, oy = drag_state.rendered_y;
                int nx = win->x,               ny = win->y;
                int fh = win->h + 20;          /* окно + title bar */
                int minx = imin(ox, nx);
                int miny = imin(oy, ny);
                int maxx = imax(ox + win->w, nx + win->w);
                int maxy = imax(oy + fh,     ny + fh);
                /* +2 px запас под чёрный контур курсора / округления. */
                wm_render_region(minx - 1, miny - 1,
                                 (maxx - minx) + 2, (maxy - miny) + 2);
                drag_state.rendered_x = nx;
                drag_state.rendered_y = ny;
                return;
            }
        }
        /* Сцена менялась иначе (создание окна, wm_flush и т.п.) — полный render. */
        wm_render_all();
    } else if (g_cursor_moved) {
        /* Двигался ТОЛЬКО курсор — лёгкий путь без recomposite окон: стираем
         * старое место (фон из back buffer) и рисуем курсор на новом. */
        if (g_rendering) return;  /* полный render идёт в задаче — подождём тик */
        g_cursor_moved = 0;
        comp_cursor_refresh();
    }
}

/* -------------------------------------------------------------------------
 * Доставка клавиатуры в userspace окно (focus).
 *
 * Раньше очередь событий окна (events[]) НИКОГДА не заполнялась — wm_get_event
 * всегда возвращал 0, поэтому userspace-приложения не могли читать клавиатуру.
 * Теперь клавиатурный IRQ (keyboard.c) на каждый символ зовёт wm_handle_key(),
 * и мы кладём событие в очередь окна с фокусом. Формат события совпадает с
 * userspace `wm_event_t` (8 x uint32 = 32 байта):
 *   слово[0]      = type (4 = key)
 *   байт[12]      = mouse_buttons
 *   байт[14..15]  = key_code  (мы кладём ASCII в младший байт)
 *   байт[16]      = key_pressed (1 = нажата)
 * Один производитель (IRQ) и один потребитель (syscall) — блокировки не нужны.
 * ------------------------------------------------------------------------- */
static void wm_enqueue_key(wm_window_t *win, char ascii) {
    uint32_t next = (win->event_tail + 1) % MAX_WM_EVENTS;
    if (next == win->event_head) return;   /* очередь полна — теряем символ */

    uint32_t *slot = &win->events[win->event_tail * 8];
    for (int i = 0; i < 8; i++) slot[i] = 0;
    slot[0] = 4;                           /* type = key */
    uint8_t *b = (uint8_t *)slot;
    b[14] = (uint8_t)ascii;                /* key_code низкий байт = ASCII */
    b[15] = 0;
    b[16] = 1;                             /* key_pressed = 1 */

    win->event_tail = next;                /* публикуем событие последним */
}

/* Зовётся из IRQ клавиатуры (keyboard.c). Кладёт символ в окно с фокусом.
 * Если окон/фокуса нет — тихо игнорируем (символ всё равно попадает в kernel
 * kb-буфер для встроенного шелла). */
void wm_handle_key(char ascii) {
    if (!ascii) return;
    wm_window_t *win = find_window(g_focused_win_id);
    if (!win) return;
    wm_enqueue_key(win, ascii);
}

int wm_get_event(uint64_t win_id, void *event_out) {
    wm_window_t *win = find_window(win_id);
    if (!win) return 0;
    
    if (win->event_head == win->event_tail) {
        // No events
        return 0;
    }
    
    // Copy event
    uint32_t *out = (uint32_t *)event_out;
    for (int i = 0; i < 8; i++) {
        out[i] = win->events[win->event_head * 8 + i];
    }
    
    win->event_head = (win->event_head + 1) % MAX_WM_EVENTS;
    return 1;
}

void wm_handle_mouse_move(int dx, int dy) {
    comp_update_mouse(dx, -dy, compositor_get()->mouse_buttons);
    
    // Если перетаскиваем окно - обновляем его позицию
    if (drag_state.dragging) {
        wm_window_t *win = find_window(drag_state.win_id);
        if (win) {
            compositor_t *comp = compositor_get();
            win->x = comp->mouse_x - drag_state.offset_x;
            win->y = comp->mouse_y - drag_state.offset_y;
            
            // Ограничиваем окно экраном
            if (win->x < 0) win->x = 0;
            if (win->y < 0) win->y = 0;
            if (win->x + win->w > (int)comp->width) win->x = comp->width - win->w;
            if (win->y + win->h + 20 > (int)comp->height) win->y = comp->height - win->h - 20;
        }
    }
    
    /* Render отвязан от ввода: в IRQ только ставим флаг, рисует таймер.
     * Тащим окно — менялась сцена → полный render. Иначе двигается только
     * курсор → лёгкий save-under путь, без recomposite окон (fix #1). */
    if (drag_state.dragging)
        g_needs_redraw = 1;
    else
        g_cursor_moved = 1;
}

void wm_handle_mouse_button(uint8_t buttons) {
    compositor_t *comp = compositor_get();
    comp->mouse_buttons = buttons;
    
    int mouse_x = comp->mouse_x;
    int mouse_y = comp->mouse_y;
    
    // Проверяем клик на title bar окна (идем с конца, чтобы верхние окна были первыми)
    if (buttons & 1) {  // Левая кнопка нажата
        /* Фокус ввода переходит к окну, по которому кликнули (title или тело). */
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            wm_window_t *win = &windows[i];
            if (win->id == 0 || !win->visible) continue;
            if (mouse_x >= win->x && mouse_x < win->x + win->w &&
                mouse_y >= win->y && mouse_y < win->y + win->h + 20) {
                g_focused_win_id = win->id;
                break;
            }
        }
        if (!drag_state.dragging) {
            // Начинаем перетаскивание
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                wm_window_t *win = &windows[i];
                if (win->id == 0 || !win->visible) continue;
                
                // Проверяем попадание в title bar (высота 20 пикселей)
                if (mouse_x >= win->x && mouse_x < win->x + win->w &&
                    mouse_y >= win->y && mouse_y < win->y + 20) {
                    drag_state.dragging = true;
                    drag_state.win_id = win->id;
                    drag_state.offset_x = mouse_x - win->x;
                    drag_state.offset_y = mouse_y - win->y;
                    /* Окно сейчас нарисовано в (win->x, win->y) — отсюда считаем
                     * damage-объединение на первом кадре перетаскивания. */
                    drag_state.rendered_x = win->x;
                    drag_state.rendered_y = win->y;
                    break;
                }
            }
        }
    } else {
        // Кнопка отпущена - заканчиваем перетаскивание
        drag_state.dragging = false;
    }
    
    /* Render отвязан от ввода: в IRQ только ставим флаг, рисует таймер. */
    g_needs_redraw = 1;
}
