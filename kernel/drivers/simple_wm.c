#include "simple_wm.h"
#include "heap.h"
#include "compositor.h"
#include "pmm.h"
#include "vmm.h"

static wm_window_t windows[MAX_WINDOWS];
static uint32_t next_win_id = 1;
static bool compositor_initialized = false;
static uint64_t last_cursor_update = 0;

/* Состояние перетаскивания */
static struct {
    bool dragging;
    uint64_t win_id;
    int32_t offset_x;  // Offset мыши относительно окна
    int32_t offset_y;
} drag_state = {0};

/* НЕ используем статические буферы - выделим через PMM при создании окна */
static uint32_t *window_buffers[MAX_WINDOWS] = {0};

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
    wm_render_all();
}

void wm_render_all(void) {
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
        
        // Draw window content from offscreen buffer
        for (int32_t dy = 0; dy < win->h; dy++) {
            for (int32_t dx = 0; dx < win->w; dx++) {
                comp_put_pixel(win->x + dx, win->y + 20 + dy, win->pixels[dy * win->w + dx]);
            }
        }
    }
    
    /* Draw cursor */
    comp_draw_cursor();
    
    /* Flip back buffer to screen */
    comp_flip();
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
    
    /* Обновляем экран сразу для плавного курсора */
    wm_render_all();
}

void wm_handle_mouse_button(uint8_t buttons) {
    compositor_t *comp = compositor_get();
    comp->mouse_buttons = buttons;
    
    int mouse_x = comp->mouse_x;
    int mouse_y = comp->mouse_y;
    
    // Проверяем клик на title bar окна (идем с конца, чтобы верхние окна были первыми)
    if (buttons & 1) {  // Левая кнопка нажата
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
                    break;
                }
            }
        }
    } else {
        // Кнопка отпущена - заканчиваем перетаскивание
        drag_state.dragging = false;
    }
    
    /* Обновляем при клике */
    wm_render_all();
}
