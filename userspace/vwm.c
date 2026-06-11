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
 *   - декорации (заголовок, кнопки, тень, скругления), панель, dock с
 *     ярлыками приложений, курсор, drag/resize, фокус — всё здесь, в ring3;
 *     рабочий стол чистый (под будущие виджеты/ярлыки пользователя);
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
#define TITLEBAR_H    26
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

#define DESK_BG       0xFF12121Eu  /* fallback-фон, если обои не выделились */
#define PANEL_H       28
#define ACCENT        0xFF5B8CFFu  /* акцент темы: рамка фокуса, панель, выделение */

#define MAX_WINDOWS   16   /* окон одновременно; держать <= SHM_MAX_SEGS-2 в ядре
                            * (2 сегмента ест сам vwm: back buffer + обои) */

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
 * Глобальный clip rect — как в настоящих композиторах (Hyprland/KWin/pixman):
 * при перекомпозиции damage-региона ВСЕ примитивы режутся по нему. Это
 * убирает класс багов «нарисовали поверх несвежего» (двойной блендинг тени
 * дока, затирание окон полосой панели и т.п.) и заодно ускоряет частичные
 * кадры: chrome окна за пределами региона не рисуется вовсе.
 * ------------------------------------------------------------------------- */
static int clip_x0, clip_y0, clip_x1, clip_y1;   /* [x0,x1) x [y0,y1) */

static inline void clip_reset(void) {
    clip_x0 = 0; clip_y0 = 0; clip_x1 = (int)fbw; clip_y1 = (int)fbh;
}
static inline void clip_set(int x, int y, int w, int h) {
    clip_x0 = imax(0, x);
    clip_y0 = imax(0, y);
    clip_x1 = imin((int)fbw, x + w);
    clip_y1 = imin((int)fbh, y + h);
}
static inline int clip_empty(void) {
    return clip_x0 >= clip_x1 || clip_y0 >= clip_y1;
}

/* ---------------------------------------------------------------------------
 * Примитивы на back buffer (порт kernel compositor.c) — все с учётом клипа
 * ------------------------------------------------------------------------- */
static inline void put_px(int x, int y, uint32_t c) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    bb[(uint32_t)y * fbw + x] = c;
}
static inline uint32_t get_px(int x, int y) {
    if (x < 0 || x >= (int)fbw || y < 0 || y >= (int)fbh) return 0;
    return bb[(uint32_t)y * fbw + x];
}
static void blend_px(int x, int y, uint32_t argb) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
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
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = &bb[(uint32_t)(y + j) * fbw + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}
/* Покрытие пикселя кругом (4x4 суперсэмплинг) — базовый кирпич всего AA.
 * Раньше жил ниже у оконного chrome; теперь им сглаживаются ВСЕ круги и
 * скругления (кнопки, dock, чипы, иконки) — пиксельная «лесенка» ушла. */
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
static void fill_circle(int cx, int cy, int r, uint32_t c) {
    uint32_t rgb = c & 0x00FFFFFF;
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int cov = circ_cov(cx + dx, cy + dy, cx, cy, r);
            if (cov <= 0) continue;
            if (cov >= 255) put_px(cx + dx, cy + dy, c);
            else blend_px(cx + dx, cy + dy, ((uint32_t)cov << 24) | rgb);
        }
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
/* Текст БЕЗ фона (только пиксели глифа) — для градиентов и обоев, где
 * прямоугольная подложка под буквами выглядела бы коробкой. */
static void draw_char_t(int x, int y, char ch, uint32_t fg) {
    uint8_t idx = (uint8_t)ch;
    if (idx >= 128) idx = '?';
    const unsigned char *glyph = vos_font[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col)) put_px(x + col, y + row, fg);
    }
}
static void draw_string_t(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; }
        else { draw_char_t(cx, y, *s, fg); cx += 8; }
        s++;
    }
}
/* Текст с мягкой тенью — читаемость на любых обоях (метки иконок стола). */
static void draw_string_sh(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 16; s++; continue; }
        uint8_t idx = (uint8_t)*s;
        if (idx >= 128) idx = '?';
        const unsigned char *glyph = vos_font[idx];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    blend_px(cx + col + 1, y + row + 1, 0xA0000000u);
        }
        draw_char_t(cx, y, *s, fg);
        cx += 8;
        s++;
    }
}
/* Блит поверхности окна целыми строками (stride задаёт вызывающий). */
static void blit_buffer(int dx, int dy, int w, int h, const uint32_t *src) {
    if (!src) return;
    for (int row = 0; row < h; row++) {
        int y = dy + row;
        if (y < clip_y0 || y >= clip_y1) continue;
        int x0 = dx, sx0 = 0, ww = w;
        if (x0 < clip_x0) { sx0 = clip_x0 - x0; ww -= sx0; x0 = clip_x0; }
        if (x0 + ww > clip_x1) ww = clip_x1 - x0;
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
/* ---------------------------------------------------------------------------
 * Курсор — больше НЕ save-under. Как в настоящих композиторах: курсор — это
 * просто верхний слой сцены, который рисуется заново в каждом кадре поверх
 * перекомпонованных damage-регионов. Save-under ловил «призраков», когда его
 * сохранённый кусок фона устаревал (сцена под курсором перерисована не через
 * take_down) — целый класс багов уходит вместе с механизмом.
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

/* где курсор «запечён» в back buffer после последнего кадра */
static int last_cx, last_cy, last_cw, last_ch, last_cur_valid = 0;

static void cursor_sprite(const cur_shape_t *s, int x, int y) {
    for (int j = 0; j < s->h; j++) {
        const char *row = s->rows[j];
        for (int i = 0; i < s->w; i++) {
            if (row[i] == 'X')      put_px(x + i, y + j, 0xFF000000);
            else if (row[i] == '.') put_px(x + i, y + j, 0xFFFFFFFF);
        }
    }
}
static void cursor_rect(int *x, int *y, int *w, int *h) {
    const cur_shape_t *s = &cur_shapes[cur_shape];
    *x = mouse_x - s->hx; *y = mouse_y - s->hy;
    *w = s->w; *h = s->h;
}
/* Курсор сдвинулся/сменил форму: повредить старое место (стереть запечённый
 * спрайт) и новое (нарисовать там). Сам рендер сделает frame(). */
static void dmg_cursor(void) {
    if (last_cur_valid) dmg_add(last_cx, last_cy, last_cw, last_ch);
    int x, y, w, h;
    cursor_rect(&x, &y, &w, &h);
    dmg_add(x, y, w, h);
}

/* ---------------------------------------------------------------------------
 * Обои: «aurora» — диагональный градиент + два мягких свечения + ordered
 * dithering (Bayer 4x4), чтобы на 8-битных каналах не было полос. Рендерятся
 * ОДИН раз при старте в отдельный shm-буфер; дальше заливка фона — это
 * копирование строк, т.е. так же дёшево, как старый fill_rect одним цветом.
 * ------------------------------------------------------------------------- */
static uint32_t *wall;   /* 0 => обои не выделились, fallback DESK_BG */

static const uint8_t bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static void wall_render(void) {
    /* база: тёмно-синий (верх-лево) -> фиолетовый (низ-право), fixed <<8 */
    const int r0 = 0x0E, g0 = 0x11, b0 = 0x24;
    const int r1 = 0x2C, g1 = 0x16, b1 = 0x46;
    /* свечения: голубое возле верх-право, бирюзовое возле низ-лево */
    int gx1 = (int)fbw * 82 / 100, gy1 = (int)fbh * 6 / 100;
    int gx2 = (int)fbw * 10 / 100, gy2 = (int)fbh * 96 / 100;
    int R1 = (int)fbw * 55 / 100, R1sq = R1 * R1;
    int R2 = (int)fbw * 42 / 100, R2sq = R2 * R2;
    for (int y = 0; y < (int)fbh; y++) {
        for (int x = 0; x < (int)fbw; x++) {
            int t = x * 128 / ((int)fbw - 1) + y * 128 / ((int)fbh - 1);
            int rf = r0 * (256 - t) + r1 * t;       /* value <<8 */
            int gf = g0 * (256 - t) + g1 * t;
            int bf = b0 * (256 - t) + b1 * t;

            int dx = x - gx1, dy = y - gy1;
            int d2 = dx * dx + dy * dy;
            if (d2 < R1sq) {
                int a = (int)(((int64_t)(R1sq - d2) << 8) / R1sq);  /* 0..256 */
                a = a * a >> 8;                     /* мягче к краю */
                rf += 0x10 * a; gf += 0x26 * a; bf += 0x5E * a;
            }
            dx = x - gx2; dy = y - gy2;
            d2 = dx * dx + dy * dy;
            if (d2 < R2sq) {
                int a = (int)(((int64_t)(R2sq - d2) << 8) / R2sq);
                a = a * a >> 8;
                rf += 0x06 * a; gf += 0x30 * a; bf += 0x2E * a;
            }

            int dth = bayer4[y & 3][x & 3] * 16;    /* 0..240 как дробь <<8 */
            int r = (rf + dth) >> 8; if (r > 255) r = 255;
            int g = (gf + dth) >> 8; if (g > 255) g = 255;
            int b = (bf + dth) >> 8; if (b > 255) b = 255;
            wall[(uint32_t)y * fbw + x] =
                0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* Заливка региона обоями (клип + построчное копирование). */
static void fill_wall(int x, int y, int w, int h) {
    if (!wall) { fill_rect(x, y, w, h, DESK_BG); return; }
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        copy_px_row(&bb[(uint32_t)(y + j) * fbw + x],
                    &wall[(uint32_t)(y + j) * fbw + x], w);
}

/* ---------------------------------------------------------------------------
 * Dock (порт kernel simple_wm: «пилюля» в стиле macOS, терминал /vterm)
 * ------------------------------------------------------------------------- */
#define DOCK_ICON   48
#define DOCK_PAD    12
#define DOCK_GAP    12
#define DOCK_BOTTOM 16

/* Ярлыки приложений (бывшие иконки рабочего стола — стол оставляем чистым
 * под будущие пользовательские виджеты/папки/ярлыки, как в macOS). */
typedef struct { const char *path; const char *label; int kind; } dock_item_t;
static const dock_item_t dock_items[] = {
    { "/bin/vterm",  "Terminal", 0 },
    { "/bin/vfiles", "Files",    1 },
    { "/bin/vdemo",  "Window",   2 },
};
#define DOCK_NITEMS ((int)(sizeof(dock_items) / sizeof(dock_items[0])))

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
/* AA-скруглённый прямоугольник: углы сглаживаются покрытием circ_cov —
 * никакой «лесенки». Итоговая альфа пикселя = a * cov / 255. */
static void fill_round(int x, int y, int w, int h, int r, uint32_t color, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    uint32_t rgb = color & 0x00FFFFFF;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int cx = -1, cy = -1;
            if (i < r)           cx = r;
            else if (i >= w - r) cx = w - r;
            if (j < r)           cy = r;
            else if (j >= h - r) cy = h - r;
            int cov = 255;
            if (cx >= 0 && cy >= 0) cov = circ_cov(i, j, cx, cy, r);
            if (cov <= 0) continue;
            int aa = a * cov / 255;
            if (aa >= 255) put_px(x + i, y + j, 0xFF000000u | rgb);
            else blend_px(x + i, y + j, ((uint32_t)aa << 24) | rgb);
        }
    }
}
static void dock_draw_tile(int kind, int x, int y, int s, int pressed) {
    if (pressed) { x += 1; y += 1; }
    if (kind == 0) {                       /* Terminal */
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
    } else if (kind == 1) {                /* Files: папка */
        fill_round(x, y, s, s, 11, 0xFF1E2433, 255);
        for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);
        uint32_t fold = 0xFFE8B64C, foldhi = 0xFFF2CC74;
        fill_rect(x + 8,  y + 14, 14, 5,  fold);    /* язычок */
        fill_rect(x + 8,  y + 18, 32, 18, fold);    /* корпус */
        fill_rect(x + 8,  y + 18, 32, 3,  foldhi);  /* блик   */
    } else {                               /* Window: демо-окно */
        fill_round(x, y, s, s, 11, 0xFFEAEAF0, 255);
        fill_rect(x + 8,  y + 10, 32, 28, 0xFFFFFFFF);
        fill_rect(x + 8,  y + 10, 32, 7,  0xFF3D6FB5);
        fill_rect(x + 12, y + 22, 24, 2,  0xFFB8C2D0);
        fill_rect(x + 12, y + 27, 24, 2,  0xFFB8C2D0);
        fill_rect(x + 12, y + 32, 16, 2,  0xFFB8C2D0);
    }
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
        dock_draw_tile(dock_items[k].kind, ix, iy, DOCK_ICON, pressed);
    }
    if (dock_hover >= 0 && dock_hover < DOCK_NITEMS) {   /* тултип-подпись */
        const char *t = dock_items[dock_hover].label;
        int len = 0; while (t[len]) len++;
        int tw = len * 8 + 14, th = 22;
        int ix, iy; dock_icon_rect(dock_hover, &ix, &iy);
        (void)iy;
        int tx = ix + DOCK_ICON / 2 - tw / 2;
        int ty = dy - th - 8;
        fill_round(tx, ty + 2, tw, th, th / 2, 0xFF000000, 60);
        fill_round(tx, ty, tw, th, th / 2, 0xFF22222F, 230);
        draw_string_t(tx + 7, ty + (th - 16) / 2, t, 0xFFF0F2F8);
    }
}
static void dock_bounds(int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    *x = dx - 4; *y = dy - 34; *w = dw + 8; *h = dh + 42;   /* + тултип сверху */
}

/* ---------------------------------------------------------------------------
 * Верхняя панель — теперь отдельный процесс /bin/vpanel (логотип, заголовок,
 * чипы-таскбар, часы). Здесь только композит его shm-поверхности поверх
 * обоев per-pixel alpha + маршрутизация кликов и списка окон (VWM_PANEL_*).
 * Это НЕ окно: без титлбара, фокуса и z-порядка. Если vpanel мёртв/не
 * поднялся — рисуем пустую полупрозрачную плашку (fallback).
 * ------------------------------------------------------------------------- */
static uint64_t  panel_pid = 0;     /* pid процесса vpanel (после ATTACH) */
static uint32_t *panel_surf = 0;    /* его поверхность fbw x PANEL_H */
static uint64_t  panel_shm = (uint64_t)-1;

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

/* Слепок списка окон для vpanel: по одному сообщению на окно
 * (id, idx/count, flags, title 23+0). count=0 -> одно msg с w1=0.
 * Зовём при любом изменении набора/фокуса/заголовков — дёшево (<=16 msgs). */
static void panel_send_wins(void) {
    if (!panel_pid) return;
    vos_msg_t m;
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].id) count++;
    if (count == 0) {
        for (int k = 0; k < 8; k++) m.w[k] = 0;
        m.w[0] = VWM_PANEL_WINS;
        vos_ipc_send(panel_pid, &m);
        return;
    }
    int idx = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *w = &windows[i];
        if (!w->id) continue;
        for (int k = 0; k < 8; k++) m.w[k] = 0;
        m.w[0] = VWM_PANEL_WINS;
        m.w[1] = w->id;
        m.w[2] = ((uint64_t)idx << 32) | (uint64_t)count;
        m.w[3] = (w->minimized ? 1u : 0u) | ((w->id == focused_id) ? 2u : 0u);
        char *t = (char *)&m.w[4];
        int n = 0;
        while (w->title[n] && n < 23) { t[n] = w->title[n]; n++; }
        t[n] = 0;
        vos_ipc_send(panel_pid, &m);
        idx++;
    }
}

static void draw_panel(void) {
    int W = (int)fbw;
    fill_wall(0, 0, W, PANEL_H);
    if (panel_surf) {
        /* per-pixel alpha-blend поверхности vpanel поверх обоев: полоса
         * 28px, перерисовывается редко — дёшево (как старая плашка). */
        for (int j = 0; j < PANEL_H; j++) {
            const uint32_t *row = &panel_surf[(uint32_t)j * W];
            for (int i = 0; i < W; i++) {
                uint32_t c = row[i];
                if (c >> 24) blend_px(i, j, c);
            }
        }
        return;
    }
    /* fallback: vpanel ещё не поднялся (или умер) — плашка без контента */
    for (int j = 0; j < PANEL_H - 1; j++)
        for (int i = 0; i < W; i++)
            blend_px(i, j, 0xD20D0E15u);
    for (int i = 0; i < W; i++)
        blend_px(i, PANEL_H - 1, 0xB0000000u | (ACCENT & 0x00FFFFFF));
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
/* Титлбар: вертикальный градиент ctop->cbot + AA-скругление верхних углов. */
static void fill_round_top(int x, int y, int w, int h, int r,
                           uint32_t ctop, uint32_t cbot) {
    if (r * 2 > w) r = w / 2;
    if (r > h) r = h;
    int rt = (ctop >> 16) & 0xFF, gt = (ctop >> 8) & 0xFF, bt = ctop & 0xFF;
    int rB = (cbot >> 16) & 0xFF, gB = (cbot >> 8) & 0xFF, bB = cbot & 0xFF;
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? j * 256 / (h - 1) : 0;
        uint32_t cr = (uint32_t)((rt * (256 - t) + rB * t) >> 8);
        uint32_t cg = (uint32_t)((gt * (256 - t) + gB * t) >> 8);
        uint32_t cb = (uint32_t)((bt * (256 - t) + bB * t) >> 8);
        uint32_t color = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
        uint32_t rgb = color & 0x00FFFFFF;
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

    if (focused)
        fill_round_top(win->x, win->y, win->w, TITLEBAR_H, WIN_CORNER,
                       0xFF434A6A, 0xFF2D3148);
    else
        fill_round_top(win->x, win->y, win->w, TITLEBAR_H, WIN_CORNER,
                       0xFF2B2D3C, 0xFF222330);
    for (int i = WIN_CORNER; i < win->w - WIN_CORNER; i++)
        blend_px(win->x + i, win->y, 0x30FFFFFFu);

    uint32_t cclose = focused ? 0xFFFF5F56 : 0xFF565664;
    uint32_t cmin   = focused ? 0xFFFFBD2E : 0xFF565664;
    uint32_t cmax   = focused ? 0xFF27C93F : 0xFF565664;
    int cy = win->y + TITLEBAR_H / 2;
    int bx = win->x + BTN_X0;
    fill_circle(bx,               cy, BTN_R, cclose);
    fill_circle(bx + BTN_GAP,     cy, BTN_R, cmin);
    fill_circle(bx + 2 * BTN_GAP, cy, BTN_R, cmax);
    if (focused) {
        /* глифы на светофорах (×  –  +) — тёмные поверх цвета кнопки */
        const uint32_t g = 0x96000000u;
        for (int d = -2; d <= 2; d++) {
            blend_px(bx + d, cy + d, g);            /* ×  close    */
            if (d) blend_px(bx + d, cy - d, g);
            blend_px(bx + BTN_GAP + d, cy, g);      /* –  minimize */
            blend_px(bx + 2 * BTN_GAP + d, cy, g);  /* +  maximize */
            if (d) blend_px(bx + 2 * BTN_GAP, cy + d, g);
        }
    }

    int tx = win->x + BTN_X0 + 2 * BTN_GAP + BTN_R + 10;
    draw_string_t(tx, win->y + (TITLEBAR_H - 16) / 2, win->title,
                  focused ? 0xFFEDEFF6 : 0xFF9A9DB0);

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

    /* активное окно очерчено акцентом (как в Plasma), остальные — нейтрально */
    uint32_t bord = focused ? ACCENT : 0xFF363642u;
    draw_round_border(win->x, win->y, win->w, fh, WIN_CORNER, bord);
}

/* ---------------------------------------------------------------------------
 * Рендер «как у взрослых» (схема Hyprland/KWin в миниатюре):
 *   1) события копят damage-прямоугольники (dmg_add / dmg_all / dmg_cursor);
 *   2) frame() раз за тик ПЕРЕКОМПОНУЕТ сцену строго внутри каждого
 *      damage-прямоугольника (clip rect режет все примитивы) в фиксированном
 *      z-порядке: обои -> окна -> панель -> dock;
 *   3) курсор рисуется ПОСЛЕДНИМ слоем поверх затронутых регионов;
 *   4) один vsync + блит damage-регионов во front buffer.
 * Никаких save-under и неклипованных перерисовок «соседей» — артефактам
 * физически неоткуда взяться: каждый пиксель кадра собран с нуля.
 * ------------------------------------------------------------------------- */
static int win_intersects(const vwin_t *win, int rx, int ry, int rw, int rh) {
    int m = WIN_MARGIN;
    int wx = win->x - m, wy = win->y - m;
    int ww = win->w + 2 * m, wh = win->h + TITLEBAR_H + 2 * m;
    if (wx >= rx + rw || wx + ww <= rx) return 0;
    if (wy >= ry + rh || wy + wh <= ry) return 0;
    return 1;
}

/* Пересобрать сцену (без курсора) внутри прямоугольника. */
static void compose_rect(int rx, int ry, int rw, int rh) {
    clip_set(rx, ry, rw, rh);
    if (clip_empty()) { clip_reset(); return; }

    fill_wall(rx, ry, rw, rh);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id || !win->pixels || win->minimized) continue;
        if (!win_intersects(win, rx, ry, rw, rh)) continue;
        draw_window_chrome(win);
    }
    {
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        if (!(rx >= px + pw || rx + rw <= px || ry >= py + ph || ry + rh <= py))
            draw_panel();
    }
    {
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        if (!(rx >= bx + bw || rx + rw <= bx || ry >= by + bh || ry + rh <= by))
            draw_dock();
    }
    clip_reset();
}

/* Кадр: перекомпоновка damage-регионов + курсор + один present. */
static void frame(void) {
    if (!scene_presented) { scene_presented = 1; dmg_all(); }
    if (!damage_full && damage_count == 0) return;

    if (damage_full) {
        damage[0].x = 0; damage[0].y = 0;
        damage[0].w = (int)fbw; damage[0].h = (int)fbh;
        damage_count = 1;
        damage_full = 0;
    }

    for (int i = 0; i < damage_count; i++)
        compose_rect(damage[i].x, damage[i].y, damage[i].w, damage[i].h);

    /* курсор — верхний слой: дорисовать в каждый затронутый регион */
    {
        int cx, cy, cw, ch;
        cursor_rect(&cx, &cy, &cw, &ch);
        const cur_shape_t *s = &cur_shapes[cur_shape];
        for (int i = 0; i < damage_count; i++) {
            const rect_t *r = &damage[i];
            if (cx >= r->x + r->w || cx + cw <= r->x ||
                cy >= r->y + r->h || cy + ch <= r->y) continue;
            clip_set(r->x, r->y, r->w, r->h);
            cursor_sprite(s, cx, cy);
            clip_reset();
        }
        last_cx = cx; last_cy = cy; last_cw = cw; last_ch = ch;
        last_cur_valid = 1;
    }

    vos_vsync();                 /* Limine-путь: ждём vblank (no-op на virtio) */
    for (int i = 0; i < damage_count; i++) {
        blit_to_front(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
        vos_fb_present(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
    }
    dmg_reset();
}

static void render_all(void) {
    dmg_all();
    frame();
    cursor_moved = 0;
    if (drag.active) {
        vwin_t *dw = find_window(drag.win_id);
        if (dw) { drag.rendered_x = dw->x; drag.rendered_y = dw->y; }
    }
}
static void render_region(int rx, int ry, int rw, int rh) {
    if (rw <= 0 || rh <= 0) return;
    dmg_add(rx, ry, rw, rh);
    if (cursor_moved) { cursor_moved = 0; dmg_cursor(); }
    frame();
}

/* Кадр по тику (порт wm_tick_render): выбирает самый дешёвый путь. */
static uint64_t last_panel_sec = 0;

static void tick_render(void) {
    uint64_t sec = vos_uptime() / 100;
    if (sec != last_panel_sec) { last_panel_sec = sec; panel_dirty = 1; }

    if (needs_redraw) {
        needs_redraw = 0;
        int handled = 0;
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
                dmg_add(minx - m, miny - m,
                        (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                rz.rendered_x = nx; rz.rendered_y = ny;
                rz.rendered_w = win->w; rz.rendered_h = win->h;
                handled = 1;
            }
        } else if (drag.active) {
            vwin_t *win = find_window(drag.win_id);
            if (win) {
                int ox = drag.rendered_x, oy = drag.rendered_y;
                int nx = win->x,          ny = win->y;
                int fh = win->h + TITLEBAR_H;
                int minx = imin(ox, nx), miny = imin(oy, ny);
                int maxx = imax(ox + win->w, nx + win->w);
                int maxy = imax(oy + fh,     ny + fh);
                int m = WIN_MARGIN;
                dmg_add(minx - m, miny - m,
                        (maxx - minx) + 2 * m, (maxy - miny) + 2 * m);
                drag.rendered_x = nx; drag.rendered_y = ny;
                handled = 1;
            }
        }
        if (!handled) {
            dmg_all();
            panel_dirty = 0;
            dock_dirty = 0;
        }
    }
    if (panel_dirty) {
        panel_dirty = 0;
        int px, py, pw, ph;
        panel_bounds(&px, &py, &pw, &ph);
        dmg_add(px, py, pw, ph);
    }
    if (dock_dirty) {
        dock_dirty = 0;
        int bx, by, bw, bh;
        dock_bounds(&bx, &by, &bw, &bh);
        dmg_add(bx, by, bw, bh);
    }
    if (cursor_moved) {
        cursor_moved = 0;
        dmg_cursor();
    }
    frame();
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

static void send_event4(uint64_t pid, uint64_t type,
                        uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = type; m.w[1] = a; m.w[2] = b; m.w[3] = c; m.w[4] = d;
    vos_ipc_send(pid, &m);
}

/* Кому отдали нажатие ЛКМ в содержимое окна (win_id, не указатель! —
 * cross-frame состояние только по id). Отпускание шлём этому же окну. */
static uint64_t mouse_press_win = 0;

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
    if (mouse_press_win == win->id) mouse_press_win = 0;
    win->id = 0;
    win->pixels = 0;
    win->minimized = 0;   /* слот переиспользуется — флаги не наследуем */
    win->maximized = 0;
    /* Безусловно: у каждого окна есть поверхность, а shm_id == 0 — валидный
     * id сегмента (нумерация с нуля). */
    vos_shm_release(win->shm_id);
    win->shm_id = 0;
    needs_redraw = 1;
    panel_send_wins();
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
    panel_send_wins();
}

/* Развернуть из панели: показать, дать фокус, поднять наверх. */
static void restore_window(vwin_t *win) {
    uint64_t id = win->id;
    win->minimized = 0;
    focused_id = id;
    raise_window(id);                   /* win после этого невалиден */
    needs_redraw = 1;
    panel_send_wins();
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

    /* Курсор повреждаем ВСЕГДА (старое+новое место — копейки), а при
     * drag/resize дополнительно перерисовываем геометрию окна. */
    cursor_moved = 1;
    if (drag.active || rz.active) needs_redraw = 1;
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
            if (active_window_count() < MAX_WINDOWS)
                vos_spawn(dock_items[dh].path);
            return;
        }
        if (dh == -2) return;
    } else {
        if (dock_pressed) { dock_pressed = 0; dock_dirty = 1; }
    }

    /* --- Панель: клик форвардим vpanel'у (чипы-таскбар там).  Клик НЕ
     * глотаем: окно, затащенное под панель (y=0), остаётся доступным за
     * титлбар. Редкий конфликт "чип против титлбара под ним" принят для
     * v1 — vpanel разрулит активацией поверх. --- */
    if ((buttons & 1) && my < PANEL_H && panel_pid) {
        vos_msg_t pm;
        for (int k = 0; k < 8; k++) pm.w[k] = 0;
        pm.w[0] = VWM_PANEL_CLICK;
        pm.w[1] = (uint64_t)mx;
        pm.w[2] = (uint64_t)my;
        pm.w[3] = (uint64_t)buttons;
        vos_ipc_send(panel_pid, &pm);
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
        /* клик в СОДЕРЖИМОЕ окна — отдаём приложению (VWM_EV_MOUSE).
         * Сюда доходим, только если клик не съели dock/панель/иконки/кнопки,
         * не начался resize и не drag. После raise-on-click кликнутое окно —
         * верхнее под курсором, ищем его заново (win после raise невалиден). */
        if (!drag.active && !rz.active) {
            for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                vwin_t *win = &windows[i];
                if (!win->id || win->minimized) continue;
                if (mx < win->x || mx >= win->x + win->w ||
                    my < win->y || my >= win->y + win->h + TITLEBAR_H) continue;
                if (my >= win->y + TITLEBAR_H) {
                    send_event4(win->owner_pid, VWM_EV_MOUSE, win->id,
                                (uint64_t)(mx - win->x),
                                (uint64_t)(my - win->y - TITLEBAR_H), 1);
                    mouse_press_win = win->id;
                }
                break;
            }
        }
    } else {
        drag.active = 0;
        rz.active = 0;
        /* отпускание ЛКМ — тому окну, которому отдали нажатие */
        if (mouse_press_win) {
            vwin_t *win = find_window(mouse_press_win);
            if (win && !win->minimized)
                send_event4(win->owner_pid, VWM_EV_MOUSE, win->id,
                            (uint64_t)(mx - win->x),
                            (uint64_t)(my - win->y - TITLEBAR_H), 0);
            mouse_press_win = 0;
        }
        update_cursor_shape();   /* отпустили кнопку — форма по тому, что под курсором */
    }

    /* клик мог сменить фокус/закрыть/свернуть — освежаем таскбар vpanel
     * (часть путей уже слала список; дубль безвреден, vpanel перерисуется) */
    if (buttons & 1) panel_send_wins();

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
    panel_send_wins();
}

/* --- Панель: ATTACH/COMMIT/ACTIVATE от /bin/vpanel --- */
static void on_panel_attach(vos_msg_t *m) {
    uint64_t sender = m->w[7];
    uint64_t shm_id = m->w[2];
    uint32_t *pixels = (uint32_t *)vos_shm_map(shm_id);

    vos_msg_t r;
    for (int k = 0; k < 8; k++) r.w[k] = 0;
    r.w[0] = VWM_PANEL_OK;
    if (!pixels) {
        vos_ipc_send(sender, &r);          /* w1=0 — отказ */
        return;
    }
    /* рестарт vpanel: отпустить поверхность предыдущего */
    if (panel_surf && panel_shm != (uint64_t)-1)
        vos_shm_release(panel_shm);
    panel_surf = pixels;
    panel_shm = shm_id;
    panel_pid = sender;
    r.w[1] = ((uint64_t)fbw << 32) | (uint64_t)PANEL_H;
    vos_ipc_send(sender, &r);
    panel_send_wins();
    panel_dirty = 1;
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
    case VWM_PANEL_ATTACH:
        on_panel_attach(m);
        break;
    case VWM_PANEL_COMMIT:
        if (m->w[7] == panel_pid) panel_dirty = 1;
        break;
    case VWM_PANEL_ACTIVATE:
        if (m->w[7] == panel_pid) {
            vwin_t *win = find_window(m->w[1]);
            if (win) {
                if (win->minimized) restore_window(win);
                else {
                    focused_id = win->id;
                    raise_window(win->id);   /* win невалиден дальше */
                    panel_send_wins();
                }
            }
        }
        break;
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

    /* 2b. Обои: ещё один shm-сегмент, рендерим один раз. Не выделились —
     * не страшно: fill_wall откатится на плоский DESK_BG. */
    uint64_t wall_shm = vos_shm_create((uint64_t)fbw * fbh * 4);
    if (wall_shm != (uint64_t)-1) {
        wall = (uint32_t *)vos_shm_map(wall_shm);
        if (wall) wall_render();
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
    vos_spawn("/bin/vpanel");
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
