/* =============================================================================
 * VortexOS — userspace/vwm.c
 * vwm — Vortex Window Manager. ПОЛНОСТЬЮ userspace (ring3) оконный менеджер и
 * композитор «по-взрослому» (микроядерная схема, как Wayland в миниатюре):
 *
 *   - ядро НЕ рисует ничего: vwm маппит framebuffer (SYS_FB_MAP), забирает
 *     весь ввод себе (SYS_INPUT_GRAB) и регистрируется сервисом WM;
 *   - клиенты (/vterm, /vdemo) рисуют ТОЛЬКО содержимое окна в свою
 *     shm-поверхность и шлют COMMIT — пиксели НЕ копируются через ядро,
 *     vwm видит тот же буфер (shared memory);
 *   - декорации (заголовок, кнопки, тень, скругления), панель, dock, иконки
 *     рабочего стола, курсор, drag/resize, фокус — всё здесь, в ring3;
 *   - рендер — повзрослому: персистентный back buffer + damage rectangles
 *     (перерисовываем и копируем во front buffer только изменившееся),
 *     курсор — save-under, кадр ~50 FPS по тикам PIT, vsync через ядро.
 *
 * Главный цикл — однопоточный event loop: ipc_recv с таймаутом 1 тик служит
 * одновременно очередью событий (ввод от ядра + сообщения клиентов) и
 * таймером кадра. Никаких busy-poll: пусто — спим в ядре.
 * ============================================================================= */

#include "vos_abi.h"
#include "font8x16.h"

/* ---------------------------------------------------------------------------
 * Геометрия и палитра (соответствуют kernel simple_wm, чтобы вид не менялся)
 * ------------------------------------------------------------------------- */
#define TITLEBAR_H    24
#define WIN_CORNER    10
#define WIN_SHADOW    10
#define WIN_SHADOW_OY 4
#define WIN_MARGIN    (WIN_SHADOW + WIN_SHADOW_OY)
#define BTN_R         5
#define BTN_GAP       16
#define BTN_X0        14

#define RESIZE_BORDER 6
#define RZ_TOP_INNER  3   /* зона ресайза ВНУТРИ титлбара — узкая, чтобы не съедать drag */
#define CORNER_GRAB   16  /* захват угла: столько px вдоль кромки от угла = диагональ  */
#define MIN_WIN_W     140
#define MIN_WIN_H     80
#define RZ_LEFT       1
#define RZ_RIGHT      2
#define RZ_TOP        4
#define RZ_BOTTOM     8

#define DESK_BG       0xFF1A1A2Eu
#define PANEL_H       24
#define PANEL_BG      0xFF15151Fu

#define MAX_WINDOWS   16   /* окон одновременно; держать <= SHM_MAX_SEGS-1 в ядре */

/* ---------------------------------------------------------------------------
 * Экран: front buffer (= то, что сканирует видеокарта) и back buffer (shm)
 * ------------------------------------------------------------------------- */
static uint32_t *fb;            /* front buffer (SYS_FB_MAP)            */
static uint32_t  fbw, fbh;      /* разрешение                            */
static uint32_t  fb_stride;     /* pitch/4                               */
static uint32_t *bb;            /* back buffer (shm, stride = fbw)       */

static int mouse_x, mouse_y;
static uint8_t mouse_buttons;

/* ---------------------------------------------------------------------------
 * Окна
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t id;                /* 0 = слот свободен */
    uint64_t owner_pid;         /* клиент, чтобы слать события */
    int x, y, w, h;             /* геометрия содержимого (без заголовка) */
    uint32_t *pixels;           /* shm-поверхность клиента (stride = w)  */
    uint64_t shm_id;
    int minimized;              /* 🟡 свёрнуто: не рисуем, чип в панели  */
    int maximized;              /* 🟢 развёрнуто на весь рабочий стол    */
    int rest_x, rest_y, rest_w, rest_h;  /* геометрия до maximize        */
    char title[32];
} vwin_t;

static vwin_t windows[MAX_WINDOWS];
static uint64_t next_win_id = 1;
static uint64_t focused_id = 0;

/* drag / resize — порт состояний из kernel simple_wm */
static struct {
    int active; uint64_t win_id;
    int off_x, off_y;
    int rendered_x, rendered_y;
} drag;

static struct {
    int active; uint64_t win_id; int edge;
    int start_mx, start_my;
    int start_x, start_y, start_w, start_h;
    int rendered_x, rendered_y, rendered_w, rendered_h;
} rz;

/* Флаги отрисовки (как в ядре, но без многозадачных гонок — мы однопоточные) */
static int needs_redraw = 1;        /* сцена менялась — полный/region рендер */
static int panel_dirty = 0;
static int dock_dirty = 0;
static int cursor_moved = 0;
static int scene_presented = 0;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ---------------------------------------------------------------------------
 * Примитивы на back buffer (порт kernel compositor.c)
 * ------------------------------------------------------------------------- */
static inline void put_px(int x, int y, uint32_t c) {
    if (x < 0 || x >= (int)fbw || y < 0 || y >= (int)fbh) return;
    bb[(uint32_t)y * fbw + x] = c;
}
static inline uint32_t get_px(int x, int y) {
    if (x < 0 || x >= (int)fbw || y < 0 || y >= (int)fbh) return 0;
    return bb[(uint32_t)y * fbw + x];
}
static void blend_px(int x, int y, uint32_t argb) {
    if (x < 0 || x >= (int)fbw || y < 0 || y >= (int)fbh) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    uint32_t *p = &bb[(uint32_t)y * fbw + x];
    if (a == 0xFF) { *p = 0xFF000000u | (argb & 0x00FFFFFF); return; }
    uint32_t dst = *p;
    uint32_t fr = (argb >> 16) & 0xFF, fg = (argb >> 8) & 0xFF, fbl = argb & 0xFF;
    uint32_t br = (dst >> 16) & 0xFF, bg = (dst >> 8) & 0xFF, bbl = dst & 0xFF;
    uint32_t r = (fr * a + br * (255 - a)) / 255;
    uint32_t g = (fg * a + bg * (255 - a)) / 255;
    uint32_t b = (fbl * a + bbl * (255 - a)) / 255;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}
/* 64-битный копировщик строк пикселей (без UB по strict aliasing). */
typedef uint64_t __attribute__((may_alias)) u64a_t;
static inline void copy_px_row(uint32_t *d, const uint32_t *s, int n) {
    while (n >= 2) {
        *(u64a_t *)d = *(const u64a_t *)s;
        d += 2; s += 2; n -= 2;
    }
    if (n > 0) *d = *s;
}

/* ОПТИМИЗАЦИЯ: клип один раз и заливка строками вместо put_px с проверкой
 * границ на КАЖДЫЙ пиксель — заливка фона была заметной статьёй расходов
 * кадра при drag/resize. */
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fbw) w = (int)fbw - x;
    if (y + h > (int)fbh) h = (int)fbh - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &bb[(uint32_t)(y + j) * fbw + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}
static void fill_circle(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) put_px(cx + dx, cy + dy, c);
}
static void draw_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        put_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}
static void draw_char(int x, int y, char ch, uint32_t fg, uint32_t bg) {
    uint8_t idx = (uint8_t)ch;
    if (idx >= 128) idx = '?';
    const unsigned char *glyph = vos_font[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++)
            put_px(x + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
    }
}
static void draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; }
        else { draw_char(cx, y, *s, fg, bg); cx += 8; }
        s++;
    }
}
/* Блит поверхности окна целыми строками (stride задаёт вызывающий). */
static void blit_buffer(int dx, int dy, int w, int h, const uint32_t *src) {
    if (!src) return;
    for (int row = 0; row < h; row++) {
        int y = dy + row;
        if (y < 0 || y >= (int)fbh) continue;
        int x0 = dx, sx0 = 0, ww = w;
        if (x0 < 0) { sx0 = -x0; ww += x0; x0 = 0; }
        if (x0 + ww > (int)fbw) ww = (int)fbw - x0;
        if (ww <= 0) continue;
        uint32_t       *d = &bb[(uint32_t)y * fbw + x0];
        const uint32_t *s = &src[(uint32_t)row * w + sx0];
        copy_px_row(d, s, ww);
    }
}

/* ---------------------------------------------------------------------------
 * Damage rectangles + present (порт kernel compositor.c)
 * ------------------------------------------------------------------------- */
typedef struct { int x, y, w, h; } rect_t;
#define MAX_DAMAGE 32
static rect_t damage[MAX_DAMAGE];
static int damage_count = 0;
static int damage_full = 0;

static void dmg_reset(void) { damage_count = 0; damage_full = 0; }
static void dmg_all(void)   { damage_full = 1; }
static void dmg_add(int x, int y, int w, int h) {
    if (damage_full) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fbw) w = (int)fbw - x;
    if (y + h > (int)fbh) h = (int)fbh - y;
    if (w <= 0 || h <= 0) return;
    if (damage_count >= MAX_DAMAGE) { damage_full = 1; return; }
    damage[damage_count].x = x; damage[damage_count].y = y;
    damage[damage_count].w = w; damage[damage_count].h = h;
    damage_count++;
}
static void blit_to_front(int x, int y, int w, int h) {
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= (int)fbh) continue;
        int x0 = x, ww = w;
        if (x0 < 0) { ww += x0; x0 = 0; }
        if (x0 + ww > (int)fbw) ww = (int)fbw - x0;
        if (ww <= 0) continue;
        uint32_t       *d = &fb[(uint32_t)yy * fb_stride + x0];
        const uint32_t *s = &bb[(uint32_t)yy * fbw + x0];
        copy_px_row(d, s, ww);
    }
}
static void present(void) {
    vos_vsync();                     /* Limine-путь: ждём vblank (no-op на virtio) */
    if (damage_full) {
        blit_to_front(0, 0, (int)fbw, (int)fbh);
        vos_fb_present(0, 0, (int)fbw, (int)fbh);
        dmg_reset();
        return;
    }
    for (int i = 0; i < damage_count; i++) {
        blit_to_front(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
        vos_fb_present(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
    }
    dmg_reset();
}

/* ---------------------------------------------------------------------------
 * Курсор (save-under, порт kernel compositor.c)
 * ------------------------------------------------------------------------- */
#define CUR_MAX_W 19
#define CUR_MAX_H 19

/* Формы курсора: стрелка + 4 ресайзных (как в Windows) */
enum {
    CUR_ARROW = 0,
    CUR_SIZE_H,      /* ↔  левый/правый край          */
    CUR_SIZE_V,      /* ↕  верхний/нижний край         */
    CUR_SIZE_NWSE,   /* ⤡  углы ЛВ/ПН                  */
    CUR_SIZE_NESW,   /* ⤢  углы ПВ/ЛН                  */
    CUR_NSHAPES
};

static const char *const cur_rows_arrow[19] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};
static const char *const cur_rows_size_h[9] = {
    "    X         X    ",
    "   XX         XX   ",
    "  X.X         X.X  ",
    " X..XXXXXXXXXXX..X ",
    "X.................X",
    " X..XXXXXXXXXXX..X ",
    "  X.X         X.X  ",
    "   XX         XX   ",
    "    X         X    ",
};
static const char *const cur_rows_size_v[19] = {
    "    X    ",
    "   X.X   ",
    "  X...X  ",
    " X.....X ",
    "XXXX.XXXX",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "   X.X   ",
    "XXXX.XXXX",
    " X.....X ",
    "  X...X  ",
    "   X.X   ",
    "    X    ",
};
static const char *const cur_rows_size_nwse[15] = {
    "XXXXXXX        ",
    "X....X         ",
    "X...X          ",
    "X..X.X         ",
    "X.X...X        ",
    "XX X...X       ",
    "X   X...X      ",
    "     X...X     ",
    "      X...X   X",
    "       X...X XX",
    "        X...X.X",
    "         X.X..X",
    "          X...X",
    "         X....X",
    "        XXXXXXX",
};
static const char *const cur_rows_size_nesw[15] = {
    "        XXXXXXX",
    "         X....X",
    "          X...X",
    "         X.X..X",
    "        X...X.X",
    "       X...X XX",
    "      X...X   X",
    "     X...X     ",
    "X   X...X      ",
    "XX X...X       ",
    "X.X...X        ",
    "X..X.X         ",
    "X...X          ",
    "X....X         ",
    "XXXXXXX        ",
};

typedef struct {
    int w, h;                 /* размер спрайта                  */
    int hx, hy;               /* hotspot (точка под mouse_x/y)   */
    const char *const *rows;
} cur_shape_t;

static const cur_shape_t cur_shapes[CUR_NSHAPES] = {
    [CUR_ARROW]     = { 12, 19, 0, 0, cur_rows_arrow     },
    [CUR_SIZE_H]    = { 19,  9, 9, 4, cur_rows_size_h    },
    [CUR_SIZE_V]    = {  9, 19, 4, 9, cur_rows_size_v    },
    [CUR_SIZE_NWSE] = { 15, 15, 7, 7, cur_rows_size_nwse },
    [CUR_SIZE_NESW] = { 15, 15, 7, 7, cur_rows_size_nesw },
};

static int cur_shape = CUR_ARROW;             /* текущая форма (hover)      */
static uint32_t cur_save[CUR_MAX_W * CUR_MAX_H];
static int cur_x, cur_y, cur_w, cur_h, cur_in_back = 0;

static void cursor_sprite(const cur_shape_t *s, int x, int y) {
    for (int j = 0; j < s->h; j++) {
        const char *row = s->rows[j];
        for (int i = 0; i < s->w; i++) {
            if (row[i] == 'X')      put_px(x + i, y + j, 0xFF000000);
            else if (row[i] == '.') put_px(x + i, y + j, 0xFFFFFFFF);
        }
    }
}
static void cursor_blit(void) {
    const cur_shape_t *s = &cur_shapes[cur_shape];
    int x = mouse_x - s->hx, y = mouse_y - s->hy;
    for (int j = 0; j < s->h; j++)
        for (int i = 0; i < s->w; i++)
            cur_save[j * s->w + i] = get_px(x + i, y + j);
    cursor_sprite(s, x, y);
    cur_x = x; cur_y = y; cur_w = s->w; cur_h = s->h; cur_in_back = 1;
}
static void cursor_unblit(void) {
    if (!cur_in_back) return;
    for (int j = 0; j < cur_h; j++)
        for (int i = 0; i < cur_w; i++)
            put_px(cur_x + i, cur_y + j, cur_save[j * cur_w + i]);
    cur_in_back = 0;
}
static void cursor_compose(void) {
    cur_in_back = 0;
    cursor_blit();
    dmg_add(cur_x, cur_y, cur_w, cur_h);
}
static void cursor_take_down(void) {
    if (!cur_in_back) return;
    dmg_add(cur_x, cur_y, cur_w, cur_h);
    cursor_unblit();
}
static void cursor_refresh(void) {
    int ox = cur_x, oy = cur_y, ow = cur_w, oh = cur_h, had = cur_in_back;
    cursor_unblit();
    cursor_blit();
    if (had) dmg_add(ox, oy, ow, oh);
    dmg_add(cur_x, cur_y, cur_w, cur_h);
    present();
}

/* ---------------------------------------------------------------------------
 * Dock (порт kernel simple_wm: «пилюля» в стиле macOS, терминал /vterm)
 * ------------------------------------------------------------------------- */
#define DOCK_ICON   48
#define DOCK_PAD    12
#define DOCK_GAP    12
#define DOCK_BOTTOM 16
#define DOCK_NITEMS 1

static int dock_hover = -1;
static int dock_pressed = 0;

static void dock_geometry(int *dx, int *dy, int *dw, int *dh) {
    int h = DOCK_ICON + DOCK_PAD * 2;
    int w = DOCK_NITEMS * DOCK_ICON + (DOCK_NITEMS - 1) * DOCK_GAP + DOCK_PAD * 2;
    *dx = ((int)fbw - w) / 2;
    *dy = (int)fbh - h - DOCK_BOTTOM;
    *dw = w; *dh = h;
}
static void dock_icon_rect(int idx, int *ix, int *iy) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    (void)dw; (void)dh;
    *ix = dx + DOCK_PAD + idx * (DOCK_ICON + DOCK_GAP);
    *iy = dy + DOCK_PAD;
}
static int dock_hit(int mx, int my) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    if (mx < dx || mx >= dx + dw || my < dy || my >= dy + dh) return -1;
    for (int k = 0; k < DOCK_NITEMS; k++) {
        int ix, iy; dock_icon_rect(k, &ix, &iy);
        if (mx >= ix && mx < ix + DOCK_ICON && my >= iy && my < iy + DOCK_ICON)
            return k;
    }
    return -2;
}
static void dock_put_blend(int x, int y, uint32_t color, int a) {
    if (a >= 255) { put_px(x, y, color); return; }
    blend_px(x, y, ((uint32_t)a << 24) | (color & 0x00FFFFFF));
}
static void fill_round(int x, int y, int w, int h, int r, uint32_t color, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int cx = -1, cy = -1;
            if (i < r && j < r)               { cx = r;         cy = r;         }
            else if (i >= w - r && j < r)     { cx = w - r - 1; cy = r;         }
            else if (i < r && j >= h - r)     { cx = r;         cy = h - r - 1; }
            else if (i >= w - r && j >= h - r){ cx = w - r - 1; cy = h - r - 1; }
            if (cx >= 0) {
                int ddx = i - cx, ddy = j - cy;
                if (ddx * ddx + ddy * ddy > r * r) continue;
            }
            dock_put_blend(x + i, y + j, color, a);
        }
    }
}
static void dock_draw_terminal(int x, int y, int s, int pressed) {
    if (pressed) { x += 1; y += 1; }
    fill_round(x, y, s, s, 11, 0xFF16161F, 255);
    for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);
    int cy = y + 9;
    fill_circle(x + 11, cy, 3, 0xFFFF5F56);
    fill_circle(x + 21, cy, 3, 0xFFFFBD2E);
    fill_circle(x + 31, cy, 3, 0xFF27C93F);
    uint32_t green = 0xFF3BE06F;
    int px = x + 10, py = y + 23;
    for (int t = 0; t < 2; t++) {
        draw_line(px,     py + t,     px + 7, py + 6 + t,  green);
        draw_line(px + 7, py + 6 + t, px,     py + 12 + t, green);
    }
    fill_rect(x + 23, py + 9, 12, 4, green);
}
static void draw_dock(void) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    int r = dh / 2;
    fill_round(dx - 2, dy + 5, dw + 4, dh, r, 0xFF000000, 45);
    fill_round(dx, dy, dw, dh, r, 0xFF22222F, 205);
    for (int i = r; i < dw - r; i++) dock_put_blend(dx + i, dy + 1, 0xFFFFFFFF, 30);
    for (int k = 0; k < DOCK_NITEMS; k++) {
        int ix, iy; dock_icon_rect(k, &ix, &iy);
        int hovered = (dock_hover == k);
        int pressed = (dock_pressed && dock_hover == k);
        if (hovered)
            fill_round(ix - 4, iy - 4, DOCK_ICON + 8, DOCK_ICON + 8, 14,
                       0xFFFFFFFF, pressed ? 60 : 35);
        dock_draw_terminal(ix, iy, DOCK_ICON, pressed);
    }
}
static void dock_bounds(int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    *x = dx - 4; *y = dy - 4; *w = dw + 8; *h = dh + 12;
}

/* ---------------------------------------------------------------------------
 * Иконки рабочего стола (порт kernel simple_wm, пути под userspace-клиенты)
 * ------------------------------------------------------------------------- */
#define DESK_ICONSZ 48
#define DESK_CELL_W 80
#define DESK_CELL_H 84
#define DESK_X0     12
#define DESK_Y0     36
#define DESK_SEL_BG 0xFF273458u

typedef struct { const char *path; const char *label; int kind; } desk_icon_t;
static const desk_icon_t desk_icons[] = {
    { "/bin/vterm", "Terminal", 0 },
    { "/bin/vdemo", "Window",   2 },
};
#define DESK_NICONS ((int)(sizeof(desk_icons) / sizeof(desk_icons[0])))

static int desk_selected = -1;
static uint64_t desk_last_click_tick = 0;
static int desk_last_click_idx = -1;

static void desk_cell_rect(int idx, int *cx, int *cy) {
    *cx = DESK_X0;
    *cy = DESK_Y0 + idx * DESK_CELL_H;
}
static int desk_icon_hit(int mx, int my) {
    for (int k = 0; k < DESK_NICONS; k++) {
        int cx, cy; desk_cell_rect(k, &cx, &cy);
        if (mx >= cx && mx < cx + DESK_CELL_W && my >= cy && my < cy + DESK_CELL_H)
            return k;
    }
    return -1;
}
static void desk_draw_tile(int kind, int x, int y, int s) {
    if (kind == 0) {
        fill_round(x, y, s, s, 10, 0xFF16161F, 255);
        uint32_t green = 0xFF3BE06F;
        int px = x + 11, py = y + 16;
        for (int t = 0; t < 2; t++) {
            draw_line(px,     py + t,     px + 8, py + 8 + t,  green);
            draw_line(px + 8, py + 8 + t, px,     py + 16 + t, green);
        }
        fill_rect(x + 24, py + 12, 12, 4, green);
    } else {
        fill_round(x, y, s, s, 10, 0xFFEAEAF0, 255);
        fill_rect(x + 8,  y + 10, 32, 28, 0xFFFFFFFF);
        fill_rect(x + 8,  y + 10, 32, 7,  0xFF3D6FB5);
        fill_rect(x + 12, y + 22, 24, 2,  0xFFB8C2D0);
        fill_rect(x + 12, y + 27, 24, 2,  0xFFB8C2D0);
        fill_rect(x + 12, y + 32, 16, 2,  0xFFB8C2D0);
    }
}
static void desk_draw_one(int idx) {
    int cx, cy; desk_cell_rect(idx, &cx, &cy);
    int selected = (desk_selected == idx);
    uint32_t lbl_bg = DESK_BG;
    if (selected) {
        fill_round(cx + 2, cy + 2, DESK_CELL_W - 4, DESK_CELL_H - 6, 8,
                   DESK_SEL_BG, 255);
        lbl_bg = DESK_SEL_BG;
    }
    int ix = cx + (DESK_CELL_W - DESK_ICONSZ) / 2;
    int iy = cy + 8;
    desk_draw_tile(desk_icons[idx].kind, ix, iy, DESK_ICONSZ);
    const char *t = desk_icons[idx].label;
    int len = 0; while (t[len]) len++;
    draw_string(cx + (DESK_CELL_W - len * 8) / 2, iy + DESK_ICONSZ + 6,
                t, 0xFFE8E8F0, lbl_bg);
}
static void draw_desktop_icons(int rx, int ry, int rw, int rh) {
    for (int k = 0; k < DESK_NICONS; k++) {
        int cx, cy; desk_cell_rect(k, &cx, &cy);
        if (rx >= cx + DESK_CELL_W || rx + rw <= cx ||
            ry >= cy + DESK_CELL_H || ry + rh <= cy) continue;
        desk_draw_one(k);
    }
}

/* ---------------------------------------------------------------------------
 * Верхняя панель (логотип + активное окно + часы через SYS_RTC)
 * ------------------------------------------------------------------------- */
static vwin_t *find_window(uint64_t id) {
    if (!id) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].id == id) return &windows[i];
    return 0;
}

/* Поднять окно наверх (raise). Z-порядок — это порядок слотов в windows[]:
 * рендер идёт 0..MAX_WINDOWS-1, верхнее окно = наибольший занятый индекс.
 * Сдвигаем слоты выше нашего на один вниз и кладём окно в самый верхний
 * занятый индекс — относительный порядок остальных не меняется.
 * ВАЖНО: после вызова любые vwin_t* невалидны (содержимое слотов
 * переехало) — drag/rz/focused и так живут через win_id. */
static void raise_window(uint64_t id) {
    int i = -1, top = -1;
    for (int k = 0; k < MAX_WINDOWS; k++) {
        if (windows[k].id == id) i = k;
        if (windows[k].id)       top = k;
    }
    if (i < 0 || top <= i) return;            /* нет такого или уже сверху */
    vwin_t tmp = windows[i];
    for (int k = i; k < top; k++) windows[k] = windows[k + 1];
    windows[top] = tmp;
    needs_redraw = 1;
}
static void panel_bounds(int *x, int *y, int *w, int *h) {
    *x = 0; *y = 0; *w = (int)fbw; *h = PANEL_H;
}

/* ---- Чипы свёрнутых окон в панели (слева от часов) --------------------
 * Раскладка считается заново и при отрисовке, и при hit-test — никакого
 * состояния между кадрами (та же дисциплина, что и с win_id). */
#define CHIP_H        18
#define CHIP_TITLE_MAX 12
typedef struct { int x, w; uint64_t id; const char *title; } chip_t;

static int panel_chips(chip_t *out) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *w = &windows[i];
        if (!w->id || !w->minimized) continue;
        int len = 0;
        while (w->title[len] && len < CHIP_TITLE_MAX) len++;
        if (len == 0) len = 1;
        out[n].w = len * 8 + 14;
        out[n].id = w->id;
        out[n].title = w->title;
        n++;
    }
    /* правый край пачки — перед часами; чипы идут слева направо
     * в порядке слотов */
    int x = (int)fbw - 8 * 8 - 24;
    for (int k = n - 1; k >= 0; k--) { x -= out[k].w + 6; out[k].x = x; }
    return n;
}
static int panel_chip_hit(int mx, int my, uint64_t *id_out) {
    if (my >= PANEL_H) return 0;
    chip_t chips[MAX_WINDOWS];
    int n = panel_chips(chips);
    int cy = (PANEL_H - CHIP_H) / 2;
    for (int k = 0; k < n; k++)
        if (mx >= chips[k].x && mx < chips[k].x + chips[k].w &&
            my >= cy && my < cy + CHIP_H) {
            *id_out = chips[k].id;
            return 1;
        }
    return 0;
}
static void draw_panel_chips(void) {
    chip_t chips[MAX_WINDOWS];
    int n = panel_chips(chips);
    int cy = (PANEL_H - CHIP_H) / 2;
    int ty = (PANEL_H - 16) / 2;
    for (int k = 0; k < n; k++) {
        fill_round(chips[k].x, cy, chips[k].w, CHIP_H, 6, 0xFF2A2A3E, 255);
        char buf[CHIP_TITLE_MAX + 1];
        int i = 0;
        while (chips[k].title[i] && i < CHIP_TITLE_MAX) {
            buf[i] = chips[k].title[i];
            i++;
        }
        buf[i] = 0;
        draw_string(chips[k].x + 7, ty, buf, 0xFF9AB8D8, 0xFF2A2A3E);
    }
}

static void draw_panel(void) {
    int W = (int)fbw;
    fill_rect(0, 0, W, PANEL_H, PANEL_BG);
    fill_rect(0, PANEL_H - 1, W, 1, 0xFF007ACC);

    int ty = (PANEL_H - 16) / 2;
    fill_rect(8, PANEL_H / 2 - 4, 8, 8, 0xFF007ACC);
    int lx = 24;
    draw_string(lx, ty, "VortexOS", 0xFFE0E0E0, PANEL_BG);
    lx += 8 * 8;

    vwin_t *fw = find_window(focused_id);
    if (fw && fw->title[0]) {
        fill_rect(lx + 8, ty + 1, 1, 14, 0xFF44445A);
        draw_string(lx + 16, ty, fw->title, 0xFF9AB8D8, PANEL_BG);
    }

    draw_panel_chips();

    uint32_t hms[3];
    vos_rtc(hms);
    char clk[9];
    clk[0] = '0' + hms[0] / 10; clk[1] = '0' + hms[0] % 10; clk[2] = ':';
    clk[3] = '0' + hms[1] / 10; clk[4] = '0' + hms[1] % 10; clk[5] = ':';
    clk[6] = '0' + hms[2] / 10; clk[7] = '0' + hms[2] % 10; clk[8] = 0;
    draw_string(W - 8 * 8 - 10, ty, clk, 0xFFFFFFFF, PANEL_BG);
}

/* ---------------------------------------------------------------------------
 * Оконный chrome (порт kernel simple_wm: тень, AA-скругления, светофоры)
 * ------------------------------------------------------------------------- */
static int isqrt32(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}
/* ОПТИМИЗАЦИЯ (FPS при drag): тень считается ТОЛЬКО по внешним полосам и
 * 4 угловым выемкам, альфа — из таблицы по d2 (вместо isqrt на пиксель).
 * Раньше цикл шёл по всей площади окна+тени (для окна 720x432 это ~380k
 * итераций на КАЖДЫЙ кадр перетаскивания); теперь трогаем только ~25k
 * пикселей собственно тени. Визуально результат идентичен. */
static uint8_t shadow_alut[WIN_SHADOW * WIN_SHADOW];   /* d2 -> alpha */
static void shadow_lut_init(void) {
    for (int d2 = 0; d2 < WIN_SHADOW * WIN_SHADOW; d2++) {
        int a = 78 * (WIN_SHADOW - isqrt32(d2)) / WIN_SHADOW;
        shadow_alut[d2] = (uint8_t)(a > 0 ? a : 0);
    }
}
static void win_draw_shadow(int wx, int wy, int ww, int wh) {
    const int S = WIN_SHADOW, oy = WIN_SHADOW_OY;
    int x0 = wx - S, y0 = wy - S + oy;
    int x1 = wx + ww + S, y1 = wy + wh + S + oy;
    const int r = WIN_CORNER;

    /* 1) внешние полосы: верх/низ — во всю ширину, бока — между ними */
    for (int y = y0; y < y1; y++) {
        int dy = 0;
        if (y < wy)            dy = wy - y;
        else if (y >= wy + wh) dy = y - (wy + wh) + 1;

        if (dy == 0) {
            /* строка на уровне окна: тень только слева и справа */
            for (int x = x0; x < wx; x++) {
                int dx = wx - x;
                if (dx * dx >= S * S) continue;
                int a = shadow_alut[dx * dx];
                if (a) blend_px(x, y, (uint32_t)a << 24);
            }
            for (int x = wx + ww; x < x1; x++) {
                int dx = x - (wx + ww) + 1;
                if (dx * dx >= S * S) continue;
                int a = shadow_alut[dx * dx];
                if (a) blend_px(x, y, (uint32_t)a << 24);
            }
            continue;
        }
        if (dy * dy >= S * S) continue;
        for (int x = x0; x < x1; x++) {
            int dx = 0;
            if (x < wx)            dx = wx - x;
            else if (x >= wx + ww) dx = x - (wx + ww) + 1;
            int d2 = dx * dx + dy * dy;
            if (d2 >= S * S) continue;
            int a = shadow_alut[d2];
            if (a) blend_px(x, y, (uint32_t)a << 24);
        }
    }

    /* 2) угловые выемки ВНУТРИ окна (за скруглением) — ровная тень a=78;
     * поверх неё потом ложится AA-скругление титлбара/низа */
    const uint32_t ca = (uint32_t)shadow_alut[0] << 24;
    for (int k = 0; k < 4; k++) {
        int bx = (k & 1) ? ww - r : 0;
        int by = (k & 2) ? wh - r : 0;
        int cx = (k & 1) ? ww - r : r;
        int cy = (k & 2) ? wh - r : r;
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int lx = bx + i, ly = by + j;
                int ddx = lx - cx, ddy = ly - cy;
                if (ddx * ddx + ddy * ddy <= r * r) continue;
                blend_px(wx + lx, wy + ly, ca);
            }
    }
}
static int circ_cov(int px, int py, int cx, int cy, int r) {
    int r2x64 = r * r * 64;
    int in = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int fx = (px - cx) * 8 + sx * 2 + 1;
            int fy = (py - cy) * 8 + sy * 2 + 1;
            if (fx * fx + fy * fy <= r2x64) in++;
        }
    return in * 255 / 16;
}
static void fill_round_top(int x, int y, int w, int h, int r, uint32_t color) {
    if (r * 2 > w) r = w / 2;
    if (r > h) r = h;
    uint32_t rgb = color & 0x00FFFFFF;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (j < r && (i < r || i >= w - r)) {
                int cx  = (i < r) ? r : (w - r);
                int cov = circ_cov(i, j, cx, r, r);
                if (cov <= 0) continue;
                if (cov >= 255) put_px(x + i, y + j, color);
                else blend_px(x + i, y + j, ((uint32_t)cov << 24) | rgb);
            } else {
                put_px(x + i, y + j, color);
            }
        }
}
static void round_bottom_aa(int x, int y, int w, int fh, int r,
                            const uint32_t *bgL, const uint32_t *bgR) {
    for (int j = 0; j < r; j++) {
        int py = (fh - r) + j;
        for (int i = 0; i < r; i++) {
            int covL = circ_cov(i, py, r, fh - r, r);
            if (covL < 255) {
                uint32_t a = (uint32_t)(255 - covL);
                blend_px(x + i, y + py, (a << 24) | (bgL[j * r + i] & 0x00FFFFFF));
            }
            int ii = w - r + i;
            int covR = circ_cov(ii, py, w - r, fh - r, r);
            if (covR < 255) {
                uint32_t a = (uint32_t)(255 - covR);
                blend_px(x + ii, y + py, (a << 24) | (bgR[j * r + i] & 0x00FFFFFF));
            }
        }
    }
}
static void draw_round_border(int x, int y, int w, int fh, int r, uint32_t color) {
    uint32_t rgb = color & 0x00FFFFFF;
    for (int i = r; i < w - r; i++) {
        put_px(x + i, y,          color);
        put_px(x + i, y + fh - 1, color);
    }
    for (int j = r; j < fh - r; j++) {
        put_px(x,         y + j, color);
        put_px(x + w - 1, y + j, color);
    }
    int cxs[4] = { r, w - r, r,      w - r  };
    int cys[4] = { r, r,     fh - r, fh - r };
    int bxs[4] = { 0, w - r, 0,      w - r  };
    int bys[4] = { 0, 0,     fh - r, fh - r };
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int px = bxs[k] + i, py = bys[k] + j;
                int ring = circ_cov(px, py, cxs[k], cys[k], r)
                         - circ_cov(px, py, cxs[k], cys[k], r - 1);
                if (ring <= 0) continue;
                if (ring >= 255) put_px(x + px, y + py, color);
                else blend_px(x + px, y + py, ((uint32_t)ring << 24) | rgb);
            }
}
static int win_button_hit(const vwin_t *win, int mx, int my) {
    int cy = win->y + TITLEBAR_H / 2;
    for (int k = 0; k < 3; k++) {
        int cx = win->x + BTN_X0 + k * BTN_GAP;
        int dx = mx - cx, dy = my - cy;
        if (dx * dx + dy * dy <= (BTN_R + 2) * (BTN_R + 2)) return k;
    }
    return -1;
}
static int resize_edge_hit(const vwin_t *win, int mx, int my) {
    int ox = win->x, oy = win->y;
    int ow = win->w, oh = win->h + TITLEBAR_H;
    int rb = RESIZE_BORDER;
    if (mx < ox - rb || mx > ox + ow + rb) return 0;
    if (my < oy - rb || my > oy + oh + rb) return 0;
    int edge = 0;
    if (mx >= ox - rb && mx <= ox + rb)           edge |= RZ_LEFT;
    if (mx >= ox + ow - rb && mx <= ox + ow + rb) edge |= RZ_RIGHT;
    /* верх: снаружи полные rb, внутрь титлбара — только RZ_TOP_INNER px,
     * чтобы ресайз не съедал зону перетаскивания заголовка */
    if (my >= oy - rb && my <= oy + RZ_TOP_INNER) edge |= RZ_TOP;
    if (my >= oy + oh - rb && my <= oy + oh + rb) edge |= RZ_BOTTOM;

    /* углы (как в Windows): возле угла захват идёт ВДОЛЬ кромки на
     * CORNER_GRAB px — попадание в боковую полосу рядом с углом даёт
     * диагональный ресайз по двум осям сразу */
    if (edge & (RZ_LEFT | RZ_RIGHT)) {
        if (my <= oy + CORNER_GRAB)           edge |= RZ_TOP;
        else if (my >= oy + oh - CORNER_GRAB) edge |= RZ_BOTTOM;
    }
    if (edge & (RZ_TOP | RZ_BOTTOM)) {
        if (mx <= ox + CORNER_GRAB)           edge |= RZ_LEFT;
        else if (mx >= ox + ow - CORNER_GRAB) edge |= RZ_RIGHT;
    }
    return edge;
}

/* Какую форму курсора показывает данный edge-битмаск */
static int edge_to_shape(int edge) {
    int h = edge & (RZ_LEFT | RZ_RIGHT);
    int v = edge & (RZ_TOP | RZ_BOTTOM);
    if (h && v) {
        if (((edge & RZ_LEFT)  && (edge & RZ_TOP)) ||
            ((edge & RZ_RIGHT) && (edge & RZ_BOTTOM)))
            return CUR_SIZE_NWSE;
        return CUR_SIZE_NESW;
    }
    if (h) return CUR_SIZE_H;
    if (v) return CUR_SIZE_V;
    return CUR_ARROW;
}
static void draw_window_chrome(vwin_t *win) {
    int focused = (win->id == focused_id);
    int fh = win->h + TITLEBAR_H;

    win_draw_shadow(win->x, win->y, win->w, fh);

    uint32_t tcol = focused ? 0xFF3A3A5E : 0xFF2C2C3A;
    fill_round_top(win->x, win->y, win->w, TITLEBAR_H, WIN_CORNER, tcol);
    for (int i = WIN_CORNER; i < win->w - WIN_CORNER; i++)
        blend_px(win->x + i, win->y, 0x28FFFFFFu);

    uint32_t cclose = focused ? 0xFFFF5F56 : 0xFF6E6E78;
    uint32_t cmin   = focused ? 0xFFFFBD2E : 0xFF6E6E78;
    uint32_t cmax   = focused ? 0xFF27C93F : 0xFF6E6E78;
    int cy = win->y + TITLEBAR_H / 2;
    fill_circle(win->x + BTN_X0,               cy, BTN_R, cclose);
    fill_circle(win->x + BTN_X0 + BTN_GAP,     cy, BTN_R, cmin);
    fill_circle(win->x + BTN_X0 + 2 * BTN_GAP, cy, BTN_R, cmax);

    int tx = win->x + BTN_X0 + 2 * BTN_GAP + BTN_R + 10;
    draw_string(tx, win->y + (TITLEBAR_H - 16) / 2, win->title, 0xFFE0E0E0, tcol);

    const int rb = WIN_CORNER;
    uint32_t bgL[WIN_CORNER * WIN_CORNER], bgR[WIN_CORNER * WIN_CORNER];
    if (rb * 2 <= win->w && rb <= fh) {
        for (int j = 0; j < rb; j++) {
            int py = (fh - rb) + j;
            for (int i = 0; i < rb; i++) {
                bgL[j * rb + i] = get_px(win->x + i,               win->y + py);
                bgR[j * rb + i] = get_px(win->x + win->w - rb + i, win->y + py);
            }
        }
    }

    blit_buffer(win->x, win->y + TITLEBAR_H, win->w, win->h, win->pixels);

    if (rb * 2 <= win->w && rb <= fh)
        round_bottom_aa(win->x, win->y, win->w, fh, rb, bgL, bgR);

    uint32_t bord = focused ? 0xFF4A4A72 : 0xFF34343E;
    draw_round_border(win->x, win->y, win->w, fh, WIN_CORNER, bord);
}

/* ---------------------------------------------------------------------------
 * Рендер: полный кадр + частичный регион (порт wm_render_all/wm_render_region)
 * ------------------------------------------------------------------------- */
static void render_all(void) {
    for (uint32_t i = 0; i < fbw * fbh; i++) bb[i] = DESK_BG;
    draw_desktop_icons(0, 0, (int)fbw, (int)fbh);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].id && windows[i].pixels && !windows[i].minimized)
            draw_window_chrome(&windows[i]);
    draw_panel();
    draw_dock();
    cursor_compose();
    dmg_all();
    present();
    scene_presented = 1;
    cursor_moved = 0;
    if (drag.active) {
        vwin_t *dw = find_window(drag.win_id);
        if (dw) { drag.rendered_x = dw->x; drag.rendered_y = dw->y; }
    }
}
static int win_intersects(const vwin_t *win, int rx, int ry, int rw, int rh) {
    int m = WIN_MARGIN;
    int wx = win->x - m, wy = win->y - m;
    int ww = win->w + 2 * m, wh = win->h + TITLEBAR_H + 2 * m;
    if (wx >= rx + rw || wx + ww <= rx) return 0;
    if (wy >= ry + rh || wy + wh <= ry) return 0;
    return 1;
}
static void render_region(int rx, int ry, int rw, int rh) {
    if (rw <= 0 || rh <= 0) return;
    if (!scene_presented) { render_all(); return; }

    dmg_reset();
    cursor_take_down();
    fill_rect(rx, ry, rw, rh, DESK_BG);
    draw_desktop_icons(rx, ry, rw, rh);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id || !win->pixels || win->minimized) continue;
        if (!win_intersects(win, rx, ry, rw, rh)) continue;
        draw_window_chrome(win);
    }

    {
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        if (!(rx >= bx + bw || rx + rw <= bx || ry >= by + bh || ry + rh <= by)) {
            draw_dock();
            dmg_add(bx, by, bw, bh);
        }
    }
    {
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        if (!(rx >= px + pw || rx + rw <= px || ry >= py + ph || ry + rh <= py)) {
            draw_panel();
            dmg_add(px, py, pw, ph);
        }
    }

    cursor_compose();
    cursor_moved = 0;
    dmg_add(rx, ry, rw, rh);
    present();
}

/* Кадр по тику (порт wm_tick_render): выбирает самый дешёвый путь. */
static uint64_t last_panel_sec = 0;

static void tick_render(void) {
    uint64_t sec = vos_uptime() / 100;
    if (sec != last_panel_sec) { last_panel_sec = sec; panel_dirty = 1; }

    if (needs_redraw) {
        needs_redraw = 0;
        panel_dirty = 0;
        dock_dirty = 0;
        if (rz.active) {
            vwin_t *win = find_window(rz.win_id);
            if (win) {
                int ox = rz.rendered_x, oy = rz.rendered_y;
                int ow = rz.rendered_w, oh = rz.rendered_h + TITLEBAR_H;
                int nx = win->x, ny = win->y;
                int nw = win->w, nh = win->h + TITLEBAR_H;
                int minx = imin(ox, nx), miny = imin(oy, ny);
                int maxx = imax(ox + ow, nx + nw), maxy = imax(oy + oh, ny + nh);
                int m = WIN_MARGIN;
                render_region(minx - m, miny - m,
                              (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                rz.rendered_x = nx; rz.rendered_y = ny;
                rz.rendered_w = win->w; rz.rendered_h = win->h;
                return;
            }
        }
        if (drag.active) {
            vwin_t *win = find_window(drag.win_id);
            if (win) {
                int ox = drag.rendered_x, oy = drag.rendered_y;
                int nx = win->x,          ny = win->y;
                int fh = win->h + TITLEBAR_H;
                int minx = imin(ox, nx), miny = imin(oy, ny);
                int maxx = imax(ox + win->w, nx + win->w);
                int maxy = imax(oy + fh,     ny + fh);
                int m = WIN_MARGIN;
                render_region(minx - m, miny - m,
                              (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                drag.rendered_x = nx; drag.rendered_y = ny;
                return;
            }
        }
        render_all();
    } else if (panel_dirty) {
        panel_dirty = 0;
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        render_region(px, py, pw, ph);
    } else if (dock_dirty) {
        dock_dirty = 0;
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        render_region(bx, by, bw, bh);
    } else if (cursor_moved) {
        cursor_moved = 0;
        cursor_refresh();
    }
}

/* Перерисовать только область одного окна (после COMMIT клиента). */
static void render_window_region(vwin_t *win) {
    int m = WIN_MARGIN;
    render_region(win->x - m, win->y - m,
                  win->w + 2 * m, win->h + TITLEBAR_H + 2 * m);
}

/* ---------------------------------------------------------------------------
 * События клиентам
 * ------------------------------------------------------------------------- */
static void send_event(uint64_t pid, uint64_t type, uint64_t a, uint64_t b, uint64_t c) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = type; m.w[1] = a; m.w[2] = b; m.w[3] = c;
    vos_ipc_send(pid, &m);
}

/* Окно умерло (крестик или DESTROY от клиента): снять его из таблицы и
 * ОТДАТЬ shm-поверхность (vos_shm_release). Без release наша ссылка держала
 * бы сегмент до конца жизни vwm — слоты shm кончались бы после ~24 окон за
 * сессию. Клиент свою ссылку отпускает сам при выходе (ядро, task_exit);
 * страницы реально освобождаются, когда отпустят оба. После release пиксели
 * трогать нельзя — маппинг снят. */
static void win_drop(vwin_t *win) {
    if (focused_id == win->id) focused_id = 0;
    if (drag.active && drag.win_id == win->id) drag.active = 0;
    if (rz.active && rz.win_id == win->id) rz.active = 0;
    win->id = 0;
    win->pixels = 0;
    win->minimized = 0;   /* слот переиспользуется — флаги не наследуем */
    win->maximized = 0;
    /* Безусловно: у каждого окна есть поверхность, а shm_id == 0 — валидный
     * id сегмента (нумерация с нуля). */
    vos_shm_release(win->shm_id);
    win->shm_id = 0;
    needs_redraw = 1;
}

static void close_window(vwin_t *win) {
    uint64_t pid = win->owner_pid, id = win->id;
    win_drop(win);
    send_event(pid, VWM_EV_CLOSE, id, 0, 0);   /* клиент должен выйти */
}

/* Применить новую геометрию окна: при смене РАЗМЕРА чистим поверхность под
 * новый stride и просим клиента перерисоваться (EV_RESIZE) — тот же контракт,
 * что и у ресайза мышью. */
static void win_apply_geometry(vwin_t *win, int nx, int ny, int nw, int nh) {
    int resized = (nw != win->w || nh != win->h);
    win->x = nx; win->y = ny; win->w = nw; win->h = nh;
    if (resized) {
        int total = nw * nh;
        for (int j = 0; j < total; j++) win->pixels[j] = 0xFF2A2A3E;
        send_event(win->owner_pid, VWM_EV_RESIZE, win->id,
                   (uint64_t)nw, (uint64_t)nh);
    }
    needs_redraw = 1;
}

/* 🟡 Свернуть: окно исчезает со стола, появляется чипом в панели.
 * Клиент НЕ трогаем — его поверхность жива, COMMIT'ы просто не рисуем. */
static void minimize_window(vwin_t *win) {
    win->minimized = 1;
    if (drag.active && drag.win_id == win->id) drag.active = 0;
    if (rz.active && rz.win_id == win->id) rz.active = 0;
    if (focused_id == win->id) {
        focused_id = 0;                 /* фокус — верхнему из оставшихся */
        for (int i = MAX_WINDOWS - 1; i >= 0; i--)
            if (windows[i].id && !windows[i].minimized) {
                focused_id = windows[i].id;
                break;
            }
    }
    needs_redraw = 1;
}

/* Развернуть из панели: показать, дать фокус, поднять наверх. */
static void restore_window(vwin_t *win) {
    uint64_t id = win->id;
    win->minimized = 0;
    focused_id = id;
    raise_window(id);                   /* win после этого невалиден */
    needs_redraw = 1;
}

/* 🟢 Maximize — тоггл: на весь рабочий стол (под панелью) и обратно.
 * Площадь поверхности ограничена VWM_MAX_PIXELS — если экран больше,
 * подрезаем высоту (как делает ресайз мышью). */
static void toggle_maximize(vwin_t *win) {
    if (!win->maximized) {
        win->rest_x = win->x; win->rest_y = win->y;
        win->rest_w = win->w; win->rest_h = win->h;
        int nw = (int)fbw;
        int nh = (int)fbh - PANEL_H - TITLEBAR_H;
        if (nw * nh > VWM_MAX_PIXELS) nh = VWM_MAX_PIXELS / nw;
        win->maximized = 1;
        win_apply_geometry(win, 0, PANEL_H, nw, nh);
    } else {
        win->maximized = 0;
        win_apply_geometry(win, win->rest_x, win->rest_y,
                           win->rest_w, win->rest_h);
    }
}

/* ---------------------------------------------------------------------------
 * Обработка ввода (порт wm_handle_mouse_move / wm_handle_mouse_button)
 * ------------------------------------------------------------------------- */
static int point_over_window(int mx, int my) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        vwin_t *win = &windows[i];
        if (!win->id || win->minimized) continue;
        if (mx >= win->x && mx < win->x + win->w &&
            my >= win->y && my < win->y + win->h + TITLEBAR_H) return 1;
    }
    return 0;
}

/* Пересчитать форму курсора по тому, что под ним (hover):
 * над краем/углом верхнего окна — ресайзная стрелка, иначе обычная. */
static void update_cursor_shape(void) {
    int shape = CUR_ARROW;
    if (rz.active) {
        shape = edge_to_shape(rz.edge);        /* во время ресайза — форма захвата */
    } else if (!drag.active) {
        if (dock_hit(mouse_x, mouse_y) == -1) {    /* dock поверх окон — там стрелка */
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                vwin_t *win = &windows[i];
                if (!win->id || win->minimized) continue;
                int owns = (mouse_x >= win->x - RESIZE_BORDER &&
                            mouse_x <= win->x + win->w + RESIZE_BORDER &&
                            mouse_y >= win->y - RESIZE_BORDER &&
                            mouse_y <= win->y + win->h + TITLEBAR_H + RESIZE_BORDER);
                if (!owns) continue;
                shape = edge_to_shape(resize_edge_hit(win, mouse_x, mouse_y));
                break;                          /* верхнее окно под курсором решает */
            }
        }
    }
    if (shape != cur_shape) {
        cur_shape = shape;
        cursor_moved = 1;                       /* перерисовать спрайт */
    }
}

static void on_mouse_move(int dx, int dy) {
    mouse_x += dx;
    mouse_y -= dy;                  /* PS/2: Y растёт вверх, экран — вниз */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)fbw) mouse_x = (int)fbw - 1;
    if (mouse_y >= (int)fbh) mouse_y = (int)fbh - 1;

    if (rz.active) {
        vwin_t *win = find_window(rz.win_id);
        if (win) {
            int mdx = mouse_x - rz.start_mx;
            int mdy = mouse_y - rz.start_my;
            int nx = rz.start_x, ny = rz.start_y;
            int nw = rz.start_w, nh = rz.start_h;
            if (rz.edge & RZ_RIGHT)  nw = rz.start_w + mdx;
            if (rz.edge & RZ_BOTTOM) nh = rz.start_h + mdy;
            if (rz.edge & RZ_LEFT) { nx = rz.start_x + mdx; nw = rz.start_w - mdx; }
            if (rz.edge & RZ_TOP)  { ny = rz.start_y + mdy; nh = rz.start_h - mdy; }
            if (nw < MIN_WIN_W) {
                if (rz.edge & RZ_LEFT) nx = rz.start_x + rz.start_w - MIN_WIN_W;
                nw = MIN_WIN_W;
            }
            if (nh < MIN_WIN_H) {
                if (rz.edge & RZ_TOP) ny = rz.start_y + rz.start_h - MIN_WIN_H;
                nh = MIN_WIN_H;
            }
            if (nx < 0) { if (rz.edge & RZ_LEFT) nw += nx; nx = 0; }
            if (ny < 0) { if (rz.edge & RZ_TOP)  nh += ny; ny = 0; }
            if (nx + nw > (int)fbw) nw = (int)fbw - nx;
            if (ny + nh + TITLEBAR_H > (int)fbh) nh = (int)fbh - ny - TITLEBAR_H;
            if (nw < MIN_WIN_W) nw = MIN_WIN_W;
            if (nh < MIN_WIN_H) nh = MIN_WIN_H;
            if (nw * nh > VWM_MAX_PIXELS) {
                if ((rz.edge & (RZ_LEFT | RZ_RIGHT)) && nw > MIN_WIN_W)
                    nw = VWM_MAX_PIXELS / nh;
                else if (nh > MIN_WIN_H)
                    nh = VWM_MAX_PIXELS / nw;
            }
            if (nw != win->w || nh != win->h || nx != win->x || ny != win->y) {
                /* ОПТИМИЗАЦИЯ: чистка поверхности + EV_RESIZE только если
                 * реально изменился РАЗМЕР. Раньше любое движение мыши при
                 * ресайзе (даже чисто по позиции) заливало весь буфер окна
                 * и дёргало клиента. */
                int resized = (nw != win->w || nh != win->h);
                win->x = nx; win->y = ny; win->w = nw; win->h = nh;
                if (resized) {
                    /* чистим поверхность под новый stride и просим клиента
                     * перерисоваться (EV_RESIZE) */
                    int total = nw * nh;
                    for (int j = 0; j < total; j++) win->pixels[j] = 0xFF2A2A3E;
                    send_event(win->owner_pid, VWM_EV_RESIZE, win->id,
                               (uint64_t)nw, (uint64_t)nh);
                }
                needs_redraw = 1;
            }
        }
        return;
    }

    if (drag.active) {
        vwin_t *win = find_window(drag.win_id);
        if (win) {
            int ox = win->x, oy = win->y;
            win->x = mouse_x - drag.off_x;
            win->y = mouse_y - drag.off_y;
            if (win->x < 0) win->x = 0;
            if (win->y < 0) win->y = 0;
            if (win->x + win->w > (int)fbw) win->x = (int)fbw - win->w;
            if (win->y + win->h + TITLEBAR_H > (int)fbh)
                win->y = (int)fbh - win->h - TITLEBAR_H;
            if (win->x != ox || win->y != oy)
                win->maximized = 0;   /* реально перетащили — не maximized */
        }
    }

    if (!drag.active) {
        int hit = dock_hit(mouse_x, mouse_y);
        int hov = (hit >= 0) ? hit : -1;
        if (hov != dock_hover) {
            dock_hover = hov;
            if (hov < 0) dock_pressed = 0;
            dock_dirty = 1;
        }
    }

    update_cursor_shape();

    if (drag.active || rz.active) needs_redraw = 1;
    else cursor_moved = 1;
}

static int active_window_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].id) n++;
    return n;
}

static void on_mouse_button(uint8_t buttons) {
    mouse_buttons = buttons;
    int mx = mouse_x, my = mouse_y;

    /* --- Dock поверх окон --- */
    int dh = dock_hit(mx, my);
    if (buttons & 1) {
        if (dh >= 0) {
            dock_hover = dh;
            dock_pressed = 1;
            dock_dirty = 1;
            if (dh == 0 && active_window_count() < MAX_WINDOWS)
                vos_spawn("/bin/vterm");
            return;
        }
        if (dh == -2) return;
    } else {
        if (dock_pressed) { dock_pressed = 0; dock_dirty = 1; }
    }

    /* --- Панель поверх окон: чипы свёрнутых окон --- */
    if (buttons & 1) {
        uint64_t cid;
        if (panel_chip_hit(mx, my, &cid)) {
            vwin_t *win = find_window(cid);
            if (win) restore_window(win);
            return;
        }
        /* пустая панель клик НЕ глотает: окно, затащенное под панель
         * (y=0), иначе стало бы невозможно схватить за титлбар */
    }

    /* --- Иконки рабочего стола (под окнами) --- */
    if (buttons & 1) {
        int di = desk_icon_hit(mx, my);
        if (!point_over_window(mx, my)) {
            if (di >= 0) {
                uint64_t now = vos_uptime();
                int dbl = (di == desk_last_click_idx &&
                           (now - desk_last_click_tick) <= 40);
                desk_last_click_idx = di;
                desk_last_click_tick = now;
                if (desk_selected != di) { desk_selected = di; needs_redraw = 1; }
                if (dbl && active_window_count() < MAX_WINDOWS) {
                    vos_spawn(desk_icons[di].path);
                    desk_last_click_idx = -1;
                }
                return;
            }
            if (desk_selected != -1) { desk_selected = -1; needs_redraw = 1; }
        }
    }

    /* --- Кнопки заголовка --- */
    if (buttons & 1) {
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            vwin_t *win = &windows[i];
            if (!win->id || win->minimized) continue;
            int inside = (mx >= win->x && mx < win->x + win->w &&
                          my >= win->y && my < win->y + win->h + TITLEBAR_H);
            if (!inside) continue;
            int b = win_button_hit(win, mx, my);
            if (b == 0) { close_window(win); return; }     /* 🔴 закрыть    */
            if (b == 1) { minimize_window(win); return; }  /* 🟡 свернуть   */
            if (b == 2) {                                  /* 🟢 развернуть */
                focused_id = win->id;
                toggle_maximize(win);
                raise_window(win->id);   /* win после этого невалиден */
                return;
            }
            break;
        }
    }

    if (buttons & 1) {
        /* фокус — окну под кликом + поднять наверх (raise-on-click) */
        for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
            vwin_t *win = &windows[i];
            if (!win->id || win->minimized) continue;
            if (mx >= win->x && mx < win->x + win->w &&
                my >= win->y && my < win->y + win->h + TITLEBAR_H) {
                focused_id = win->id;
                raise_window(win->id);   /* win после этого невалиден */
                break;
            }
        }
        /* resize за край/угол */
        if (!drag.active && !rz.active) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                vwin_t *win = &windows[i];
                if (!win->id || win->minimized) continue;
                int edge = resize_edge_hit(win, mx, my);
                int owns = (mx >= win->x - RESIZE_BORDER && mx <= win->x + win->w + RESIZE_BORDER &&
                            my >= win->y - RESIZE_BORDER && my <= win->y + win->h + TITLEBAR_H + RESIZE_BORDER);
                if (!owns) continue;
                if (edge) {
                    win->maximized = 0;   /* ручной ресайз снимает maximize */
                    rz.active = 1;
                    rz.win_id = win->id;
                    rz.edge = edge;
                    rz.start_mx = mx; rz.start_my = my;
                    rz.start_x = win->x; rz.start_y = win->y;
                    rz.start_w = win->w; rz.start_h = win->h;
                    rz.rendered_x = win->x; rz.rendered_y = win->y;
                    rz.rendered_w = win->w; rz.rendered_h = win->h;
                    focused_id = win->id;
                    /* клик по рамке может быть ВНЕ bounds окна — цикл фокуса
                     * выше его не поднял, поднимаем здесь (win невалиден после) */
                    raise_window(win->id);
                    needs_redraw = 1;
                    return;
                }
                break;
            }
        }
        /* перетаскивание за title bar */
        if (!drag.active && !rz.active) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                vwin_t *win = &windows[i];
                if (!win->id || win->minimized) continue;
                if (mx >= win->x && mx < win->x + win->w &&
                    my >= win->y && my < win->y + TITLEBAR_H) {
                    /* maximized снимаем не здесь, а при реальном движении
                     * в on_mouse_move — простой клик по титлбару не должен
                     * сбрасывать состояние тоггла 🟢 */
                    drag.active = 1;
                    drag.win_id = win->id;
                    drag.off_x = mx - win->x;
                    drag.off_y = my - win->y;
                    drag.rendered_x = win->x;
                    drag.rendered_y = win->y;
                    break;
                }
            }
        }
    } else {
        drag.active = 0;
        rz.active = 0;
        update_cursor_shape();   /* отпустили кнопку — форма по тому, что под курсором */
    }

    needs_redraw = 1;
}

static void on_key(char ascii, int pressed) {
    if (!ascii || !pressed) return;
    vwin_t *win = find_window(focused_id);
    if (!win) return;
    send_event(win->owner_pid, VWM_EV_KEY, win->id, (uint64_t)(uint8_t)ascii, 1);
}

/* ---------------------------------------------------------------------------
 * Сообщения клиентов
 * ------------------------------------------------------------------------- */
static void on_create(vos_msg_t *m) {
    uint64_t sender = m->w[7];
    int w = (int)(m->w[1] >> 32);
    int h = (int)(m->w[1] & 0xFFFFFFFFu);
    uint64_t shm_id = m->w[2];

    vwin_t *win = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].id) { win = &windows[i]; break; }
    if (!win || w <= 0 || h <= 0 || w * h > VWM_MAX_PIXELS) {
        send_event(sender, VWM_CREATED, 0, 0, 0);
        return;
    }

    uint32_t *pixels = (uint32_t *)vos_shm_map(shm_id);
    if (!pixels) {
        send_event(sender, VWM_CREATED, 0, 0, 0);
        return;
    }

    win->id = next_win_id++;
    win->owner_pid = sender;
    win->w = w; win->h = h;
    win->pixels = pixels;
    win->shm_id = shm_id;
    /* каскад: каждое следующее окно чуть ниже и правее */
    {
        int n = active_window_count() - 1;
        win->x = 80 + (n % 5) * 48;
        win->y = 60 + (n % 5) * 40;
        if (win->x + win->w > (int)fbw) win->x = imax(0, (int)fbw - win->w - 8);
        if (win->y + win->h + TITLEBAR_H > (int)fbh)
            win->y = imax(PANEL_H, (int)fbh - win->h - TITLEBAR_H - 8);
    }
    const char *t = (const char *)&m->w[3];
    int i = 0;
    while (t[i] && i < 31) { win->title[i] = t[i]; i++; }
    win->title[i] = 0;

    focused_id = win->id;
    send_event(sender, VWM_CREATED, win->id, shm_id, 0);
    /* Слоты переиспользуются после close — без raise новое окно могло бы
     * родиться ПОД существующими (низкий индекс = низ z-порядка). */
    raise_window(win->id);
    needs_redraw = 1;
}

static void handle_msg(vos_msg_t *m) {
    switch (m->w[0]) {
    case VIN_MOUSE: {
        int dx = (int)(int64_t)m->w[1];
        int dy = (int)(int64_t)m->w[2];
        uint8_t buttons = (uint8_t)m->w[3];
        int changed = (int)m->w[4];
        if (dx || dy) on_mouse_move(dx, dy);
        if (changed) on_mouse_button(buttons);
        break;
    }
    case VIN_KEY:
        on_key((char)m->w[1], (int)m->w[2]);
        break;
    case VWM_CREATE:
        on_create(m);
        break;
    case VWM_DESTROY: {
        vwin_t *win = find_window(m->w[1]);
        if (win && win->owner_pid == m->w[7])
            win_drop(win);   /* в т.ч. отдаёт shm-поверхность */
        break;
    }
    case VWM_COMMIT: {
        vwin_t *win = find_window(m->w[1]);
        if (win && win->owner_pid == m->w[7]) {
            /* Пиксели уже в shm — просто перерисовываем область окна.
             * Если сцена и так грязная, кадр по тику всё перерисует. */
            if (win->minimized)
                ;   /* свёрнуто: пиксели в shm обновились, рисовать нечего */
            else if (!needs_redraw && !drag.active && !rz.active)
                render_window_region(win);
            else
                needs_redraw = 1;
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
void _start(void) {
    /* 1. Экран */
    struct { uint64_t phys; uint32_t w, h, pitch, bpp; } info;
    syscall1(SYS_FB_INFO, (uint64_t)&info);
    fb = (uint32_t *)syscall0(SYS_FB_MAP);
    if (!fb || !info.w || !info.h) {
        puts("vwm: no framebuffer\n");
        exit(1);
    }
    fbw = info.w; fbh = info.h; fb_stride = info.pitch / 4;

    /* 2. Back buffer в shm (PMM-страницы, не куча ядра) */
    uint64_t bb_shm = vos_shm_create((uint64_t)fbw * fbh * 4);
    if (bb_shm == (uint64_t)-1) {
        puts("vwm: shm_create back buffer failed\n");
        exit(1);
    }
    bb = (uint32_t *)vos_shm_map(bb_shm);
    if (!bb) {
        puts("vwm: shm_map back buffer failed\n");
        exit(1);
    }

    /* 3. Становимся WM: сервис + весь ввод наш */
    shadow_lut_init();
    vos_svc_register(VOS_SVC_WM);
    vos_input_grab();

    mouse_x = (int)fbw / 2;
    mouse_y = (int)fbh / 2;

    puts("vwm: userspace window manager up\n");

    /* 4. Первый кадр + стартовый терминал */
    render_all();
    vos_spawn("/bin/vterm");

    /* 5. Event loop: ipc_recv — и очередь событий, и таймер кадра.
     * Спим максимум 1 тик; кадр рисуем не чаще раза в 2 тика (~50 FPS). */
    uint64_t last_frame = 0;
    vos_msg_t m;
    for (;;) {
        int got = (int)vos_ipc_recv(&m, 1);
        while (got) {
            handle_msg(&m);
            got = (int)vos_ipc_recv(&m, VOS_IPC_NOWAIT);  /* выгребаем всё */
        }
        /* Во время drag/resize рисуем каждый тик (до ~100 FPS): кадр после
         * оптимизаций дешёвый, а плавность перетаскивания решает. В покое —
         * раз в 2 тика, как раньше. */
        uint64_t now = vos_uptime();
        uint64_t interval = (drag.active || rz.active) ? 1 : 2;
        if (now - last_frame >= interval) {
            tick_render();
            last_frame = now;
        }
    }
}
