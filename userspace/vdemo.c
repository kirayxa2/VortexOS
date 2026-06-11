/* =============================================================================
 * VortexOS — userspace/vdemo.c
 * vdemo — минимальный клиент userspace window manager'а (/vwm) и живой пример
 * протокола: создать окно через IPC, нарисовать содержимое в shm-поверхность,
 * закоммитить, реагировать на resize/close. Хорошая отправная точка для новых
 * приложений.
 * ============================================================================= */

#include "vos_abi.h"
#include "font8x16.h"

#define START_W 400
#define START_H 300

#define COL_BG     0xFF2A2A3E
#define COL_ACCENT 0xFF007ACC
#define COL_FG     0xFFE0E0E0
#define COL_DIM    0xFF9090A8

static uint64_t  wm_pid = 0;
static uint64_t  win_id = 0;
static uint32_t *surf = 0;
static int win_w = START_W, win_h = START_H;

static void draw_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= win_h) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= win_w) continue;
            surf[(uint32_t)py * win_w + px] = c;
        }
    }
}
static void draw_text(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        uint8_t idx = (uint8_t)*s;
        if (idx >= 128) idx = '?';
        const unsigned char *glyph = vos_font[idx];
        for (int row = 0; row < 16; row++) {
            int py = y + row;
            if (py >= 0 && py < win_h) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int px = cx + col;
                    if (px >= 0 && px < win_w)
                        surf[(uint32_t)py * win_w + px] = fg;
                }
            }
        }
        cx += 8;
        s++;
    }
}

static void render(void) {
    draw_rect(0, 0, win_w, win_h, COL_BG);
    draw_rect(0, 0, win_w, 4, COL_ACCENT);              /* акцентная полоса */
    draw_text(16, 20, "Hello from userspace WM!", COL_FG);
    draw_text(16, 44, "This window is drawn by the", COL_DIM);
    draw_text(16, 62, "/vdemo process in ring3:", COL_DIM);
    draw_text(16, 86, "- surface: shared memory", COL_FG);
    draw_text(16, 104, "- events: IPC from /vwm", COL_FG);
    draw_text(16, 122, "- kernel never touches pixels", COL_FG);
    draw_text(16, 152, "Drag an edge to resize.", COL_DIM);
    draw_text(16, 170, "Red button closes me.", COL_DIM);
    /* рамочка-углы для наглядности геометрии */
    draw_rect(4, win_h - 8, 24, 2, COL_ACCENT);
    draw_rect(win_w - 28, win_h - 8, 24, 2, COL_ACCENT);
    vwm_commit(wm_pid, win_id, 0, 0, win_w, win_h);
}

void _start(void) {
    wm_pid = vwm_wait_for_wm();
    win_id = vwm_create_window(wm_pid, "vdemo - userspace window",
                               START_W, START_H, &surf);
    if (!win_id) {
        puts("vdemo: failed to create window\n");
        exit(1);
    }
    render();

    vos_msg_t m;
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        switch (m.w[0]) {
        case VWM_EV_RESIZE:
            if (m.w[1] == win_id) {
                win_w = (int)m.w[2];
                win_h = (int)m.w[3];
                render();
            }
            break;
        case VWM_EV_CLOSE:
            exit(0);
            break;
        default:
            break;
        }
    }
}
