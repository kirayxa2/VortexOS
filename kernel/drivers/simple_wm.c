#include "simple_wm.h"
#include "heap.h"
#include "compositor.h"
#include "virtio_gpu.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"   /* sched_current/sched_block_current/sched_wake — block, don't poll */

static wm_window_t windows[MAX_WINDOWS];
static uint32_t next_win_id = 1;

/* -------------------------------------------------------------------------
 *  Оконный chrome (#3) + per-pixel alpha (#2):
 *  заголовок со скруглёнными верхними углами, три кнопки-«светофора»
 *  (закрыть/свернуть/развернуть) и мягкая тень под окном.
 * ---------------------------------------------------------------------- */
#define TITLEBAR_H   20          /* высота заголовка окна (px) */
#define WIN_CORNER   10          /* радиус скругления углов окна (верх и низ) */
#define WIN_SHADOW   10          /* радиус мягкой тени вокруг окна */
#define WIN_SHADOW_OY 4          /* смещение тени вниз (глубина) */
#define WIN_MARGIN   (WIN_SHADOW + WIN_SHADOW_OY)  /* запас для damage/region */
#define BTN_R        5           /* радиус кнопки-светофора */
#define BTN_GAP      16          /* шаг между кнопками */
#define BTN_X0       14          /* центр первой кнопки от левого края окна */

/* --- Изменение размера окна (resize как в Windows) --- */
#define RESIZE_BORDER 6          /* ширина зоны захвата у края окна (px) */
#define MIN_WIN_W    140         /* минимальная ширина содержимого окна */
#define MIN_WIN_H    80          /* минимальная высота содержимого окна */
#define RZ_LEFT      1
#define RZ_RIGHT     2
#define RZ_TOP       4
#define RZ_BOTTOM    8
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

/* Атомарный «захват» рендера. Раньше рендер шёл в IRQ0 (прерывания off — захват
 * был атомарен сам собой). Теперь рендер вынесен в задачу (fix #1) и может быть
 * вытеснен планировщиком, поэтому простой `if (g_rendering) ...; g_rendering=1`
 * стал гонкой: две задачи (задача рендера и wm_flush из userspace) могли пройти
 * проверку до того, как любая выставит флаг, и зайти в композитор одновременно.
 * Делаем проверку-и-установку атомарной, кратко погасив прерывания (сам тяжёлый
 * рендер по-прежнему идёт с ВКЛЮЧЁННЫМИ прерываниями — гасим лишь пару инструкций
 * захвата). Возвращает 1, если рендер захвачен нами; 0 — если уже идёт. */
static inline int render_try_enter(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    int busy = g_rendering;
    if (!busy) g_rendering = 1;
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
    return !busy;
}
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

/* Состояние изменения размера (тянем за край/угол окна, как в Windows). */
static struct {
    bool active;
    uint64_t win_id;
    int edge;            // битовая маска RZ_LEFT|RZ_RIGHT|RZ_TOP|RZ_BOTTOM
    int32_t start_mx, start_my;          // позиция мыши в начале
    int32_t start_x, start_y, start_w, start_h; // геометрия окна в начале
    int32_t rendered_x, rendered_y, rendered_w, rendered_h; // что сейчас на экране
} resize_state = {0};

/* НЕ используем статические буферы - выделим через PMM при создании окна */
static uint32_t *window_buffers[MAX_WINDOWS] = {0};

/* Был ли уже хотя бы один полный кадр (фон рабочего стола + все окна) выведен
 * в back buffer. До этого wm_flush обязан сделать полный render, иначе частичная
 * (region) перерисовка оставит остальной экран неинициализированным. */
static bool g_scene_presented = false;

/* Частичная перерисовка прямоугольника (определена ниже). */
static void wm_render_region(int rx, int ry, int rw, int rh);

/* =========================================================================
 *  Dock — панель в стиле macOS (скруглённая «пилюля» по центру снизу).
 *
 *  Рисуется поверх рабочего стола и окон (как плавающий dock в macOS), прямо в
 *  back buffer композитора (полупрозрачное «стекло» через альфа-смешивание с
 *  тем, что под ней). Пока содержит одну иконку — терминал (/vsh).
 *  Клик по иконке просит ядро запустить терминал (если ни одного окна нет —
 *  см. wm_dock_consume_launch / dock_launcher_task в kmain.c).
 * ===================================================================== */
#define DOCK_ICON     48          /* размер иконки (px) */
#define DOCK_PAD      12          /* отступ внутри пилюли вокруг иконок */
#define DOCK_GAP      12          /* зазор между иконками */
#define DOCK_BOTTOM   16          /* отступ пилюли от низа экрана */
#define DOCK_NITEMS   1           /* пока только терминал */

static volatile int g_dock_launch_request = 0; /* mouse IRQ ставит, kmain-задача забирает */
static task_t *g_dock_task = 0;                /* задача дока — её будим по клику */

/* Регистрируется из kmain после task_create("dock", ...). Нужна, чтобы клик по
 * иконке дока мог разбудить заснувшую (block, don't poll) задачу-лаунчер. */
void wm_set_dock_task(task_t *t) { g_dock_task = t; }
static int g_dock_hover   = -1;   /* индекс иконки под курсором, -1 если нет */
static int g_dock_pressed = 0;    /* нажата ли иконка под курсором */
static volatile int g_dock_dirty = 0; /* нужно перерисовать только область дока */
static volatile int g_panel_dirty = 0; /* нужно перерисовать верхнюю панель (часы) */

static void wm_draw_dock(void);

/* Геометрия пилюли (зависит от разрешения экрана). */
static void dock_geometry(int *dx, int *dy, int *dw, int *dh) {
    extern uint32_t fb_width, fb_height;
    int h = DOCK_ICON + DOCK_PAD * 2;
    int w = DOCK_NITEMS * DOCK_ICON + (DOCK_NITEMS - 1) * DOCK_GAP + DOCK_PAD * 2;
    *dx = ((int)fb_width - w) / 2;
    *dy = (int)fb_height - h - DOCK_BOTTOM;
    *dw = w;
    *dh = h;
}

static void dock_icon_rect(int idx, int *ix, int *iy) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    (void)dw; (void)dh;
    *ix = dx + DOCK_PAD + idx * (DOCK_ICON + DOCK_GAP);
    *iy = dy + DOCK_PAD;
}

/* hit-test: >=0 — индекс иконки, -2 — на пилюле мимо иконок, -1 — мимо дока. */
static int dock_hit(int mx, int my) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    if (mx < dx || mx >= dx + dw || my < dy || my >= dy + dh) return -1;
    for (int k = 0; k < DOCK_NITEMS; k++) {
        int ix, iy;
        dock_icon_rect(k, &ix, &iy);
        if (mx >= ix && mx < ix + DOCK_ICON && my >= iy && my < iy + DOCK_ICON)
            return k;
    }
    return -2;
}

/* Альфа-смешивание src поверх того, что уже в back buffer (a: 0..255). */
static inline uint32_t dock_blend(uint32_t bg, uint32_t fg, int a) {
    int br = (bg >> 16) & 0xFF, bg2 = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int fr = (fg >> 16) & 0xFF, fg2 = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    int r = (fr * a + br * (255 - a)) / 255;
    int g = (fg2 * a + bg2 * (255 - a)) / 255;
    int b = (fb * a + bb * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void dock_put_blend(int x, int y, uint32_t color, int a) {
    if (a >= 255) { comp_put_pixel(x, y, color); return; }
    comp_put_pixel(x, y, dock_blend(comp_get_pixel(x, y), color, a));
}

/* Заливка скруглённого прямоугольника с альфой (углы радиусом r). */
static void dock_fill_round(int x, int y, int w, int h, int r, uint32_t color, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int cx = -1, cy = -1;
            if (i < r && j < r)              { cx = r;         cy = r;         }
            else if (i >= w - r && j < r)    { cx = w - r - 1; cy = r;         }
            else if (i < r && j >= h - r)    { cx = r;         cy = h - r - 1; }
            else if (i >= w - r && j >= h - r){ cx = w - r - 1; cy = h - r - 1; }
            if (cx >= 0) {
                int ddx = i - cx, ddy = j - cy;
                if (ddx * ddx + ddy * ddy > r * r) continue;  /* за пределами скругления */
            }
            dock_put_blend(x + i, y + j, color, a);
        }
    }
}

/* Иконка терминала: тёмная скруглённая плитка, три «светофора» и зелёный
 * промпт «>» с курсор-блоком. pressed — слегка утоплена. */
static void dock_draw_terminal(int x, int y, int s, int pressed) {
    if (pressed) { x += 1; y += 1; }
    /* плитка */
    dock_fill_round(x, y, s, s, 11, 0xFF16161F, 255);
    /* лёгкий верхний блик */
    for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);
    /* три «светофора» сверху */
    int cy = y + 9;
    comp_fill_circle(x + 11, cy, 3, 0xFFFF5F56);
    comp_fill_circle(x + 21, cy, 3, 0xFFFFBD2E);
    comp_fill_circle(x + 31, cy, 3, 0xFF27C93F);
    /* зелёный промпт «>» из двух линий потолще + курсор-блок */
    uint32_t green = 0xFF3BE06F;
    int px = x + 10, py = y + 23;
    for (int t = 0; t < 2; t++) {
        comp_draw_line(px,     py + t,     px + 7, py + 6 + t, green);
        comp_draw_line(px + 7, py + 6 + t, px,     py + 12 + t, green);
    }
    comp_fill_rect(x + 23, py + 9, 12, 4, green);
}

/* Рисует весь dock в back buffer (поверх уже скомпонованной сцены). */
static void wm_draw_dock(void) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    int r = dh / 2;
    /* мягкая тень */
    dock_fill_round(dx - 2, dy + 5, dw + 4, dh, r, 0xFF000000, 45);
    /* «стеклянная» пилюля (полупрозрачная тёмная) */
    dock_fill_round(dx, dy, dw, dh, r, 0xFF22222F, 205);
    /* верхняя световая кромка */
    for (int i = r; i < dw - r; i++) dock_put_blend(dx + i, dy + 1, 0xFFFFFFFF, 30);
    /* иконки */
    for (int k = 0; k < DOCK_NITEMS; k++) {
        int ix, iy;
        dock_icon_rect(k, &ix, &iy);
        int hovered = (g_dock_hover == k);
        int pressed = (g_dock_pressed && g_dock_hover == k);
        if (hovered)
            dock_fill_round(ix - 4, iy - 4, DOCK_ICON + 8, DOCK_ICON + 8, 14,
                            0xFFFFFFFF, pressed ? 60 : 35);
        dock_draw_terminal(ix, iy, DOCK_ICON, pressed);
    }
}

/* =========================================================================
 * Верхняя панель (Hyprland-style top bar)
 *  Полоса вверху экрана: слева — логотип + заголовок активного окна,
 *  справа — часы HH:MM:SS. Рисуется в back buffer поверх сцены и окон.
 * ======================================================================= */
#define PANEL_H   24                /* высота верхней панели (px) */
#define PANEL_BG  0xFF15151Fu       /* фон панели (непрозрачный) */

/* QEMU по умолчанию хранит RTC в UTC. Если часы показывают не местное время —
 * поправь это смещение (Москва = UTC+3). Если QEMU настроен на localtime,
 * оставь 0. */
#define RTC_TZ_OFFSET_HOURS  0

static inline void rtc_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t rtc_inb(uint16_t port) {
    uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static uint8_t cmos_read(uint8_t reg) {
    rtc_outb(0x70, (uint8_t)(0x80 | reg));  /* bit7=1: NMI off на время чтения */
    return rtc_inb(0x71);
}
static int cmos_update_in_progress(void) {
    rtc_outb(0x70, 0x8A);
    return rtc_inb(0x71) & 0x80;
}
/* Возвращает часы/минуты/секунды (24ч), с учётом BCD/12ч и смещения TZ. */
static void rtc_read_hms(int *hh, int *mm, int *ss) {
    /* Дождаться, пока RTC не в процессе обновления (UIP flag). */
    int guard = 100000;
    while (cmos_update_in_progress() && --guard) { }
    uint8_t s    = cmos_read(0x00);
    uint8_t mi   = cmos_read(0x02);
    uint8_t h    = cmos_read(0x04);
    uint8_t regB = cmos_read(0x0B);
    int second, minute, hour;
    if (!(regB & 0x04)) {                 /* BCD режим — конвертируем */
        second = (s  & 0x0F) + ((s  >> 4) * 10);
        minute = (mi & 0x0F) + ((mi >> 4) * 10);
        hour   = (h  & 0x0F) + (((h & 0x70) >> 4) * 10);
        if (h & 0x80) hour = (hour + 12) % 24;   /* 12ч формат с PM-битом */
    } else {                              /* бинарный режим */
        second = s; minute = mi; hour = h & 0x7F;
        if (h & 0x80) hour = (hour + 12) % 24;
    }
    hour = (hour + RTC_TZ_OFFSET_HOURS) % 24;
    if (hour < 0) hour += 24;
    *hh = hour; *mm = minute; *ss = second;
}

static wm_window_t* find_window(uint64_t win_id);  /* forward */

/* Прямоугольник, охватывающий панель (для damage/региона). */
static void panel_bounds(int *x, int *y, int *w, int *h) {
    extern uint32_t fb_width;
    *x = 0; *y = 0; *w = (int)fb_width; *h = PANEL_H;
}

/* Рисует верхнюю панель в back buffer. */
static void wm_draw_panel(void) {
    extern uint32_t fb_width;
    int W = (int)fb_width;

    comp_fill_rect(0, 0, W, PANEL_H, PANEL_BG);
    /* нижняя акцентная линия (синяя — фирменная граница VortexOS) */
    comp_fill_rect(0, PANEL_H - 1, W, 1, 0xFF007ACC);

    int ty = (PANEL_H - 16) / 2;   /* шрифт 8x16 → вертикальное центрирование */

    /* слева: логотип-квадрат + "VortexOS" */
    comp_fill_rect(8, PANEL_H / 2 - 4, 8, 8, 0xFF007ACC);
    int lx = 24;
    comp_draw_string(lx, ty, "VortexOS", 0xFFE0E0E0, PANEL_BG);
    lx += 8 * 8;   /* "VortexOS" = 8 символов * 8px */

    /* заголовок активного окна после разделителя */
    wm_window_t *fw = find_window(g_focused_win_id);
    if (fw && fw->title[0]) {
        comp_fill_rect(lx + 8, ty + 1, 1, 14, 0xFF44445A);  /* разделитель */
        comp_draw_string(lx + 16, ty, fw->title, 0xFF9AB8D8, PANEL_BG);
    }

    /* справа: часы HH:MM:SS */
    int hh, mm, ss;
    rtc_read_hms(&hh, &mm, &ss);
    char clk[9];
    clk[0] = '0' + hh / 10; clk[1] = '0' + hh % 10; clk[2] = ':';
    clk[3] = '0' + mm / 10; clk[4] = '0' + mm % 10; clk[5] = ':';
    clk[6] = '0' + ss / 10; clk[7] = '0' + ss % 10; clk[8] = 0;
    int cx = W - 8 * 8 - 10;   /* 8 символов * 8px + правый отступ */
    comp_draw_string(cx, ty, clk, 0xFFFFFFFF, PANEL_BG);
}

/* Прямоугольник, охватывающий весь dock + тень (для damage/региона). */
static void dock_bounds(int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    *x = dx - 4; *y = dy - 4; *w = dw + 8; *h = dh + 12;
}

/* Сколько окон сейчас активно (для решения, запускать ли терминал). */
int wm_active_window_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].id != 0) n++;
    return n;
}

/* Забрать запрос на запуск терминала (зовётся из kmain dock_launcher_task). */
int wm_dock_consume_launch(void) {
    if (g_dock_launch_request) { g_dock_launch_request = 0; return 1; }
    return 0;
}

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
    resize_state.active = false;
    resize_state.win_id = 0;
}

static void ensure_compositor_init(void) {
    if (compositor_initialized) return;
    
    extern uint32_t *fb_addr;
    extern uint32_t fb_width, fb_height;
    extern uint64_t fb_pitch;

    /* Если поднялся virtio-gpu — рисуем в его backing (его и презентит GPU),
     * а не в линейный Limine-scanout. Иначе fallback на Limine framebuffer. */
    if (virtio_gpu_active()) {
        compositor_init(virtio_gpu_framebuffer(), virtio_gpu_width(),
                        virtio_gpu_height(), virtio_gpu_pitch());
    } else {
        compositor_init(fb_addr, fb_width, fb_height, fb_pitch);
    }
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
    win->waiter     = 0;

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
    
    // Вычислить размер в байтах и количество страниц.
    // ВАЖНО: маппим сразу под МАКСИМАЛЬНЫЙ размер окна (MAX_WINDOW_PIXELS), а не
    // под начальный w*h. Иначе resize окна больше начального размера полез бы в
    // НЕзамапленные страницы → #PF. Память резервируется в IRQ-безопасный момент
    // (syscall создания окна), поэтому в обработчике мыши при resize аллокации нет.
    uint64_t buffer_size = (uint64_t)MAX_WINDOW_PIXELS * 4;  // 4 bytes per pixel (ARGB)
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
            
            // Освободить PMM страницы. Маппили под MAX_WINDOW_PIXELS (см.
            // wm_create_window), поэтому и освобождаем под максимум, иначе при
            // resized-окне (w*h < max) часть страниц утекла бы.
            if (window_buffers[i] != 0) {
                uint64_t buffer_size = (uint64_t)MAX_WINDOW_PIXELS * 4;
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
    /* Регион включает заголовок + запас под тень окна. */
    int m = WIN_MARGIN;
    wm_render_region(win->x - m, win->y - m,
                     win->w + 2 * m, win->h + TITLEBAR_H + 2 * m);
}

/* Целочисленный квадратный корень (для плавного спада тени). */
static int isqrt32(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* Мягкая тень под окном: чёрный с альфой, спадающей к краям (per-pixel alpha).
 * (wx,wy,ww,wh) — полный прямоугольник окна вместе с заголовком. Пиксели под
 * самим окном пропускаем — они будут непрозрачными. */
static void win_draw_shadow(int wx, int wy, int ww, int wh) {
    const int S = WIN_SHADOW, oy = WIN_SHADOW_OY;
    int x0 = wx - S, y0 = wy - S + oy;
    int x1 = wx + ww + S, y1 = wy + wh + S + oy;
    const int r = WIN_CORNER;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            /* Пропускаем только пиксели, РЕАЛЬНО закрытые окном — с учётом
             * скруглённых углов. Вырезы углов (внутри прямоугольника, но за
             * дугой скругления) НЕ пропускаем: туда тоже должна падать тень.
             * Иначе в углу за скруглением остаётся голый фон десктопа без
             * тени — он светлее окна и читается как «лишний светлый квадрат». */
            if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
                int lx = x - wx, ly = y - wy, cx = -1, cy = -1;
                if      (lx < r      && ly < r)      { cx = r;      cy = r;      }
                else if (lx >= ww - r && ly < r)     { cx = ww - r; cy = r;      }
                else if (lx < r      && ly >= wh - r){ cx = r;      cy = wh - r; }
                else if (lx >= ww - r && ly >= wh - r){ cx = ww - r; cy = wh - r; }
                if (cx < 0) continue;  /* не в углу — закрыто окном */
                int ddx = lx - cx, ddy = ly - cy;
                if (ddx * ddx + ddy * ddy <= r * r) continue;  /* внутри дуги — закрыто */
                /* иначе: вырез скруглённого угла — даём тени упасть сюда */
            }
            int dx = 0, dy = 0;
            if (x < wx)            dx = wx - x;
            else if (x >= wx + ww) dx = x - (wx + ww) + 1;
            if (y < wy)            dy = wy - y;
            else if (y >= wy + wh) dy = y - (wy + wh) + 1;
            int d2 = dx * dx + dy * dy;
            if (d2 >= S * S) continue;
            int a = 78 * (S - isqrt32(d2)) / S;   /* спад к краям */
            if (a <= 0) continue;
            comp_blend_pixel(x, y, (uint32_t)a << 24);  /* чёрная тень */
        }
    }
}

/* Доля пикселя [px,px+1)x[py,py+1), попавшая внутрь окружности радиуса r с
 * центром в узле сетки (cx,cy). Суперсэмплинг 4x4 (16 подвыборок) даёт мягкий
 * край (anti-aliasing) без float/sqrt — всё в целочисленной арифметике (×8).
 * Возвращает покрытие 0..255 (0 — пиксель снаружи, 255 — полностью внутри). */
static int circ_cov(int px, int py, int cx, int cy, int r) {
    int r2x64 = r * r * 64;          /* (r*8)^2 */
    int in = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int fx = (px - cx) * 8 + sx * 2 + 1;   /* центр под-пикселя ×8 */
            int fy = (py - cy) * 8 + sy * 2 + 1;
            if (fx * fx + fy * fy <= r2x64) in++;
        }
    }
    return in * 255 / 16;
}

/* Заливка прямоугольника со скруглёнными ВЕРХНИМИ углами + сглаживанием.
 * Внутри — сплошная заливка, на дуге углов — альфа-смешивание с тем, что под
 * окном (тень/фон), отчего край выглядит гладким, а не «лесенкой». */
static void fill_round_top(int x, int y, int w, int h, int r, uint32_t color) {
    if (r * 2 > w) r = w / 2;
    if (r > h) r = h;
    uint32_t rgb = color & 0x00FFFFFF;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (j < r && (i < r || i >= w - r)) {
                int cx  = (i < r) ? r : (w - r);   /* узел сетки центра дуги */
                int cov = circ_cov(i, j, cx, r, r);
                if (cov <= 0) continue;                 /* снаружи дуги */
                if (cov >= 255) comp_put_pixel(x + i, y + j, color);
                else comp_blend_pixel(x + i, y + j, ((uint32_t)cov << 24) | rgb);
            } else {
                comp_put_pixel(x + i, y + j, color);
            }
        }
    }
}

/* Скругление НИЖНИХ углов окна со сглаживанием. Вызывается ПОСЛЕ блита
 * контента: на дуге угла смешиваем сохранённый ДО блита фон (bgL/bgR — два
 * массива r×r) обратно поверх контента с альфой (255−покрытие). Так контент
 * «съедается» к краям ровно по дуге, давая гладкий скруглённый угол. */
static void round_bottom_aa(int x, int y, int w, int fh, int r,
                            const uint32_t *bgL, const uint32_t *bgR) {
    for (int j = 0; j < r; j++) {
        int py = (fh - r) + j;                 /* строки от fh-r до fh-1 */
        for (int i = 0; i < r; i++) {
            int covL = circ_cov(i, py, r, fh - r, r);
            if (covL < 255) {
                uint32_t a = (uint32_t)(255 - covL);
                comp_blend_pixel(x + i, y + py, (a << 24) | (bgL[j * r + i] & 0x00FFFFFF));
            }
            int ii = w - r + i;
            int covR = circ_cov(ii, py, w - r, fh - r, r);
            if (covR < 255) {
                uint32_t a = (uint32_t)(255 - covR);
                comp_blend_pixel(x + ii, y + py, (a << 24) | (bgR[j * r + i] & 0x00FFFFFF));
            }
        }
    }
}

/* hit-test кнопок заголовка: 0=закрыть, 1=свернуть, 2=развернуть, -1=мимо. */
static int win_button_hit(const wm_window_t *win, int mx, int my) {
    int cy = win->y + TITLEBAR_H / 2;
    for (int k = 0; k < 3; k++) {
        int cx = win->x + BTN_X0 + k * BTN_GAP;
        int dx = mx - cx, dy = my - cy;
        if (dx * dx + dy * dy <= (BTN_R + 2) * (BTN_R + 2)) return k;
    }
    return -1;
}

/* Возвращает маску краёв (RZ_*), за которые можно тянуть resize, если курсор
 * в зоне захвата у границы окна (учитывая title bar). 0 — не у края.
 * Внешний прямоугольник окна: (x, y, w, h + TITLEBAR_H). Зона захвата шириной
 * RESIZE_BORDER расположена симметрично по обе стороны границы. */
static int resize_edge_hit(const wm_window_t *win, int mx, int my) {
    int ox = win->x, oy = win->y;
    int ow = win->w, oh = win->h + TITLEBAR_H;
    int rb = RESIZE_BORDER;
    /* Курсор должен быть в пределах окна, расширенного на зону захвата. */
    if (mx < ox - rb || mx > ox + ow + rb) return 0;
    if (my < oy - rb || my > oy + oh + rb) return 0;
    int edge = 0;
    if (mx >= ox - rb && mx <= ox + rb)            edge |= RZ_LEFT;
    if (mx >= ox + ow - rb && mx <= ox + ow + rb)  edge |= RZ_RIGHT;
    if (my >= oy - rb && my <= oy + rb)            edge |= RZ_TOP;
    if (my >= oy + oh - rb && my <= oy + oh + rb)  edge |= RZ_BOTTOM;
    return edge;
}

/* Кладёт в очередь окна событие изменения размера, чтобы resize-aware
 * приложение перерисовало содержимое под новый размер.
 * Формат (как wm_event_t, 8 x uint32):
 *   слово[0] = type (5 = resize)
 *   слово[1] = новая ширина содержимого
 *   слово[2] = новая высота содержимого */
static void wm_enqueue_resize(wm_window_t *win) {
    uint32_t next = (win->event_tail + 1) % MAX_WM_EVENTS;
    if (next == win->event_head) return;   /* очередь полна — пропускаем */
    uint32_t *slot = &win->events[win->event_tail * 8];
    for (int i = 0; i < 8; i++) slot[i] = 0;
    slot[0] = 5;                  /* type = resize */
    slot[1] = (uint32_t)win->w;
    slot[2] = (uint32_t)win->h;
    win->event_tail = next;
    if (win->waiter) { sched_wake((task_t *)win->waiter); win->waiter = 0; }
}

/* AA-рамка со скруглёнными углами: прямые стороны + 4 дуги-обводки по той же
 * дуге (радиус r), что и заливка углов. Нужна, потому что bg окна по цвету
 * почти сливается с тенью — без обводки скругление НЕ ВИДНО и угол кажется
 * прямым. Кольцо обводки = покрытие(r) − покрытие(r−1): тонкая ~1px линия,
 * чтобы дуга совпадала по толщине с прямыми сторонами рамки. */
static void draw_round_border(int x, int y, int w, int fh, int r, uint32_t color) {
    uint32_t rgb = color & 0x00FFFFFF;
    for (int i = r; i < w - r; i++) {
        comp_put_pixel(x + i, y,          color);   /* верх */
        comp_put_pixel(x + i, y + fh - 1, color);   /* низ  */
    }
    for (int j = r; j < fh - r; j++) {
        comp_put_pixel(x,         y + j, color);     /* лево  */
        comp_put_pixel(x + w - 1, y + j, color);     /* право */
    }
    int cxs[4] = { r, w - r, r,      w - r  };
    int cys[4] = { r, r,     fh - r, fh - r };
    int bxs[4] = { 0, w - r, 0,      w - r  };
    int bys[4] = { 0, 0,     fh - r, fh - r };
    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < r; j++) {
            for (int i = 0; i < r; i++) {
                int px = bxs[k] + i, py = bys[k] + j;
                int ring = circ_cov(px, py, cxs[k], cys[k], r)
                         - circ_cov(px, py, cxs[k], cys[k], r - 1);
                if (ring <= 0) continue;
                if (ring >= 255) comp_put_pixel(x + px, y + py, color);
                else comp_blend_pixel(x + px, y + py, ((uint32_t)ring << 24) | rgb);
            }
        }
    }
}

/* Рисует chrome окна: тень, скруглённый заголовок, три кнопки-светофора,
 * текст заголовка, контент и тонкую рамку. focused — окно в фокусе. */
static void wm_draw_window_chrome(wm_window_t *win) {
    int focused = (win->id == g_focused_win_id);
    int fh = win->h + TITLEBAR_H;

    /* мягкая тень под окном (per-pixel alpha) */
    win_draw_shadow(win->x, win->y, win->w, fh);

    /* заголовок со скруглёнными верхними углами */
    uint32_t tcol = focused ? 0xFF3A3A5E : 0xFF2C2C3A;
    fill_round_top(win->x, win->y, win->w, TITLEBAR_H, WIN_CORNER, tcol);
    /* верхняя световая кромка */
    for (int i = WIN_CORNER; i < win->w - WIN_CORNER; i++)
        comp_blend_pixel(win->x + i, win->y, 0x28FFFFFFu);

    /* три кнопки-светофора (приглушены, если окно не в фокусе) */
    uint32_t cclose = focused ? 0xFFFF5F56 : 0xFF6E6E78;
    uint32_t cmin   = focused ? 0xFFFFBD2E : 0xFF6E6E78;
    uint32_t cmax   = focused ? 0xFF27C93F : 0xFF6E6E78;
    int cy = win->y + TITLEBAR_H / 2;
    comp_fill_circle(win->x + BTN_X0,               cy, BTN_R, cclose);
    comp_fill_circle(win->x + BTN_X0 + BTN_GAP,     cy, BTN_R, cmin);
    comp_fill_circle(win->x + BTN_X0 + 2 * BTN_GAP, cy, BTN_R, cmax);

    /* текст заголовка после кнопок */
    int tx = win->x + BTN_X0 + 2 * BTN_GAP + BTN_R + 10;
    comp_draw_string(tx, win->y + 2, win->title, 0xFFE0E0E0, tcol);

    /* Сохраняем фон ДО блита под двумя нижними углами (r×r каждый), чтобы потом
     * скруглить их со сглаживанием (смешать фон обратно по дуге). */
    const int rb = WIN_CORNER;
    uint32_t bgL[WIN_CORNER * WIN_CORNER], bgR[WIN_CORNER * WIN_CORNER];
    if (rb * 2 <= win->w && rb <= fh) {
        for (int j = 0; j < rb; j++) {
            int py = (fh - rb) + j;
            for (int i = 0; i < rb; i++) {
                bgL[j * rb + i] = comp_get_pixel(win->x + i,                 win->y + py);
                bgR[j * rb + i] = comp_get_pixel(win->x + win->w - rb + i,   win->y + py);
            }
        }
    }

    /* контент окна (быстрый блит целыми строками) */
    comp_blit_buffer(win->x, win->y + TITLEBAR_H, win->w, win->h, win->pixels);

    /* скругляем нижние углы (со сглаживанием) поверх блита */
    if (rb * 2 <= win->w && rb <= fh)
        round_bottom_aa(win->x, win->y, win->w, fh, rb, bgL, bgR);

    /* AA-рамка, обводящая скруглённые углы (дуги + прямые стороны). Делает
     * скругление видимым даже когда bg окна сливается с тенью по цвету. */
    uint32_t bord = focused ? 0xFF4A4A72 : 0xFF34343E;
    draw_round_border(win->x, win->y, win->w, fh, WIN_CORNER, bord);
}

void wm_render_all(void) {
    /* Защита от реентерабельности: если рендер уже идёт (например, таймерный IRQ
     * прилетел посреди wm_flush из задачи) — не входим повторно, а лишь помечаем,
     * что нужна свежая перерисовка. */
    if (!render_try_enter()) { g_needs_redraw = 1; return; }

    /* Используем compositor для отрисовки */
    comp_clear(0xFF1A1A2E);  // Dark background
    
    // Render all visible windows
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        if (win->id == 0 || !win->visible || !win->pixels) continue;
        wm_draw_window_chrome(win);
    }

    /* Верхняя панель (часы + активное окно) поверх окон. */
    wm_draw_panel();

    /* Dock поверх окон (плавающая панель, как в macOS). */
    wm_draw_dock();

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
    /* Учитываем тень: расширяем footprint окна на WIN_MARGIN со всех сторон,
     * иначе тень окна, задетого регионом краем, не перерисуется и «смажется». */
    int m = WIN_MARGIN;
    int wx = win->x - m, wy = win->y - m;
    int ww = win->w + 2 * m, wh = win->h + TITLEBAR_H + 2 * m;
    if (wx >= rx + rw || wx + ww <= rx) return 0;
    if (wy >= ry + rh || wy + wh <= ry) return 0;
    return 1;
}

/* Перерисовывает ТОЛЬКО прямоугольник (rx,ry,rw,rh): чистит фон в нём,
 * перекомпоновывает окна, которые его задевают, вкомпоновывает курсор и
 * копирует во front buffer только этот регион. Остальной back buffer остаётся
 * валидным с прошлого кадра (double buffer персистентный). */
static void wm_render_region(int rx, int ry, int rw, int rh) {
    if (rw <= 0 || rh <= 0) return;
    if (!render_try_enter()) { g_needs_redraw = 1; return; }

    /* Накапливаем damage этого кадра с нуля. take_down/compose добавят сюда
     * прямоугольники старого и нового положения курсора. */
    comp_damage_reset();

    /* Снимаем софт-курсор из back buffer ДО перекомпоновки: иначе старый спрайт
     * (если он вне региона) попадёт в save-under следующего comp_cursor_compose
     * и «штампанётся» белыми следами при движении мыши (эффект «стёрки»).
     * take_down также добавит старый прямоугольник курсора в damage. */
    comp_cursor_take_down();

    /* Фон только в регионе. */
    comp_fill_rect(rx, ry, rw, rh, 0xFF1A1A2E);

    /* Перерисовываем окна, задевающие регион (рисуем целиком — примитивы сами
     * клипуют к экрану; пиксели вне региона в back buffer и так корректны). */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        if (win->id == 0 || !win->visible || !win->pixels) continue;
        if (!win_intersects(win, rx, ry, rw, rh)) continue;
        wm_draw_window_chrome(win);
    }

    /* Если регион задевает dock — перерисовываем dock (он поверх окон) и
     * помечаем его полный прямоугольник как damage, чтобы свежие пиксели
     * (вкл. hover/press) скопировались во front buffer. */
    {
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        if (!(rx >= bx + bw || rx + rw <= bx || ry >= by + bh || ry + rh <= by)) {
            wm_draw_dock();
            comp_damage_add(bx, by, bw, bh);
        }
    }

    /* Если регион задевает верхнюю панель — перерисовываем её (она поверх окон)
     * и помечаем её прямоугольник как damage. */
    {
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        if (!(rx >= px + pw || rx + rw <= px || ry >= py + ph || ry + rh <= py)) {
            wm_draw_panel();
            comp_damage_add(px, py, pw, ph);
        }
    }

    /* Курсор поверх (он внутри региона при перетаскивании за title bar).
     * compose добавит прямоугольник нового положения курсора в damage. */
    comp_cursor_compose();
    g_cursor_moved = 0;

    /* Во front buffer уходит этот прямоугольник + прямоугольники курсора
     * (старый из take_down, новый из compose). */
    comp_damage_add(rx, ry, rw, rh);
    comp_present();

    g_rendering = 0;
}

/* -------------------------------------------------------------------------
 * Задача рендера (fix #1) — рендер вынесен из прерывания PIT в отдельную
 * kernel-задачу.
 *
 * Раньше wm_tick_render() крутился прямо внутри обработчика IRQ0 (PIT): кадр
 * рисовался с ЗАПРЕЩЁННЫМИ прерываниями, что стопорило планировщик и остальные
 * IRQ (мышь/клавиатура/диск) на всё время отрисовки. Теперь, когда есть
 * полноценная многозадачность, PIT лишь выставляет флаг (render_request), а эта
 * задача забирает его (pit_consume_render_request) и рисует кадр с ВКЛЮЧЁННЫМИ
 * прерываниями. Между кадрами задача спит на hlt до следующего прерывания.
 *
 * Троттлинг (~50 FPS) задаёт PIT, выставляя флаг раз в 2 тика, поэтому даже если
 * задача успевает забрать флаг быстрее — лишних кадров не будет, она просто
 * уснёт до следующего запроса. g_rendering по-прежнему защищает от
 * реентерабельности с wm_flush из userspace-задач. */
void wm_render_task(void) {
    extern int pit_consume_render_request(void);
    for (;;) {
        if (pit_consume_render_request()) {
            wm_tick_render();
        }
        /* Спим до следующего прерывания (PIT/мышь/клавиатура) и проверяем снова. */
        __asm__ volatile("hlt");
    }
}

/* Вызывается из задачи рендера (wm_render_task) с троттлингом от PIT. Рисует
 * только если что-то изменилось и compositor уже инициализирован. */
void wm_tick_render(void) {
    if (!compositor_initialized) return;

    /* Тикаем часы на панели ~раз в секунду (PIT = 100 Hz). */
    {
        extern uint64_t pit_ticks(void);
        static uint64_t last_panel_sec = 0;
        uint64_t sec = pit_ticks() / 100;
        if (sec != last_panel_sec) { last_panel_sec = sec; g_panel_dirty = 1; }
    }

    if (g_needs_redraw) {
        g_needs_redraw = 0;
        g_panel_dirty = 0; /* полный рендер всё равно перерисует панель */
        g_dock_dirty = 0;  /* полный/drag-рендер всё равно перерисует dock */
        /* Изменение размера — частичная перерисовка объединения старого и
         * нового прямоугольника окна (позиция И размер могли поменяться). */
        if (resize_state.active) {
            wm_window_t *win = find_window(resize_state.win_id);
            if (win) {
                int ox = resize_state.rendered_x, oy = resize_state.rendered_y;
                int ow = resize_state.rendered_w, oh = resize_state.rendered_h + TITLEBAR_H;
                int nx = win->x, ny = win->y;
                int nw = win->w, nh = win->h + TITLEBAR_H;
                int minx = imin(ox, nx);
                int miny = imin(oy, ny);
                int maxx = imax(ox + ow, nx + nw);
                int maxy = imax(oy + oh, ny + nh);
                int m = WIN_MARGIN;
                wm_render_region(minx - m, miny - m,
                                 (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                resize_state.rendered_x = nx;
                resize_state.rendered_y = ny;
                resize_state.rendered_w = win->w;
                resize_state.rendered_h = win->h;
                return;
            }
        }
        /* Перетаскивание окна — частичная перерисовка только объединения
         * старого и нового положения окна (как делают настоящие композиторы),
         * вместо полного clear+flip всего экрана. */
        if (drag_state.dragging) {
            wm_window_t *win = find_window(drag_state.win_id);
            if (win) {
                int ox = drag_state.rendered_x, oy = drag_state.rendered_y;
                int nx = win->x,               ny = win->y;
                int fh = win->h + TITLEBAR_H;  /* окно + title bar */
                int minx = imin(ox, nx);
                int miny = imin(oy, ny);
                int maxx = imax(ox + win->w, nx + win->w);
                int maxy = imax(oy + fh,     ny + fh);
                /* Запас WIN_MARGIN под мягкую тень окна (иначе она «смажется»). */
                int m = WIN_MARGIN;
                wm_render_region(minx - m, miny - m,
                                 (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                drag_state.rendered_x = nx;
                drag_state.rendered_y = ny;
                return;
            }
        }
        /* Сцена менялась иначе (создание окна, wm_flush и т.п.) — полный render. */
        wm_render_all();
    } else if (g_panel_dirty) {
        /* Тикнули часы — перерисовываем только полосу верхней панели (region),
         * без полного кадра. */
        g_panel_dirty = 0;
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        wm_render_region(px, py, pw, ph);
    } else if (g_dock_dirty) {
        /* Менялось только состояние dock (hover/press) — перерисовываем лишь
         * область dock (region), без полного кадра всего экрана. */
        g_dock_dirty = 0;
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        wm_render_region(bx, by, bw, bh);
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

    /* Будим задачу, уснувшую в wm_wait_event на этом окне (если есть). Зовётся
     * из IRQ клавиатуры (прерывания уже off) — поэтому проверка пустоты очереди
     * и блокировка в wm_wait_event под cli не словят lost-wakeup. */
    if (win->waiter) {
        sched_wake((task_t *)win->waiter);
        win->waiter = 0;
    }
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

/* -------------------------------------------------------------------------
 * Блокирующее ожидание события — «как у взрослых ОС».
 *
 * Вместо того чтобы userspace крутил busy-poll (wm_get_event в цикле + pause),
 * сжигая весь свой квант, задача УСЫПЛЯЕТСЯ: сохраняем её как win->waiter,
 * помечаем TASK_BLOCKED и отдаём процессор. Планировщик больше не ставит её на
 * CPU, пока IRQ клавиатуры (wm_enqueue_key) не положит событие и не разбудит.
 * Тогда задача рендера и остальные получают процессор беспрепятственно —
 * курсор и перетаскивание окон идут плавно, а простаивающее приложение ест 0%
 * CPU.
 *
 * Защита от lost-wakeup: проверка очереди, регистрация будильщика и блокировка
 * делаются с ВЫКЛЮЧЕННЫМИ прерываниями (cli). `sti; hlt` неделим (sti вступает
 * в силу только после следующей инструкции), поэтому прерывание не «проскочит»
 * между разблокировкой и hlt. Первый же IRQ после hlt вызовет sched_pick,
 * который (раз мы BLOCKED) переключится на другую задачу. Когда нас разбудят,
 * планировщик вернёт нам процессор ровно сюда — за hlt, и мы перечитаем очередь.
 * ------------------------------------------------------------------------- */
int wm_wait_event(uint64_t win_id, void *event_out) {
    wm_window_t *win = find_window(win_id);
    if (!win) return 0;

    for (;;) {
        __asm__ volatile("cli");
        if (win->event_head != win->event_tail) {
            __asm__ volatile("sti");
            return wm_get_event(win_id, event_out);
        }
        /* Очередь пуста — регистрируем себя и засыпаем. */
        win->waiter = sched_current();
        sched_block_current();              /* state=BLOCKED, need_resched=1 */
        __asm__ volatile("sti\n\thlt");     /* неделимо; первый IRQ переключит */
        /* Проснулись (нас разбудил sched_wake из IRQ клавиатуры) — на новый круг
         * перечитываем очередь под cli. */
    }
}

void wm_handle_mouse_move(int dx, int dy) {
    comp_update_mouse(dx, -dy, compositor_get()->mouse_buttons);

    // Если меняем размер окна — пересчитываем геометрию по краю/углу.
    if (resize_state.active) {
        wm_window_t *win = find_window(resize_state.win_id);
        if (win) {
            compositor_t *comp = compositor_get();
            int mdx = comp->mouse_x - resize_state.start_mx;
            int mdy = comp->mouse_y - resize_state.start_my;
            int nx = resize_state.start_x, ny = resize_state.start_y;
            int nw = resize_state.start_w, nh = resize_state.start_h;

            if (resize_state.edge & RZ_RIGHT)  nw = resize_state.start_w + mdx;
            if (resize_state.edge & RZ_BOTTOM) nh = resize_state.start_h + mdy;
            if (resize_state.edge & RZ_LEFT) { nx = resize_state.start_x + mdx; nw = resize_state.start_w - mdx; }
            if (resize_state.edge & RZ_TOP)  { ny = resize_state.start_y + mdy; nh = resize_state.start_h - mdy; }

            /* Минимальный размер: при тяге за левый/верхний край держим
             * противоположную сторону на месте. */
            if (nw < MIN_WIN_W) {
                if (resize_state.edge & RZ_LEFT) nx = resize_state.start_x + resize_state.start_w - MIN_WIN_W;
                nw = MIN_WIN_W;
            }
            if (nh < MIN_WIN_H) {
                if (resize_state.edge & RZ_TOP) ny = resize_state.start_y + resize_state.start_h - MIN_WIN_H;
                nh = MIN_WIN_H;
            }
            /* Не выходим за экран. */
            if (nx < 0) { if (resize_state.edge & RZ_LEFT) nw += nx; nx = 0; }
            if (ny < 0) { if (resize_state.edge & RZ_TOP)  nh += ny; ny = 0; }
            if (nx + nw > (int)comp->width)  nw = (int)comp->width - nx;
            if (ny + nh + TITLEBAR_H > (int)comp->height) nh = (int)comp->height - ny - TITLEBAR_H;
            if (nw < MIN_WIN_W) nw = MIN_WIN_W;
            if (nh < MIN_WIN_H) nh = MIN_WIN_H;
            /* Буфер содержимого фиксирован (MAX_WINDOW_PIXELS) — не превышаем. */
            if (nw * nh > MAX_WINDOW_PIXELS) {
                if ((resize_state.edge & (RZ_LEFT | RZ_RIGHT)) && nw > MIN_WIN_W)
                    nw = MAX_WINDOW_PIXELS / nh;
                else if (nh > MIN_WIN_H)
                    nh = MAX_WINDOW_PIXELS / nw;
            }

            if (nw != win->w || nh != win->h || nx != win->x || ny != win->y) {
                win->x = nx; win->y = ny; win->w = nw; win->h = nh;
                /* Чистим содержимое под новый stride (win->w), чтобы не было
                 * «разъезда» старых пикселей, и просим приложение перерисоваться. */
                int total = nw * nh;
                for (int j = 0; j < total; j++) win->pixels[j] = 0xFF2A2A3E;
                wm_enqueue_resize(win);
                g_needs_redraw = 1;
            }
        }
        return;  /* во время resize dock-hover/курсор-путь не трогаем */
    }

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
            if (win->y + win->h + TITLEBAR_H > (int)comp->height) win->y = comp->height - win->h - TITLEBAR_H;
        }
    }
    
    /* Hover над dock: если иконка под курсором сменилась — перерисуем dock. */
    if (!drag_state.dragging) {
        compositor_t *c = compositor_get();
        int hit = dock_hit(c->mouse_x, c->mouse_y);
        int hov = (hit >= 0) ? hit : -1;
        if (hov != g_dock_hover) {
            g_dock_hover = hov;
            if (hov < 0) g_dock_pressed = 0; /* ушли с иконки — снимаем нажатие */
            g_dock_dirty = 1;
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

    /* --- Dock имеет приоритет: он поверх окон --- */
    int dh = dock_hit(mouse_x, mouse_y);
    if (buttons & 1) {
        if (dh >= 0) {
            /* Нажатие на иконку: визуальный отклик + запрос на запуск.
             * Теперь у каждого процесса своё адресное пространство + свой
             * kernel-стек (per-task CR3/syscall-стек), поэтому можно открывать
             * несколько терминалов — вплоть до MAX_WINDOWS. */
            g_dock_hover = dh;
            g_dock_pressed = 1;
            g_dock_dirty = 1;
            if (dh == 0 && wm_active_window_count() < MAX_WINDOWS) {
                g_dock_launch_request = 1;
                /* Будим заснувшую задачу-лаунчер дока (block, don't poll). Мы
                 * уже в обработчике mouse IRQ (прерывания off) — флаг выставлен
                 * до sched_wake, поэтому lost-wakeup невозможен. */
                if (g_dock_task) sched_wake(g_dock_task);
            }
            return;  /* не таскаем окно под доком */
        }
        if (dh == -2) {  /* клик по пилюле мимо иконки — просто гасим */
            return;
        }
    } else {
        if (g_dock_pressed) { g_dock_pressed = 0; g_dock_dirty = 1; }
    }

    /* --- Кнопки заголовка (закрыть/свернуть/развернуть) имеют приоритет над
     * перетаскиванием. Идём сверху вниз; берём верхнее окно под курсором. --- */
    if (buttons & 1) {
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            wm_window_t *win = &windows[i];
            if (win->id == 0 || !win->visible) continue;
            int inside = (mouse_x >= win->x && mouse_x < win->x + win->w &&
                          mouse_y >= win->y && mouse_y < win->y + win->h + TITLEBAR_H);
            if (!inside) continue;
            int b = win_button_hit(win, mouse_x, mouse_y);
            if (b == 0) {            /* 🔴 закрыть окно */
                uint64_t id = win->id;
                if (g_focused_win_id == id) g_focused_win_id = 0;
                if (drag_state.dragging && drag_state.win_id == id)
                    drag_state.dragging = false;
                if (resize_state.active && resize_state.win_id == id)
                    resize_state.active = false;
                wm_destroy_window(id);
                g_needs_redraw = 1;
                return;
            }
            if (b >= 0) {            /* 🟡/🟢 свернуть/развернуть: пока зарезервировано
                                      * (нужен taskbar для restore + resize-протокол) */
                g_needs_redraw = 1;
                return;
            }
            break;  /* верхнее окно под курсором, но не по кнопке → обычная обработка */
        }
    }

    // Проверяем клик на title bar окна (идем с конца, чтобы верхние окна были первыми)
    if (buttons & 1) {  // Левая кнопка нажата
        /* Фокус ввода переходит к окну, по которому кликнули (title или тело). */
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            wm_window_t *win = &windows[i];
            if (win->id == 0 || !win->visible) continue;
            if (mouse_x >= win->x && mouse_x < win->x + win->w &&
                mouse_y >= win->y && mouse_y < win->y + win->h + TITLEBAR_H) {
                g_focused_win_id = win->id;
                break;
            }
        }
        /* --- Resize имеет приоритет над перетаскиванием: тянем за край/угол
         * (как в Windows). Идём сверху вниз; берём верхнее окно, которое
         * «владеет» точкой (курсор в пределах окна + зоны захвата). --- */
        if (!drag_state.dragging && !resize_state.active) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                wm_window_t *win = &windows[i];
                if (win->id == 0 || !win->visible) continue;
                int edge = resize_edge_hit(win, mouse_x, mouse_y);
                /* Точка внутри окна (или его зоны захвата)? Если да — это «наше»
                 * верхнее окно: либо ресайзим (edge!=0), либо отдаём клик дальше. */
                int owns = (mouse_x >= win->x - RESIZE_BORDER && mouse_x <= win->x + win->w + RESIZE_BORDER &&
                            mouse_y >= win->y - RESIZE_BORDER && mouse_y <= win->y + win->h + TITLEBAR_H + RESIZE_BORDER);
                if (!owns) continue;
                if (edge) {
                    resize_state.active = true;
                    resize_state.win_id = win->id;
                    resize_state.edge = edge;
                    resize_state.start_mx = mouse_x;
                    resize_state.start_my = mouse_y;
                    resize_state.start_x = win->x;
                    resize_state.start_y = win->y;
                    resize_state.start_w = win->w;
                    resize_state.start_h = win->h;
                    resize_state.rendered_x = win->x;
                    resize_state.rendered_y = win->y;
                    resize_state.rendered_w = win->w;
                    resize_state.rendered_h = win->h;
                    g_focused_win_id = win->id;
                    g_needs_redraw = 1;
                    return;
                }
                break;  /* верхнее окно владеет точкой, но не у края → обычная обработка */
            }
        }

        if (!drag_state.dragging && !resize_state.active) {
            // Начинаем перетаскивание
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                wm_window_t *win = &windows[i];
                if (win->id == 0 || !win->visible) continue;
                
                // Проверяем попадание в title bar (высота 20 пикселей)
                if (mouse_x >= win->x && mouse_x < win->x + win->w &&
                    mouse_y >= win->y && mouse_y < win->y + TITLEBAR_H) {
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
        // Кнопка отпущена - заканчиваем перетаскивание и resize
        drag_state.dragging = false;
        resize_state.active = false;
    }
    
    /* Render отвязан от ввода: в IRQ только ставим флаг, рисует таймер. */
    g_needs_redraw = 1;
}
