#ifndef SIMPLE_WM_H
#define SIMPLE_WM_H

#include "types.h"

#define MAX_WINDOWS 2
#define MAX_WM_EVENTS 16
#define MAX_WINDOW_PIXELS (640 * 480)  // Maximum pixels per window

typedef struct {
    uint32_t id;
    int32_t x, y, w, h;
    uint32_t *pixels;  // Offscreen buffer
    char title[64];
    uint32_t owner_pid;
    bool visible;
    bool dirty;
    
    // Event queue
    uint32_t events[MAX_WM_EVENTS * 8];  // Each event is 8 uint32_t
    uint32_t event_head;
    uint32_t event_tail;
} wm_window_t;

void wm_init(void);
uint64_t wm_create_window(const char *title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pid);
void wm_destroy_window(uint64_t win_id);
void wm_draw_rect(uint64_t win_id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void wm_draw_string(uint64_t win_id, int32_t x, int32_t y, const char *str, uint32_t color);
void wm_flush(uint64_t win_id);
int wm_get_event(uint64_t win_id, void *event_out);
void wm_render_all(void);
void wm_handle_mouse_move(int dx, int dy);
void wm_handle_mouse_button(uint8_t buttons);

#endif
