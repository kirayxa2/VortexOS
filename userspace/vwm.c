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
#include "vfont_ui.h"

/* vwm — standalone-бинарь без libc. GCC при копии больших структур
 * (vwin_t с 8 KB title-cache) генерирует вызов memcpy — подсовываем свой. */
void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ---------------------------------------------------------------------------
 * Геометрия и палитра (соответствуют kernel simple_wm, чтобы вид не менялся)
 * ------------------------------------------------------------------------- */
#define TITLEBAR_H    32   /* GNOME-стиль: более высокий заголовок */
#define WIN_CORNER    12   /* GNOME-стиль: больше скругление */
#define WIN_SHADOW    18   /* больше радиус — мягче */
#define WIN_SHADOW_OY 8    /* смещение вниз: тень больше снизу, как в Windows/macOS */
#define WIN_MARGIN    (WIN_SHADOW + WIN_SHADOW_OY)
#define BTN_R         6    /* GNOME-стиль: чуть больше кнопки */
#define BTN_GAP       22   /* GNOME-стиль: отступ между кнопками */
#define BTN_X0        14   /* отступ от правого края до первой кнопки (справа) */

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
/* Состояние scale-from-dock анимации окна. */
#define ANIM_NONE        0
#define ANIM_OPENING     1   /* растёт из dock-launcher'а */
#define ANIM_CLOSING     2   /* стягивается к launcher; по окончании — close_window */
#define ANIM_MINIMIZING  3   /* стягивается к чипу окна; по окончании — minimized = 1 */
#define ANIM_RESTORING   4   /* растёт из чипа окна; по окончании — обычная отрисовка */
#define ANIM_MS          240

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
    /* TTF-кэш заголовка: alpha-coverage bitmap. Пересчитывается ТОЛЬКО при
     * смене win->title (см. win_invalidate_title_cache). При drag — просто
     * блит через alpha без TTF/stb_truetype. Хватит на title 256x32. */
    uint8_t  title_cov[256 * 32];
    int      title_cw, title_ch;
    int      title_cached;      /* 0 = надо перерисовать в кэш */
    /* Scale-from-dock анимация. t ∈ [0..1] идёт от src к dst (lerp).
     *   OPENING:  src = dock-icon-rect, dst = окно (win-geom)
     *   CLOSING:  src = окно (win-geom),  dst = dock-icon-rect
     * anim_start_t — PIT-тики (vos_uptime()), длительность ANIM_MS мс
     * (= ANIM_MS/10 тиков, PIT у нас 100 Гц). */
    int      anim_state;
    uint64_t anim_start_t;
    int      anim_src_x, anim_src_y, anim_src_w, anim_src_h;
    int      anim_dst_x, anim_dst_y, anim_dst_w, anim_dst_h;
    int      dock_kind;         /* для CLOSING-анимации: куда стягиваться;
                                 * -1 = не из дока, целимся в центр дока */
    /* Bbox последнего отрисованного кадра анимации — нужен для damage
     * union(prev, current): чтобы не перерисовывать обои на пол-экрана
     * каждый тик, а чистить только «уехавшую» зону. */
    int      anim_prev_x, anim_prev_y, anim_prev_w, anim_prev_h;
    int      anim_has_prev;

} vwin_t;

/* Chrome subsurface: ОДИН глобальный буфер на все окна — иначе 16 × 164 KB
 * раздуют BSS и не влезут в VortexFS. На drag фокус не меняется, окно одно —
 * один rebuild + N blits, идеально. Когда rendered_for_id меняется (фокус
 * перескочил, рисуем другое окно) — пересобираем. */
#define CHROME_MAX_W 1280
static uint32_t g_chrome_buf[CHROME_MAX_W * TITLEBAR_H];
static uint64_t g_chrome_for_id = 0;
static int      g_chrome_w = 0;
static int      g_chrome_focused = 0;

static vwin_t windows[MAX_WINDOWS];
static uint64_t next_win_id = 1;
static uint64_t focused_id = 0;

/* Lock screen: если != 0, рисуем только окно с этим id — обои/панель/док
 * /остальные окна СКРЫТЫ. Используется vlogin'ом. UNLOCK -> 0. */
static uint64_t g_lock_win = 0;

/* Лаунчер (/bin/vmenu): спавнится по VWM_LAUNCHER_TOGGLE от vpanel (клик по лого
 * V). g_vmenu_pid — pid спавненного процесса (запоминаем при spawn), g_vmenu_id —
 * id его окна (фиксируем в on_create при совпадении owner_pid). Toggle закрывает
 * окно через close_window (graceful: win_drop + VWM_EV_CLOSE), а не kill, т.к.
 * vwm не реагирует на смерть процесса сам. Обе переменные сбрасываются в win_drop
 * (само-закрытие после выбора приложения) и при повторном toggle. */
static uint64_t g_vmenu_pid = 0;
static uint64_t g_vmenu_id  = 0;

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
/* UI-текст (заголовки окон, тултипы дока): AdwaitaSans если загружен,
 * иначе fallback на встроенный bitmap font8x16 (draw_string_t). bg=0 ->
 * прозрачный фон (рисуем только глифы). */
static void draw_ui_text(int x, int y, const char *s, uint32_t fg) {
    if (vfont_ui) vfont_draw(bb, (int)fbw, (int)fbh, x, y, s, fg, 0, vfont_ui);
    else draw_string_t(x, y, s, fg);
}

/* ---------------------------------------------------------------------------
 * Title-cache: TTF — самая дорогая операция в кадре vwm (растеризация
 * через stb_truetype с alpha-блендингом). Заголовок окна не меняется во
 * время drag/resize, так что один раз растеризуем его в alpha-coverage
 * bitmap, а потом просто блитим этот bitmap с переменным цветом fg.
 * ------------------------------------------------------------------------- */
static void title_cache_rebuild(vwin_t *win) {
    win->title_cw = 0;
    win->title_ch = 0;
    if (!vfont_ui || !vfont_ui->ok) { win->title_cached = 1; return; }

    int tw = vfont_ui_text_width(win->title);
    int th = vfont_ui_line_height();
    if (tw > 256) tw = 256;
    if (th > 32)  th = 32;
    if (tw <= 0 || th <= 0) { win->title_cached = 1; return; }

    for (int i = 0; i < tw * th; i++) win->title_cov[i] = 0;

    /* Бежим по символам, складываем alpha-coverage из atlas в наш bitmap. */
    vfont_t *f = vfont_ui;
    float cur_x = 0.f;
    int baseline_y = f->ascent;
    const char *s = win->title;
    while (*s) {
        int cp = (unsigned char)*s++;
        if (cp < VF_FIRST || cp >= VF_FIRST + VF_COUNT) {
            cur_x += f->ch_w;
            continue;
        }
        stbtt_bakedchar *bc = &f->glyphs[cp - VF_FIRST];
        int gw = (int)(bc->x1 - bc->x0);
        int gh = (int)(bc->y1 - bc->y0);
        if (gw <= 0 || gh <= 0) { cur_x += bc->xadvance; continue; }
        int sx = (int)(cur_x + 0.5f) + (int)(bc->xoff + 0.5f);
        int sy = baseline_y + (int)(bc->yoff + 0.5f);
        for (int row = 0; row < gh; row++) {
            int py = sy + row;
            if (py < 0 || py >= th) continue;
            int ay = (int)bc->y0 + row;
            const unsigned char *arow = f->atlas + (unsigned int)ay * VF_ATLAS_W;
            uint8_t *drow = &win->title_cov[(unsigned int)py * tw];
            for (int col = 0; col < gw; col++) {
                int px = sx + col;
                if (px < 0 || px >= tw) continue;
                int ax = (int)bc->x0 + col;
                unsigned int a = arow[ax];
                if (!a) continue;
                if (a > drow[px]) drow[px] = (uint8_t)a;   /* max-merge */
            }
        }
        cur_x += bc->xadvance;
    }
    win->title_cw = tw;
    win->title_ch = th;
    win->title_cached = 1;
}

/* Блит кэшированного title в bb по заданному цвету fg (используя только
 * coverage bitmap). Это горячий путь — без TTF, без stb_truetype. */
static void title_cache_blit(vwin_t *win, int x, int y, uint32_t fg) {
    if (!win->title_cached) return;
    if (win->title_cw <= 0 || win->title_ch <= 0) {
        /* TTF не доступен — fallback на bitmap-шрифт. */
        draw_string_t(x, y, win->title, fg);
        return;
    }
    int tw = win->title_cw, th = win->title_ch;
    /* Клип к damage-rect (тот же, что у chrome). */
    int x0 = 0, y0 = 0, x1 = tw, y1 = th;
    if (x < clip_x0) x0 = clip_x0 - x;
    if (y < clip_y0) y0 = clip_y0 - y;
    if (x + tw > clip_x1) x1 = clip_x1 - x;
    if (y + th > clip_y1) y1 = clip_y1 - y;
    if (x0 >= x1 || y0 >= y1) return;

    unsigned int fr = (fg >> 16) & 0xFF;
    unsigned int fgc = (fg >>  8) & 0xFF;
    unsigned int fb  =  fg        & 0xFF;

    for (int j = y0; j < y1; j++) {
        uint32_t *drow = &bb[(uint32_t)(y + j) * fbw + x];
        const uint8_t *cov = &win->title_cov[(uint32_t)j * tw];
        for (int i = x0; i < x1; i++) {
            unsigned int a = cov[i];
            if (!a) continue;
            if (a == 255) {
                drow[i] = fg;
            } else {
                uint32_t d = drow[i];
                unsigned int ia = 255u - a;
                unsigned int r = (fr  * a + ((d >> 16) & 0xFF) * ia) / 255u;
                unsigned int g = (fgc * a + ((d >>  8) & 0xFF) * ia) / 255u;
                unsigned int b = (fb  * a + ( d        & 0xFF) * ia) / 255u;
                drow[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}
/* Ширина и высота строки текущим UI-шрифтом (для центрирования) */
static int ui_text_width(const char *s)  { return vfont_ui_text_width(s); }
static int ui_line_height(void)          { return vfont_ui_line_height(); }
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
/* Слияние пересекающихся damage-прямоугольников (bounding box). Во время drag
 * старая и новая позиции окна почти всегда пересекаются: без слияния зона
 * пересечения компонуется И презентуется дважды (на virtio это два лишних
 * TRANSFER+FLUSH). O(n^2) при n<=32 — копейки по сравнению с композицией. */
static int rects_overlap(const rect_t *a, const rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}
static void dmg_merge(void) {
    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < damage_count; i++) {
            for (int j = i + 1; j < damage_count; j++) {
                if (!rects_overlap(&damage[i], &damage[j])) continue;
                int x0 = damage[i].x < damage[j].x ? damage[i].x : damage[j].x;
                int y0 = damage[i].y < damage[j].y ? damage[i].y : damage[j].y;
                int xa = damage[i].x + damage[i].w, xb = damage[j].x + damage[j].w;
                int ya = damage[i].y + damage[i].h, yb = damage[j].y + damage[j].h;
                damage[i].x = x0; damage[i].y = y0;
                damage[i].w = (xa > xb ? xa : xb) - x0;
                damage[i].h = (ya > yb ? ya : yb) - y0;
                damage[j] = damage[--damage_count];
                merged = 1;
                j--;
            }
        }
    }
}
static void blit_to_front(int x, int y, int w, int h) {
    if (bb == fb) return;   /* virtio: компонуем прямо в backing, копия не нужна */
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

/* macOS-style курсор: чёрная заливка (`.`), белая обводка по контуру (`X`).
 * Аккуратная, тонкая стрелка 12×18. На любых обоях видно за счёт двойного
 * цвета — то же что делает Aqua: outer-white outline, inner-black fill. */
static const char *const cur_rows_arrow[18] = {
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
    "X..X.X..X   ",
    "X.X  X..X   ",
    "XX    X..X  ",
    "       XX X ",
    "        XXX ",
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
    [CUR_ARROW]     = { 12, 18, 0, 0, cur_rows_arrow     },
    [CUR_SIZE_H]    = { 19,  9, 9, 4, cur_rows_size_h    },
    [CUR_SIZE_V]    = {  9, 19, 4, 9, cur_rows_size_v    },
    [CUR_SIZE_NWSE] = { 15, 15, 7, 7, cur_rows_size_nwse },
    [CUR_SIZE_NESW] = { 15, 15, 7, 7, cur_rows_size_nesw },
};

static int cur_shape = CUR_ARROW;             /* текущая форма (hover)      */

/* Hardware-курсор: если virtio-gpu cursorq доступна, ядро рисует sprite
 * в отдельном слое, и vwm полностью отказывается от композита спрайта.
 * Это снимает per-frame work с курсора и оставляет его плавным даже когда
 * сцена тормозит (анимация, drag тяжёлого окна). */
static int hw_cursor_ok = 0;

/* где курсор «запечён» в back buffer после последнего кадра */
static int last_cx, last_cy, last_cw, last_ch, last_cur_valid = 0;

/* Подготовить спрайт стрелки 64×64 BGRA и отдать ядру для HW-курсора.
 * cur_rows_arrow — 12×19 ASCII-art: 'X' = чёрный контур, '.' = белая
 * заливка. На virtio-gpu фиксированный размер 64×64; рисуем стрелку в
 * верхний-левый угол с hot_x=0/hot_y=0 (как было в исходных hx/hy). */
static void hw_cursor_init(void) {
    static uint32_t sprite[64 * 64] __attribute__((aligned(16)));
    for (int i = 0; i < 64 * 64; i++) sprite[i] = 0;          /* прозрачный */
    const cur_shape_t *s = &cur_shapes[CUR_ARROW];
    for (int j = 0; j < s->h && j < 64; j++) {
        const char *row = s->rows[j];
        for (int i = 0; i < s->w && i < 64; i++) {
            char c = row[i];
            uint32_t px = 0;
            if (c == 'X')      px = 0xFF000000u;              /* чёрный  */
            else if (c == '.') px = 0xFFFFFFFFu;              /* белый   */
            sprite[j * 64 + i] = px;
        }
    }
    if (vos_cursor_set(sprite, s->hx, s->hy) == 0) {
        hw_cursor_ok = 1;
        vos_cursor_move(mouse_x, mouse_y);
    }
}

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
static uint64_t  wall_shm = (uint64_t)-1;   /* shm id обоев (пересоздаётся при
                                               смене разрешения) */

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
#define DOCK_SEP_W  16   /* зазор + место под разделительную полоску */

/* Ярлыки приложений (бывшие иконки рабочего стола — стол оставляем чистым
 * под будущие пользовательские виджеты/папки/ярлыки, как в macOS). */
typedef struct { const char *path; const char *label; int kind; } dock_item_t;
static const dock_item_t dock_items[] = {
    { "/bin/vterm",  "Terminal", 0 },
    { "/bin/vfiles", "Files",    1 },
    { "/bin/vcalc",  "Calculator", 2 },
    { "/bin/vsettings", "Settings", 3 },
};
#define DOCK_NITEMS ((int)(sizeof(dock_items) / sizeof(dock_items[0])))

static int dock_hover = -1;     /* индекс: 0..DOCK_NITEMS-1 launcher,
                                 *         DOCK_NITEMS+k — k-е окно */
static int dock_pressed = 0;

/* Pending spawn от dock-launcher'а: запомнили rect иконки и время, ждём
 * пока приложение пришлёт VWM_CREATE. on_create привяжет к окну OPENING
 * анимацию из этого rect. TTL 200 тиков (2 сек). */
static int      pending_open_valid = 0;
static int      pending_open_x, pending_open_y, pending_open_w, pending_open_h;
static int      pending_open_path_kind = -1;   /* для match'а с тем же лаунчером (если нужно) */
static uint64_t pending_open_t = 0;

/* Сколько окон сейчас живёт (для раскладки чипов справа от разделителя). */
static int dock_window_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].id) n++;
    return n;
}
/* Маппинг dock-slot (0..N-1) -> индекс в windows[]. */
static int dock_slot_to_winidx(int slot) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].id) continue;
        if (n == slot) return i;
        n++;
    }
    return -1;
}

static void dock_geometry(int *dx, int *dy, int *dw, int *dh) {
    int nwin = dock_window_count();
    int total = DOCK_NITEMS + nwin;
    int sep = (nwin > 0) ? DOCK_SEP_W : 0;
    int h = DOCK_ICON + DOCK_PAD * 2;
    int w = total * DOCK_ICON + (total - 1) * DOCK_GAP + DOCK_PAD * 2 + sep;
    *dx = ((int)fbw - w) / 2;
    *dy = (int)fbh - h - DOCK_BOTTOM;
    *dw = w; *dh = h;
}
static void dock_icon_rect(int idx, int *ix, int *iy) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    (void)dw; (void)dh;
    int x = dx + DOCK_PAD + idx * (DOCK_ICON + DOCK_GAP);
    if (idx >= DOCK_NITEMS) x += DOCK_SEP_W;   /* зазор после разделителя */
    *ix = x;
    *iy = dy + DOCK_PAD;
}
static int dock_hit(int mx, int my) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    if (mx < dx || mx >= dx + dw || my < dy || my >= dy + dh) return -1;
    int total = DOCK_NITEMS + dock_window_count();
    for (int k = 0; k < total; k++) {
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
    } else if (kind == 2) {                /* Calculator */
        fill_round(x, y, s, s, 11, 0xFF1A1E2A, 255);
        for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);
        /* Дисплей сверху */
        fill_rect(x + 7,  y + 8,  34, 9,  0xFF0E1118);
        /* Кнопки 3×3 */
        uint32_t b = 0xFF3C4054, op = 0xFFFF9F2D;
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                fill_rect(x + 7 + col * 11, y + 20 + row * 8, 8, 6, b);
            }
            fill_rect(x + 7 + 3 * 11, y + 20 + row * 8, 8, 6, op);  /* столбец операций */
        }
    } else {                               /* Settings: шестерёнка */
        fill_round(x, y, s, s, 11, 0xFF2A2A36, 255);
        for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);
        uint32_t gear = 0xFFB9C0CE;
        int cx = x + s / 2, cy = y + s / 2;
        /* зубцы: 4 прямых + 4 диагональных «лопасти» */
        fill_rect(cx - 2,  cy - 14, 4, 28, gear);
        fill_rect(cx - 14, cy - 2,  28, 4, gear);
        for (int t = -1; t <= 1; t++) {
            draw_line(cx - 9 + t, cy - 9, cx + 9 + t, cy + 9, gear);
            draw_line(cx - 9 + t, cy + 9, cx + 9 + t, cy - 9, gear);
        }
        fill_circle(cx, cy, 8, gear);
        fill_circle(cx, cy, 4, 0xFF2A2A36);   /* отверстие */
    }
}
/* Чип запущенного окна: серая плитка с инициалом, плюс индикатор-точка снизу
 * (запущено всегда, цвет акцентом для focused, приглушённый для minimized). */
static void dock_draw_window_tile(int slot, int x, int y, int s, int pressed) {
    if (pressed) { x += 1; y += 1; }
    int wi = dock_slot_to_winidx(slot);
    if (wi < 0) return;
    vwin_t *w = &windows[wi];
    int focused  = (w->id == focused_id) && !w->minimized;
    int minimized = w->minimized;

    uint32_t bg = focused ? 0xFF3A4566 : (minimized ? 0xFF1A1E2A : 0xFF2A2F44);
    fill_round(x, y, s, s, 11, bg, 255);
    for (int i = 3; i < s - 3; i++) dock_put_blend(x + i, y + 2, 0xFFFFFFFF, 16);

    /* Инициал из заголовка — крупный, по центру. */
    char init[2] = { 0, 0 };
    for (int i = 0; w->title[i]; i++) {
        char c = w->title[i];
        if (c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        init[0] = c;
        break;
    }
    if (!init[0]) init[0] = '?';
    int tw = ui_text_width(init);
    int lh = ui_line_height();
    uint32_t fg = minimized ? 0xFF9099AC : 0xFFEDF0F8;
    draw_ui_text(x + (s - tw) / 2, y + (s - lh) / 2, init, fg);

    /* Индикатор-точка под чипом. */
    int dot_y = y + s - 3;
    int dot_x = x + s / 2;
    uint32_t dot = focused ? 0xFF5B8CFF : 0xFF9099AC;
    fill_circle(dot_x, dot_y, 2, dot);
}

static void draw_dock(void) {
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    int nwin = dock_window_count();
    int total = DOCK_NITEMS + nwin;
    /* macOS-style: мягкий round-rect, не круглые торцы. Радиус ~28% высоты
     * (~20 px при dh=72) даёт характерный «squircle-ish» силуэт дока. */
    int r = dh * 28 / 100;
    fill_round(dx - 2, dy + 5, dw + 4, dh, r, 0xFF000000, 45);
    fill_round(dx, dy, dw, dh, r, 0xFF22222F, 205);
    for (int i = r; i < dw - r; i++) dock_put_blend(dx + i, dy + 1, 0xFFFFFFFF, 30);

    /* Вертикальный разделитель между лаунчерами и чипами окон. */
    if (nwin > 0) {
        int last_lx, last_ly;
        dock_icon_rect(DOCK_NITEMS - 1, &last_lx, &last_ly);
        (void)last_ly;
        int sep_x = last_lx + DOCK_ICON + DOCK_GAP + (DOCK_SEP_W - 1) / 2;
        int sep_y0 = dy + DOCK_PAD + 4;
        int sep_y1 = dy + dh - DOCK_PAD - 4;
        for (int y = sep_y0; y < sep_y1; y++)
            dock_put_blend(sep_x, y, 0xFFFFFFFF, 60);
    }

    for (int k = 0; k < total; k++) {
        int ix, iy; dock_icon_rect(k, &ix, &iy);
        int hovered = (dock_hover == k);
        int pressed = (dock_pressed && dock_hover == k);
        if (hovered)
            fill_round(ix - 4, iy - 4, DOCK_ICON + 8, DOCK_ICON + 8, 14,
                       0xFFFFFFFF, pressed ? 60 : 35);
        if (k < DOCK_NITEMS)
            dock_draw_tile(dock_items[k].kind, ix, iy, DOCK_ICON, pressed);
        else
            dock_draw_window_tile(k - DOCK_NITEMS, ix, iy, DOCK_ICON, pressed);
    }

    /* Тултип: для лаунчера — label, для чипа окна — полный title. */
    if (dock_hover >= 0 && dock_hover < total) {
        const char *t;
        char tbuf[40];
        if (dock_hover < DOCK_NITEMS) {
            t = dock_items[dock_hover].label;
        } else {
            int wi = dock_slot_to_winidx(dock_hover - DOCK_NITEMS);
            if (wi < 0) return;
            int n = 0;
            while (windows[wi].title[n] && n < (int)sizeof(tbuf) - 1) {
                tbuf[n] = windows[wi].title[n]; n++;
            }
            tbuf[n] = 0;
            t = tbuf;
        }
        int tw = ui_text_width(t) + 14, th = 22;
        int ix, iy; dock_icon_rect(dock_hover, &ix, &iy);
        (void)iy;
        int tx = ix + DOCK_ICON / 2 - tw / 2;
        int ty = dy - th - 8;
        fill_round(tx, ty + 2, tw, th, th / 2, 0xFF000000, 60);
        fill_round(tx, ty, tw, th, th / 2, 0xFF22222F, 230);
        draw_ui_text(tx + 7, ty + (th - ui_line_height()) / 2, t, 0xFFF0F2F8);
    }
}
static void dock_bounds(int *x, int *y, int *w, int *h) {
    int dy_unused, dh_curr;
    {
        int _x, _w;
        dock_geometry(&_x, &dy_unused, &_w, &dh_curr);
        (void)_x; (void)_w;
    }
    /* Док растёт/сжимается при изменении набора окон. Чтобы при удалении
     * окна старый «хвост» обоев был перерисован, инвалидируем сразу
     * максимально возможную ширину дока (DOCK_NITEMS + MAX_WINDOWS).
     * Лишняя зона — это всё равно бэк-буфер, пересылается только видимый
     * dmg-rect. */
    int total_max = DOCK_NITEMS + MAX_WINDOWS;
    int sep = DOCK_SEP_W;
    int max_w = total_max * DOCK_ICON + (total_max - 1) * DOCK_GAP + DOCK_PAD * 2 + sep;
    int max_x = ((int)fbw - max_w) / 2;
    *x = max_x - 4;
    *y = dy_unused - 34;             /* + тултип сверху */
    *w = max_w + 8;
    *h = dh_curr + 42;
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
    /* Любой commit состава/состояния окон требует перерисовки дока:
     * чипы окон живут справа от вертикального разделителя. */
    dock_dirty = 1;
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
        int a = 72 * (WIN_SHADOW - isqrt32(d2)) / WIN_SHADOW;
        shadow_alut[d2] = (uint8_t)(a > 0 ? a : 0);
    }
}
/* Тень в стиле Windows/macOS: асимметричная. Сверху почти нет, снизу и бокам — есть.
 * Реализация: вертикальный модификатор альфы — dy>0 (ниже окна) полная сила,
 * dy<0 (выше окна) сильно затухает до нуля, бока — половина силы. */
static void win_draw_shadow(int wx, int wy, int ww, int wh) {
    const int S = WIN_SHADOW;
    /* Бокс вокруг окна где рисуем тень */
    int x0 = wx - S,     y0 = wy - S / 3;   /* сверху мало */
    int x1 = wx + ww + S, y1 = wy + wh + S; /* снизу полно */

    /* Клипуем бокс к damage-rect/экрану ОДИН РАЗ — внутри цикла больше
     * никаких per-pixel проверок границ. Тень — чисто чёрная (RGB=0),
     * поэтому формула блендинга упрощается до dst = dst * (255-a) / 255. */
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) goto corners;

    for (int y = y0; y < y1; y++) {
        int dy = 0;
        if (y < wy)            dy = y - wy;
        else if (y >= wy + wh) dy = y - (wy + wh) + 1;

        int vmul;
        if (dy > 0)         vmul = 256;
        else if (dy == 0)   vmul = 140;
        else {
            int fade = S / 3 + dy;
            vmul = (fade > 0) ? (fade * 256 / (S / 3)) : 0;
            if (vmul > 140) vmul = 140;
        }
        if (vmul <= 0) continue;

        uint32_t *row = &bb[(uint32_t)y * fbw];
        int ady = dy < 0 ? -dy : dy;
        int inside_y = (dy == 0);   /* строка внутри по вертикали */

        /* Делим строку на три отрезка: левый край (dx>0), середина (внутри
         * окна по X — пропускаем если inside_y), правый край (dx>0).
         * Внутри окна тень не нужна — её закроет окно. */
        int mid_x0 = wx, mid_x1 = wx + ww;
        if (mid_x0 < x0) mid_x0 = x0;
        if (mid_x1 > x1) mid_x1 = x1;

        /* Левая «дуга» */
        for (int x = x0; x < mid_x0; x++) {
            int dx = wx - x;
            int d2 = dx * dx + ady * ady;
            if (d2 >= S * S) continue;
            int a = (int)shadow_alut[d2] * vmul >> 8;
            if (a <= 0) continue;
            uint32_t *p = &row[x];
            uint32_t d = *p;
            uint32_t ia = 255u - (uint32_t)a;
            uint32_t r = ((d >> 16) & 0xFF) * ia / 255u;
            uint32_t g = ((d >>  8) & 0xFF) * ia / 255u;
            uint32_t b = ( d        & 0xFF) * ia / 255u;
            *p = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        /* Середина: только если строка ВЫШЕ/НИЖЕ окна (там нет блита) */
        if (!inside_y) {
            for (int x = mid_x0; x < mid_x1; x++) {
                int d2 = ady * ady;
                if (d2 >= S * S) continue;
                int a = (int)shadow_alut[d2] * vmul >> 8;
                if (a <= 0) continue;
                uint32_t *p = &row[x];
                uint32_t d = *p;
                uint32_t ia = 255u - (uint32_t)a;
                uint32_t r = ((d >> 16) & 0xFF) * ia / 255u;
                uint32_t g = ((d >>  8) & 0xFF) * ia / 255u;
                uint32_t b = ( d        & 0xFF) * ia / 255u;
                *p = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        /* Правая «дуга» */
        for (int x = mid_x1; x < x1; x++) {
            int dx = x - (wx + ww) + 1;
            int d2 = dx * dx + ady * ady;
            if (d2 >= S * S) continue;
            int a = (int)shadow_alut[d2] * vmul >> 8;
            if (a <= 0) continue;
            uint32_t *p = &row[x];
            uint32_t d = *p;
            uint32_t ia = 255u - (uint32_t)a;
            uint32_t r = ((d >> 16) & 0xFF) * ia / 255u;
            uint32_t g = ((d >>  8) & 0xFF) * ia / 255u;
            uint32_t b = ( d        & 0xFF) * ia / 255u;
            *p = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

corners:
    /* угловые выемки внутри окна (за скруглением) — мало пикселей, оставляем
     * как было, через blend_px (с его клипом). */
    {
        const int r  = WIN_CORNER;
        const uint32_t ca = (uint32_t)shadow_alut[0] * 140 / 256 << 24;
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
    /* кнопки справа: close=0, min=1, max=2 */
    int bx_close = win->x + win->w - BTN_X0;
    int bx_min   = bx_close - BTN_GAP;
    int bx_max   = bx_min   - BTN_GAP;
    int bxs[3]   = { bx_close, bx_min, bx_max };
    for (int k = 0; k < 3; k++) {
        int dx = mx - bxs[k], dy = my - cy;
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

/* ---------------------------------------------------------------------------
 * Scale-from-dock анимация: интерполяция геометрии + общая альфа окна.
 * ------------------------------------------------------------------------- */
/* Прогресс [0..1] с ease-in-out (cubic) — медленный старт и финал, выраженная
 * середина: окно «разгоняется» от иконки и «припарковывается» у цели. */
static float anim_progress(vwin_t *win) {
    uint64_t now = vos_uptime();
    uint64_t elapsed_ticks = (now >= win->anim_start_t) ? (now - win->anim_start_t) : 0;
    int elapsed_ms = (int)elapsed_ticks * 10;   /* PIT 100 Гц */
    if (elapsed_ms >= ANIM_MS) return 1.f;
    float t = (float)elapsed_ms / (float)ANIM_MS;
    if (t < 0.5f) {
        return 4.f * t * t * t;
    } else {
        float u = -2.f * t + 2.f;
        return 1.f - (u * u * u) * 0.5f;
    }
}

/* Bbox анимирующегося окна — union(src, dst). Genie-эффект растягивает
 * строки по кривой между этими двумя rect'ами, поэтому union покрывает
 * всё что может появиться на экране. Используется для damage и win_intersects.
 * alpha больше не нужен извне — он вычисляется per-row внутри draw_window_anim. */
static void anim_current(vwin_t *win, int *x, int *y, int *w, int *h, int *alpha) {
    int sx = win->anim_src_x, sy = win->anim_src_y;
    int sw = win->anim_src_w, sh = win->anim_src_h;
    int dx = win->anim_dst_x, dy = win->anim_dst_y;
    int dw = win->anim_dst_w, dh = win->anim_dst_h;
    int x0 = (sx < dx) ? sx : dx;
    int y0 = (sy < dy) ? sy : dy;
    int x1 = (sx + sw > dx + dw) ? sx + sw : dx + dw;
    int y1 = (sy + sh > dy + dh) ? sy + sh : dy + dh;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (alpha) *alpha = 255;
}

/* Helper: row-blit одной строки src в bb с горизонтальным scale (fixed-point)
 * и заданной альфой. Используется как genie-эффектом (по строке), так и
 * обычным sample-блитом (для tail-renderer'ов, см. ниже). */
static inline void blit_row_scaled_alpha(const uint32_t *srow, int sw,
                                         int dst_y, int dst_x, int dst_w,
                                         unsigned int alpha) {
    if (alpha == 0 || dst_w <= 0) return;
    if (dst_y < clip_y0 || dst_y >= clip_y1) return;
    int x0 = dst_x, x1 = dst_x + dst_w;
    int sx0_off = 0;
    if (x0 < clip_x0) { sx0_off = clip_x0 - x0; x0 = clip_x0; }
    if (x1 > clip_x1) x1 = clip_x1;
    if (x0 >= x1) return;
    uint32_t sx_step = ((uint32_t)sw << 16) / (uint32_t)dst_w;
    uint32_t sx_fp   = (uint32_t)sx0_off * sx_step;
    uint32_t *drow = &bb[(uint32_t)dst_y * fbw];
    unsigned int A  = alpha;
    unsigned int ia = 255u - A;
    for (int i = x0; i < x1; i++, sx_fp += sx_step) {
        int sx = (int)(sx_fp >> 16);
        if (sx >= sw) sx = sw - 1;
        uint32_t src = srow[sx];
        uint32_t d = drow[i];
        unsigned int r = ((src >> 16) & 0xFF) * A / 255u + ((d >> 16) & 0xFF) * ia / 255u;
        unsigned int g = ((src >>  8) & 0xFF) * A / 255u + ((d >>  8) & 0xFF) * ia / 255u;
        unsigned int b = ( src        & 0xFF) * A / 255u + ( d        & 0xFF) * ia / 255u;
        drow[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* Genie/Magic-Lamp: каждая source-row получает свой row_t с «волновой»
 * задержкой по высоте окна — верхние ряды первыми долетают к финальной
 * позиции (OPENING) или последними покидают её (CLOSING). Получается
 * характерное «выползание» из дока, как в macOS.
 *
 * Источник = pixels клиента (sw × sh, включая полосу под титлбаром, она
 * заполняется приглушённым фоном — chrome во время анимации не рисуется).
 * Цель (на t=1) = реальная геометрия окна. Точка-источник (на t=0) = центр
 * dock-icon (src rect 48×48). */
static void draw_window_anim(vwin_t *win) {
    if (!win->pixels) return;

    /* Прогресс всей анимации с easing (медленный старт/финал). */
    float t = anim_progress(win);
    /* «Растущее» направление: OPENING и RESTORING — растёт от дока к окну;
     * CLOSING/MINIMIZING — обратно. */
    int opening = (win->anim_state == ANIM_OPENING ||
                   win->anim_state == ANIM_RESTORING);
    int sw = win->w;
    int sh = win->h + TITLEBAR_H;

    /* Дocк-точка (центр src rect): на t=0 здесь — все ряды. */
    int dock_cx = win->anim_src_x + win->anim_src_w / 2;
    int dock_cy = win->anim_src_y + win->anim_src_h / 2;
    int dock_hw = win->anim_src_w / 2;

    /* Финальная геометрия окна (на t=1): прямоугольник реальной позиции. */
    int win_x = win->anim_dst_x;
    int win_y = win->anim_dst_y;
    int win_w = win->anim_dst_w;
    /* anim_dst_h ~ полная высота окна с титлбаром, используем sh из pixels
     * (sh == anim_dst_h, мы их так и задавали при start). */

    /* Волна: 0.55 — половина окна уже на финальной позиции, половина ещё в
     * пути. Большие значения = более выраженный «жидкий» эффект. */
    const float WAVE = 0.55f;
    const float scale = 1.f + WAVE;

    /* Заполнение между соседними dst_y, чтобы не было gap'ов. Идём по
     * source-rows j ∈ [0, sh). prev_dst_y хранит y предыдущей row. */
    int last_dst_y = -10000;
    int titlebar_h = TITLEBAR_H;
    uint32_t base_color = 0xFF1E2330u;

    for (int j = 0; j < sh; j++) {
        float row_delay = (float)j / (float)(sh - 1);
        if (!opening) row_delay = 1.f - row_delay;   /* при closing нижние первыми */
        float lt = t * scale - row_delay * WAVE;
        if (lt < 0.f) lt = 0.f;
        if (lt > 1.f) lt = 1.f;
        /* В opening на ранних кадрах lt близко к 0 — все ряды толпой в доке,
         * их видно как одну плотную точку. Это нормально. */
        float inv = 1.f - lt;

        /* Горизонталь: левый/правый край интерполируем между dock и окном. */
        int dx_left  = (int)(inv * (dock_cx - dock_hw) + lt * win_x);
        int dx_right = (int)(inv * (dock_cx + dock_hw) + lt * (win_x + win_w));
        int dst_w    = dx_right - dx_left;
        /* Вертикаль: центр строки между dock_cy и win_y + j. */
        int dst_y = (int)(inv * dock_cy + lt * (win_y + j));

        /* Альфа: opening fade-in, closing fade-out (по локальному прогрессу
         * каждой row — выходит «жидкое» исчезание). */
        float a = opening ? lt : inv;
        unsigned int alpha = (unsigned int)(a * 255.f);
        if (alpha > 255) alpha = 255;

        /* Источник: если j внутри полосы титлбара — берём «крышечку» из
         * базового цвета (это row из 1 пикселя растягивается). Иначе — реальная
         * строка контента. */
        const uint32_t *srow;
        uint32_t one_px;
        int row_sw;
        if (j < titlebar_h) {
            one_px = base_color;
            srow = &one_px;
            row_sw = 1;
        } else {
            srow = &win->pixels[(uint32_t)(j - titlebar_h) * win->w];
            row_sw = sw;
        }

        /* Заполняем целевые y между last_dst_y и dst_y (включительно) —
         * без gap'ов даже при быстром смещении строк. */
        int y_from = (last_dst_y == -10000) ? dst_y : last_dst_y + 1;
        int y_to   = dst_y;
        if (y_to < y_from) { int tmp = y_from; y_from = y_to; y_to = tmp; }
        for (int y = y_from; y <= y_to; y++)
            blit_row_scaled_alpha(srow, row_sw, y, dx_left, dst_w, alpha);
        last_dst_y = dst_y;
    }
}

/* Bbox анимирующегося окна для damage. */
static void anim_bbox(vwin_t *win, int *x, int *y, int *w, int *h) {
    int sx = win->anim_src_x, sy = win->anim_src_y;
    int sw = win->anim_src_w, sh = win->anim_src_h;
    int dx = win->anim_dst_x, dy = win->anim_dst_y;
    int dw = win->anim_dst_w, dh = win->anim_dst_h;
    int x0 = (sx < dx) ? sx : dx;
    int y0 = (sy < dy) ? sy : dy;
    int x1 = (sx + sw > dx + dw) ? sx + sw : dx + dw;
    int y1 = (sy + sh > dy + dh) ? sy + sh : dy + dh;
    *x = x0 - 4; *y = y0 - 4;
    *w = x1 - x0 + 8; *h = y1 - y0 + 8;
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
/* Helpers для render'а chrome в локальный буфер (без bb/clip — координаты
 * 0..w/0..TITLEBAR_H в chrome_buf). */
static inline void cb_put(uint32_t *cb, int w, int h, int x, int y, uint32_t c) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    cb[y * w + x] = c;
}
static inline void cb_blend(uint32_t *cb, int w, int h, int x, int y, uint32_t argb) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 0) return;
    uint32_t *p = &cb[y * w + x];
    if (a == 0xFF) { *p = 0xFF000000u | (argb & 0x00FFFFFF); return; }
    uint32_t dst = *p;
    uint32_t fr = (argb >> 16) & 0xFF, fgc = (argb >> 8) & 0xFF, fbl = argb & 0xFF;
    uint32_t br = (dst >> 16) & 0xFF, bgc = (dst >> 8) & 0xFF, bbl = dst & 0xFF;
    uint32_t rr = (fr  * a + br  * (255 - a)) / 255;
    uint32_t gg = (fgc * a + bgc * (255 - a)) / 255;
    uint32_t bb_= (fbl * a + bbl * (255 - a)) / 255;
    *p = 0xFF000000u | (rr << 16) | (gg << 8) | bb_;
}
static inline void cb_fill_circle(uint32_t *cb, int w, int h, int cx, int cy, int r, uint32_t color) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                cb_put(cb, w, h, cx + i, cy + j, color);
}

/* Перерисовать g_chrome_buf для окна. Без AA-углов снизу (они зависят от
 * обоев под окном, делаются live в draw_window_chrome). */
static void chrome_rebuild(vwin_t *win) {
    int focused = (win->id == focused_id);
    int w = win->w;
    if (w <= 0 || w > CHROME_MAX_W) { g_chrome_for_id = 0; return; }
    int h = TITLEBAR_H;
    uint32_t *cb = g_chrome_buf;

    /* 1. Фон титлбара с AA-скруглением верхних углов (как fill_round_top). */
    uint32_t tb_color = focused ? 0xFF353550 : 0xFF252535;
    uint32_t rgb = tb_color & 0x00FFFFFF;
    int r = WIN_CORNER;
    if (r * 2 > w) r = w / 2;
    if (r > h) r = h;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (j < r && (i < r || i >= w - r)) {
                int cx = (i < r) ? r : (w - r);
                int cov = circ_cov(i, j, cx, r, r);
                if (cov <= 0) { cb[j * w + i] = 0; continue; }
                if (cov >= 255) cb[j * w + i] = tb_color;
                else {
                    /* AA по AA-краю: храним пиксель с alpha=cov и rgb цвета
                     * титлбара. chrome_blit на финале сделает src-over с тем
                     * что в bb (обои/тень) — без чёрного «гало». */
                    cb[j * w + i] = ((uint32_t)cov << 24) | rgb;
                }
            } else {
                cb[j * w + i] = tb_color;
            }
        }
    }

    /* 2. Тонкий блик поверх. */
    for (int i = WIN_CORNER; i < w - WIN_CORNER; i++)
        cb_blend(cb, w, h, i, 0, 0x18FFFFFFu);

    /* 3. Кнопки (круги). */
    int cy = TITLEBAR_H / 2;
    int bx_close = w - BTN_X0;
    int bx_min   = bx_close - BTN_GAP;
    int bx_max   = bx_min   - BTN_GAP;
    uint32_t cclose = focused ? 0xFFFF5F56 : 0xFF4A4A5A;
    uint32_t cmin   = focused ? 0xFFFFBD2E : 0xFF4A4A5A;
    uint32_t cmax   = focused ? 0xFF27C93F : 0xFF4A4A5A;
    cb_fill_circle(cb, w, h, bx_max,   cy, BTN_R, cmax);
    cb_fill_circle(cb, w, h, bx_min,   cy, BTN_R, cmin);
    cb_fill_circle(cb, w, h, bx_close, cy, BTN_R, cclose);

    if (focused) {
        const uint32_t g = 0x96000000u;
        for (int d = -2; d <= 2; d++) {
            cb_blend(cb, w, h, bx_close + d, cy + d, g);
            if (d) cb_blend(cb, w, h, bx_close + d, cy - d, g);
            cb_blend(cb, w, h, bx_min   + d, cy, g);
            cb_blend(cb, w, h, bx_max   + d, cy, g);
            if (d) cb_blend(cb, w, h, bx_max, cy + d, g);
        }
    }

    /* Верхний border (1px) — рисуется на каждый кадр live, потому что
     * focused может меняться, а у нас всё в этой функции и так привязано. */
    uint32_t bord = focused ? 0xFF454560u : 0xFF2E2E3Au;
    for (int i = r; i < w - r; i++)
        cb_put(cb, w, h, i, 0, bord);
    for (int j = r; j < h; j++) {
        cb_put(cb, w, h, 0,     j, bord);
        cb_put(cb, w, h, w - 1, j, bord);
    }

    g_chrome_for_id  = win->id;
    g_chrome_w       = w;
    g_chrome_focused = focused;
}

/* Блит chrome полосы в bb с клипом — используется на горячем пути. */
static void chrome_blit(vwin_t *win) {
    if (g_chrome_for_id != win->id) return;
    int w = g_chrome_w, h = TITLEBAR_H;
    int sx = win->x, sy = win->y;
    int x0 = sx, y0 = sy, x1 = sx + w, y1 = sy + h;
    int cx0 = 0, cy0 = 0;
    if (x0 < clip_x0) { cx0 = clip_x0 - x0; x0 = clip_x0; }
    if (y0 < clip_y0) { cy0 = clip_y0 - y0; y0 = clip_y0; }
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *drow = &bb[(uint32_t)j * fbw + x0];
        const uint32_t *srow = &g_chrome_buf[(j - sy) * w + cx0];
        int n = x1 - x0;
        for (int i = 0; i < n; i++) {
            uint32_t s = srow[i];
            /* alpha=0 — «вне chrome» (углы за AA): не трогаем bb, там обои/
             * окна сзади. Полное непрозрачное — простая копия. Промежуточная
             * альфа — стандартный src-over blend. */
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 0xFF) { drow[i] = s; continue; }
            uint32_t d = drow[i];
            uint32_t ia = 255u - a;
            uint32_t r = (((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255u;
            uint32_t g = (((s >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * ia) / 255u;
            uint32_t b = (( s        & 0xFF) * a + ( d        & 0xFF) * ia) / 255u;
            drow[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void draw_window_chrome(vwin_t *win) {
    int focused = (win->id == focused_id);
    int fh = win->h + TITLEBAR_H;

    win_draw_shadow(win->x, win->y, win->w, fh);

    /* Chrome subsurface: один глобальный prerendered буфер. Пересчитываем
     * когда (id, w, focused) изменились. На drag окно не меняется — один
     * rebuild на старте, остальные кадры — только blit. */
    if (g_chrome_for_id != win->id ||
        g_chrome_w     != win->w  ||
        g_chrome_focused != focused) {
        chrome_rebuild(win);
    }
    chrome_blit(win);

    /* Заголовок по центру — из cached coverage bitmap, без TTF. Title
     * блитится поверх chrome (чтобы цвет fg менялся по focused без
     * пересборки chrome). */
    if (!win->title_cached) title_cache_rebuild(win);
    int title_w = win->title_cw ? win->title_cw : ui_text_width(win->title);
    int title_h = win->title_ch ? win->title_ch : ui_line_height();
    int tx = win->x + (win->w - title_w) / 2;
    if (tx < win->x + 8) tx = win->x + 8;
    title_cache_blit(win, tx, win->y + (TITLEBAR_H - title_h) / 2,
                     focused ? 0xFFEDEFF6 : 0xFF7A7A9A);

    /* Save-under для AA нижних углов — они нужны live, потому что под
     * окном могут быть обои/окна с любым цветом. */
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

    /* Боковая+нижняя рамка (1px). Верхняя строка уже в chrome_buf. */
    uint32_t bord = focused ? 0xFF454560u : 0xFF2E2E3Au;
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
    int wx, wy, ww, wh;
    if (win->anim_state != ANIM_NONE) {
        /* Анимирующееся окно занимает интерполированный rect, не реальный. */
        int ax, ay, aw, ah, alpha;
        anim_current((vwin_t *)win, &ax, &ay, &aw, &ah, &alpha);
        (void)alpha;
        wx = ax - 4; wy = ay - 4; ww = aw + 8; wh = ah + 8;
    } else {
        int m = WIN_MARGIN;
        wx = win->x - m; wy = win->y - m;
        ww = win->w + 2 * m; wh = win->h + TITLEBAR_H + 2 * m;
    }
    if (wx >= rx + rw || wx + ww <= rx) return 0;
    if (wy >= ry + rh || wy + wh <= ry) return 0;
    return 1;
}

/* Пересобрать сцену (без курсора) внутри прямоугольника.
 * start_idx >= 0 — оптимизация (occlusion): известно, что НЕПРОЗРАЧНОЕ тело
 * окна windows[start_idx] целиком накрывает rect => обои и окна ниже него
 * в этом rect не видны, их можно не рисовать (главная экономия CPU при
 * drag больших окон: вместо «обои + окно» рисуется только окно). */
static void compose_rect_from(int rx, int ry, int rw, int rh, int start_idx) {
    clip_set(rx, ry, rw, rh);
    if (clip_empty()) { clip_reset(); return; }

    /* Lock screen: рисуем тёмный фон и ТОЛЬКО lock-окно. Никаких чужих окон,
     * панели и дока. (start_idx тут не используем — режим обзора прямой.) */
    if (g_lock_win) {
        fill_rect(rx, ry, rw, rh, 0xFF1A1A24u);
        for (int i = 0; i < MAX_WINDOWS; i++) {
            vwin_t *win = &windows[i];
            if (win->id != g_lock_win || !win->pixels) continue;
            if (!win_intersects(win, rx, ry, rw, rh)) continue;
            /* Lock-окно рисуем БЕЗ chrome — экран блокировки сам себе chrome. */
            blit_buffer(win->x, win->y, win->w, win->h, win->pixels);
            break;
        }
        clip_reset();
        return;
    }

    if (start_idx < 0) {
        fill_wall(rx, ry, rw, rh);
        start_idx = 0;
    }

    for (int i = start_idx; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id || !win->pixels || win->minimized) continue;
        if (!win_intersects(win, rx, ry, rw, rh)) continue;
        if (win->anim_state != ANIM_NONE)
            draw_window_anim(win);    /* масштабированная миниатюра + fade */
        else
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

static void compose_rect(int rx, int ry, int rw, int rh) {
    compose_rect_from(rx, ry, rw, rh, -1);
}

/* Непрозрачное тело окна: контент без титлбара (верхние скругления/блик) и
 * без нижних WIN_CORNER строк (скруглённые AA-углы просвечивают фон). */
static void win_opaque_body(const vwin_t *w, int *x, int *y, int *ww, int *hh) {
    *x = w->x; *y = w->y + TITLEBAR_H;
    *ww = w->w; *hh = w->h - WIN_CORNER;
}

/* Компоновка damage-прямоугольника с учётом перекрытия (occlusion).
 * Если непрозрачное тело какого-то окна накрывает БОЛЬШУЮ часть rect
 * (типичный случай: drag/resize/COMMIT большого окна), режем rect на
 * центр (рисуем начиная с этого окна, без обоев и нижних окон) и до
 * четырёх рамок по краям (полная компоновка). На эмулируемом CPU это
 * сокращает работу кадра при drag примерно вдвое. */
static void compose_damage(int rx, int ry, int rw, int rh) {
    int best = -1, best_area = 0;
    int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id || !win->pixels || win->minimized) continue;
        if (win->anim_state != ANIM_NONE) continue;   /* не накрывает полностью */
        int bx, by, bw, bh;
        win_opaque_body(win, &bx, &by, &bw, &bh);
        if (bw <= 0 || bh <= 0) continue;
        int x0 = imax(rx, bx), y0 = imax(ry, by);
        int x1 = imin(rx + rw, bx + bw), y1 = imin(ry + rh, by + bh);
        if (x1 <= x0 || y1 <= y0) continue;
        int area = (x1 - x0) * (y1 - y0);
        /* >= : при равном покрытии берём окно ВЫШЕ по z — меньше перерисовки */
        if (area >= best_area) {
            best = i; best_area = area;
            ix0 = x0; iy0 = y0; ix1 = x1; iy1 = y1;
        }
    }

    /* нет доминирующего непрозрачного куска — обычный путь */
    if (best < 0 || best_area * 2 < rw * rh) {
        compose_rect(rx, ry, rw, rh);
        return;
    }

    /* центр: тело окна best накрывает sub-rect целиком => старт с него */
    compose_rect_from(ix0, iy0, ix1 - ix0, iy1 - iy0, best);
    /* рамки вокруг центра — полная компоновка */
    if (iy0 > ry)           compose_rect(rx, ry, rw, iy0 - ry);              /* верх  */
    if (iy1 < ry + rh)      compose_rect(rx, iy1, rw, ry + rh - iy1);        /* низ   */
    if (ix0 > rx)           compose_rect(rx, iy0, ix0 - rx, iy1 - iy0);      /* лево  */
    if (ix1 < rx + rw)      compose_rect(ix1, iy0, rx + rw - ix1, iy1 - iy0);/* право */
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

    dmg_merge();

    for (int i = 0; i < damage_count; i++)
        compose_damage(damage[i].x, damage[i].y, damage[i].w, damage[i].h);

    /* курсор — верхний слой: дорисовать в каждый затронутый регион.
     * При HW-курсоре спрайт рисует QEMU поверх scanout — пропускаем
     * полностью, в bb ничего не пишем, cursor damage не нужен. */
    if (!hw_cursor_ok) {
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
    if (cursor_moved) {
        cursor_moved = 0;
        if (!hw_cursor_ok) dmg_cursor();
    }
    frame();
}

/* Кадр по тику (порт wm_tick_render): выбирает самый дешёвый путь. */
static uint64_t last_panel_sec = 0;

/* Forward: close_window и finalize_minimize определены ниже, а вызываются
 * из tick_render по окончании CLOSING/MINIMIZING анимаций. */
static void close_window(vwin_t *win);
static void finalize_minimize(vwin_t *win);

/* Есть ли хоть одно анимирующееся окно (для плавности — рендер каждый тик). */
static int anim_any_active(void) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].id && windows[i].anim_state != ANIM_NONE) return 1;
    return 0;
}

static void tick_render(void) {
    uint64_t sec = vos_uptime() / 100;
    if (sec != last_panel_sec) { last_panel_sec = sec; panel_dirty = 1; }

    /* Scale-from-dock анимации: на каждом тике продвигаем все active окна.
     * По завершении OPENING — переход к нормальной отрисовке. По завершении
     * CLOSING — реальный close_window. На каждом активном кадре анимации
     * invalidate'им bbox(src ∪ dst), чтобы был перерисован обои/окна под
     * движущейся миниатюрой. */
    uint64_t now = vos_uptime();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id || win->anim_state == ANIM_NONE) continue;
        uint64_t elapsed = (now >= win->anim_start_t) ? (now - win->anim_start_t) : 0;
        int elapsed_ms = (int)elapsed * 10;

        /* Damage = текущий rect анимации (+ предыдущий если был) — узкая
         * полоса между двумя кадрами, а не половина экрана. */
        int cx, cy, cw, ch, alpha;
        anim_current(win, &cx, &cy, &cw, &ch, &alpha);
        (void)alpha;
        int margin = 6;
        dmg_add(cx - margin, cy - margin, cw + 2 * margin, ch + 2 * margin);
        if (win->anim_has_prev) {
            dmg_add(win->anim_prev_x - margin, win->anim_prev_y - margin,
                    win->anim_prev_w + 2 * margin, win->anim_prev_h + 2 * margin);
        }
        win->anim_prev_x = cx; win->anim_prev_y = cy;
        win->anim_prev_w = cw; win->anim_prev_h = ch;
        win->anim_has_prev = 1;
        needs_redraw = 1;

        if (elapsed_ms >= ANIM_MS) {
            /* На завершении — окно встанет на финальное место (или закроется).
             * Перерисовать old prev_rect (там был последний кадр) + destination
             * rect, чтобы остатков не было. */
            dmg_add(win->anim_prev_x - margin, win->anim_prev_y - margin,
                    win->anim_prev_w + 2 * margin, win->anim_prev_h + 2 * margin);
            if (win->anim_state == ANIM_CLOSING) {
                close_window(win);
            } else if (win->anim_state == ANIM_MINIMIZING) {
                win->anim_state = ANIM_NONE;
                win->anim_has_prev = 0;
                finalize_minimize(win);
            } else {
                int m = WIN_MARGIN;
                dmg_add(win->x - m, win->y - m,
                        win->w + 2 * m, win->h + TITLEBAR_H + 2 * m);
                win->anim_state = ANIM_NONE;
                win->anim_has_prev = 0;
            }
        }
    }

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
        if (!hw_cursor_ok) dmg_cursor();  /* HW-курсор: damage не нужен */
    }
    frame();
}

/* Перерисовать только область одного окна (после COMMIT клиента). */
static void render_window_region(vwin_t *win) {
    int m = WIN_MARGIN;
    render_region(win->x - m, win->y - m,
                  win->w + 2 * m, win->h + TITLEBAR_H + 2 * m);
}

/* Per-client damage: клиент COMMIT'ит конкретный rect в координатах своего
 * shm (без титлбара, т.е. (0,0) = верх контента). vwm перерисовывает только
 * пересечение этой зоны со своей экранной геометрией окна. Если rect = весь
 * shm — поведение совпадает с render_window_region.
 *
 * Клипуем к содержимому окна (без титлбара/border'ов), потому что damage от
 * клиента описывает только контент. Перевод в экранные координаты:
 *   screen_x = win->x + cx,  screen_y = win->y + TITLEBAR_H + cy. */
static void render_commit_region(vwin_t *win, int cx, int cy, int cw, int ch) {
    if (cw <= 0 || ch <= 0) { render_window_region(win); return; }
    /* Клип к содержимому окна. */
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > win->w) cw = win->w - cx;
    if (cy + ch > win->h) ch = win->h - cy;
    if (cw <= 0 || ch <= 0) return;
    /* Перерисовка ровно содержимого, без overdraw chrome/тени. Тень/титлбар
     * не пострадают: они не лежат в этой зоне. */
    render_region(win->x + cx, win->y + TITLEBAR_H + cy, cw, ch);
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
    if (g_vmenu_id == win->id) { g_vmenu_id = 0; g_vmenu_pid = 0; }
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

/* Найти dock-icon-rect, куда лететь при CLOSING. По kind, если знаем;
 * иначе — центр дока (нейтральная цель). */
static void close_target_rect(int kind, int *tx, int *ty, int *tw, int *th) {
    if (kind >= 0) {
        for (int k = 0; k < DOCK_NITEMS; k++) {
            if (dock_items[k].kind == kind) {
                int ix, iy;
                dock_icon_rect(k, &ix, &iy);
                *tx = ix; *ty = iy;
                *tw = DOCK_ICON; *th = DOCK_ICON;
                return;
            }
        }
    }
    int dx, dy, dw, dh;
    dock_geometry(&dx, &dy, &dw, &dh);
    *tw = DOCK_ICON; *th = DOCK_ICON;
    *tx = dx + (dw - *tw) / 2;
    *ty = dy + (dh - *th) / 2;
}

/* Rect чипа окна в доке (для minimize/restore анимации). win_idx — индекс
 * окна в windows[]. Возврат 0 = нет такого чипа (например, нет окон). */
static int win_chip_rect(int win_idx, int *tx, int *ty, int *tw, int *th) {
    int slot = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].id) continue;
        if (i == win_idx) {
            int ix, iy;
            dock_icon_rect(DOCK_NITEMS + slot, &ix, &iy);
            *tx = ix; *ty = iy;
            *tw = DOCK_ICON; *th = DOCK_ICON;
            return 1;
        }
        slot++;
    }
    return 0;
}

/* Запустить CLOSING-анимацию. По окончании tick_render позовёт close_window. */
static void start_close_anim(vwin_t *win) {
    if (win->anim_state == ANIM_CLOSING) return;  /* уже летим */
    int tx, ty, tw, th;
    close_target_rect(win->dock_kind, &tx, &ty, &tw, &th);
    win->anim_state   = ANIM_CLOSING;
    win->anim_start_t = vos_uptime();
    win->anim_src_x = win->x;
    win->anim_src_y = win->y;
    win->anim_src_w = win->w;
    win->anim_src_h = win->h + TITLEBAR_H;
    win->anim_dst_x = tx;
    win->anim_dst_y = ty;
    win->anim_dst_w = tw;
    win->anim_dst_h = th;
    win->anim_has_prev = 0;
    needs_redraw = 1;
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
/* Финализация minimize — вызывается из tick по окончании анимации. */
static void finalize_minimize(vwin_t *win) {
    win->minimized = 1;
    if (focused_id == win->id) {
        focused_id = 0;
        for (int i = MAX_WINDOWS - 1; i >= 0; i--)
            if (windows[i].id && !windows[i].minimized) {
                focused_id = windows[i].id;
                break;
            }
    }
    needs_redraw = 1;
    panel_send_wins();
}

static void minimize_window(vwin_t *win) {
    if (win->anim_state == ANIM_MINIMIZING) return;
    int win_idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (&windows[i] == win) { win_idx = i; break; }
    int tx, ty, tw, th;
    if (win_idx < 0 || !win_chip_rect(win_idx, &tx, &ty, &tw, &th))
        close_target_rect(win->dock_kind, &tx, &ty, &tw, &th);
    win->anim_state   = ANIM_MINIMIZING;
    win->anim_start_t = vos_uptime();
    win->anim_src_x = win->x; win->anim_src_y = win->y;
    win->anim_src_w = win->w; win->anim_src_h = win->h + TITLEBAR_H;
    win->anim_dst_x = tx; win->anim_dst_y = ty;
    win->anim_dst_w = tw; win->anim_dst_h = th;
    win->anim_has_prev = 0;
    if (drag.active && drag.win_id == win->id) drag.active = 0;
    if (rz.active && rz.win_id == win->id) rz.active = 0;
    needs_redraw = 1;
}

static void restore_window(vwin_t *win) {
    uint64_t id = win->id;
    win->minimized = 0;
    focused_id = id;
    raise_window(id);
    vwin_t *w = find_window(id);
    if (w) {
        int win_idx = -1;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (&windows[i] == w) { win_idx = i; break; }
        int sx, sy, sw, sh;
        if (win_idx < 0 || !win_chip_rect(win_idx, &sx, &sy, &sw, &sh))
            close_target_rect(w->dock_kind, &sx, &sy, &sw, &sh);
        w->anim_state   = ANIM_RESTORING;
        w->anim_start_t = vos_uptime();
        w->anim_src_x = sx; w->anim_src_y = sy;
        w->anim_src_w = sw; w->anim_src_h = sh;
        w->anim_dst_x = w->x; w->anim_dst_y = w->y;
        w->anim_dst_w = w->w; w->anim_dst_h = w->h + TITLEBAR_H;
        w->anim_has_prev = 0;
    }
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
    /* HW-курсор: позиция отдаётся напрямую в virtio-gpu, минуя back buffer.
     * Курсор плавный даже когда vwm рендерит 5 FPS. */
    if (hw_cursor_ok) vos_cursor_move(mouse_x, mouse_y);

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

    /* Lock-screen: все клики ТОЛЬКО lock-окну, дока/панели/других окон нет. */
    if (g_lock_win) {
        vwin_t *w = find_window(g_lock_win);
        if (w) {
            send_event4(w->owner_pid, VWM_EV_MOUSE, w->id,
                        (uint64_t)mx, (uint64_t)my, (uint64_t)buttons);
        }
        return;
    }

    /* --- Dock поверх окон --- */
    int dh = dock_hit(mx, my);
    if (buttons & 1) {
        if (dh >= 0) {
            dock_hover = dh;
            dock_pressed = 1;
            dock_dirty = 1;
            if (dh < DOCK_NITEMS) {
                /* Лаунчер: запустить приложение. Запоминаем rect иконки —
                 * первое окно от этого приложения (придёт через VWM_CREATE)
                 * получит OPENING-анимацию роста из этого прямоугольника. */
                if (active_window_count() < MAX_WINDOWS) {
                    int ix, iy;
                    dock_icon_rect(dh, &ix, &iy);
                    pending_open_x = ix; pending_open_y = iy;
                    pending_open_w = DOCK_ICON; pending_open_h = DOCK_ICON;
                    pending_open_path_kind = dock_items[dh].kind;
                    pending_open_t = vos_uptime();
                    pending_open_valid = 1;
                    vos_spawn(dock_items[dh].path);
                }
            } else {
                /* Чип запущенного окна: minimized -> восстановить; focused ->
                 * свернуть; иначе — поднять и сфокусировать. */
                int wi = dock_slot_to_winidx(dh - DOCK_NITEMS);
                if (wi >= 0) {
                    vwin_t *w = &windows[wi];
                    if (w->minimized) {
                        restore_window(w);
                    } else if (w->id == focused_id) {
                        minimize_window(w);
                    } else {
                        focused_id = w->id;
                        raise_window(w->id);
                        needs_redraw = 1;
                        panel_send_wins();
                    }
                }
            }
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
            if (b == 0) { start_close_anim(win); return; } /* 🔴 закрыть (анимация → реальный close в tick) */
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
    /* Лаунчер: запоминаем id его окна и заякориваем под лого V (слева, под
     * панелью) — так popup появляется рядом с кнопкой V, как задумано. */
    if (sender == g_vmenu_pid && g_vmenu_pid) {
        g_vmenu_id = win->id;
        win->x = 8;
        win->y = PANEL_H + 6;
    }
    const char *t = (const char *)&m->w[3];
    int i = 0;
    while (t[i] && i < 31) { win->title[i] = t[i]; i++; }
    win->title[i] = 0;
    win->title_cached = 0;       /* title-cache: пересчитаем при первом draw */

    /* Scale-from-dock OPENING: если пользователь только что кликнул иконку
     * в доке (pending_open_valid + TTL не вышел), окно начинает с rect
     * иконки и растёт до своих win->x/y/w/h. */
    win->anim_state = ANIM_NONE;
    win->dock_kind  = -1;
    if (pending_open_valid && vos_uptime() - pending_open_t < 600) {
        win->anim_state = ANIM_OPENING;
        win->anim_start_t = vos_uptime();
        win->anim_src_x = pending_open_x;
        win->anim_src_y = pending_open_y;
        win->anim_src_w = pending_open_w;
        win->anim_src_h = pending_open_h;
        win->anim_dst_x = win->x;
        win->anim_dst_y = win->y;
        win->anim_dst_w = win->w;
        win->anim_dst_h = win->h + TITLEBAR_H;
        win->dock_kind  = pending_open_path_kind;
        win->anim_has_prev = 0;
        pending_open_valid = 0;
    }

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

/* ---------------------------------------------------------------------------
 * Ядро сменило видеорежим (SYS_DISPLAY_SET_MODE из «Настроек» -> VIN_DISPLAY).
 * sys_fb_map отдаёт маппинг на ВЕСЬ backing (под максимальный режим), так что
 * указатель fb остаётся валидным — меняется только геометрия. VIN_DISPLAY
 * приходит только на virtio-gpu, а там bb == fb (direct compose).
 * ------------------------------------------------------------------------- */
static void on_display_change(uint32_t nw, uint32_t nh) {
    if (!nw || !nh || (nw == fbw && nh == fbh)) return;
    if (bb != fb) return;   /* не direct compose — событие не для нас */

    fbw = nw; fbh = nh; fb_stride = nw;   /* virtio: pitch всегда = w*4 */

    /* Обои: площадь сменилась — пересоздаём shm и рендерим заново */
    if (wall) {
        vos_shm_release(wall_shm);
        wall = 0; wall_shm = (uint64_t)-1;
    }
    wall_shm = vos_shm_create((uint64_t)fbw * fbh * 4);
    if (wall_shm != (uint64_t)-1) {
        wall = (uint32_t *)vos_shm_map(wall_shm);
        if (wall) wall_render();
        else { vos_shm_release(wall_shm); wall_shm = (uint64_t)-1; }
    }

    /* Мышь — в пределах нового экрана */
    if (mouse_x >= (int)fbw) mouse_x = (int)fbw - 1;
    if (mouse_y >= (int)fbh) mouse_y = (int)fbh - 1;

    /* Окна: развёрнутые подгоняем под новый стол, обычные клампим внутрь.
     * Любое изменение РАЗМЕРА идёт через win_apply_geometry — оно чистит
     * поверхность и шлёт клиенту EV_RESIZE (новый stride!). */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        vwin_t *win = &windows[i];
        if (!win->id) continue;
        if (win->maximized) {
            int mw = (int)fbw;
            int mh = (int)fbh - PANEL_H - TITLEBAR_H;
            if (mw * mh > VWM_MAX_PIXELS) mh = VWM_MAX_PIXELS / mw;
            win_apply_geometry(win, 0, PANEL_H, mw, mh);
        } else {
            int x = win->x, y = win->y, w = win->w, h = win->h;
            int maxh = (int)fbh - PANEL_H - TITLEBAR_H;
            if (w > (int)fbw) w = (int)fbw;
            if (h > maxh)     h = maxh;
            if (x + w > (int)fbw)               x = (int)fbw - w;
            if (y + h + TITLEBAR_H > (int)fbh)  y = (int)fbh - h - TITLEBAR_H;
            if (x < 0)            x = 0;
            if (y < PANEL_H)      y = PANEL_H;
            if (x != win->x || y != win->y || w != win->w || h != win->h)
                win_apply_geometry(win, x, y, w, h);
        }
    }

    /* Панель: её поверхность fbw x PANEL_H — stride устарел. Отцепляем и
     * просим vpanel пересоздать shm и заново прислать ATTACH. */
    if (panel_pid) {
        if (panel_surf && panel_shm != (uint64_t)-1)
            vos_shm_release(panel_shm);
        panel_surf = 0;
        panel_shm = (uint64_t)-1;
        send_event(panel_pid, VWM_PANEL_REATTACH,
                   (uint64_t)fbw, (uint64_t)fbh, 0);
    }

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
    case VIN_DISPLAY:
        on_display_change((uint32_t)m->w[1], (uint32_t)m->w[2]);
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
        /* Per-client damage: vpanel шлёт rect только изменённой области
         * (часы — узкий прямоугольник). Добавляем damage точечно вместо
         * перерисовки всей панели. Если rect нулевой (legacy) — старое
         * поведение через panel_dirty. */
        if (m->w[7] == panel_pid) {
            int cx = (int)m->w[1];
            int cy = (int)m->w[2];
            int cw = (int)m->w[3];
            int ch = (int)m->w[4];
            if (cw > 0 && ch > 0 && (cx | cy | cw | ch)) {
                int px, py, pw, ph;
                panel_bounds(&px, &py, &pw, &ph);
                /* Координаты vpanel'а — относительно его surf (которая
                 * накладывается в (px,py)). Перевод в экранные: +px,+py. */
                dmg_add(px + cx, py + cy, cw, ch);
                needs_redraw = 1;
            } else {
                panel_dirty = 1;
            }
        }
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
    case VWM_LAUNCHER_TOGGLE:
        /* Клик по лого V в панели. Открыт -> закрыть (graceful close_window:
         * win_drop + VWM_EV_CLOSE, vmenu сам выйдет). Закрыт -> спавнить. */
        if (m->w[7] == panel_pid) {
            vwin_t *mw = g_vmenu_id ? find_window(g_vmenu_id) : 0;
            if (mw) {
                close_window(mw);          /* win_drop сбросит g_vmenu_id/pid */
            } else {
                g_vmenu_id = 0; g_vmenu_pid = 0;   /* окно умерло без DESTROY */
                int64_t pid = (int64_t)vos_spawn("/bin/vmenu");
                if (pid > 0) g_vmenu_pid = (uint64_t)pid;
            }
        }
        break;
    case VWM_COMMIT: {
        vwin_t *win = find_window(m->w[1]);
        if (win && win->owner_pid == m->w[7]) {
            /* Per-client damage: w2/w3/w4/w5 — rect в координатах shm
             * (контент окна без титлбара). Клиенты, шлющие весь shm,
             * передают (0,0,win_w,win_h) — поведение совпадает с full
             * перерисовкой как было раньше. */
            int cx = (int)m->w[2];
            int cy = (int)m->w[3];
            int cw = (int)m->w[4];
            int ch = (int)m->w[5];
            if (win->minimized)
                ;
            else if (win->anim_state != ANIM_NONE)
                needs_redraw = 1;     /* во время анимации перерисует tick */
            else if (!needs_redraw && !drag.active && !rz.active)
                render_commit_region(win, cx, cy, cw, ch);
            else
                needs_redraw = 1;
        }
        break;
    }
    case VWM_LOCK: {
        /* Окно-владелец lock'а должно существовать и принадлежать отправителю. */
        vwin_t *win = find_window(m->w[1]);
        if (win && win->owner_pid == m->w[7]) {
            g_lock_win = win->id;
            focused_id = win->id;
            /* Растягиваем lock-окно на весь экран без титлбара. */
            win->x = 0; win->y = 0;
            win->w = (int)fbw; win->h = (int)fbh;
            /* Клиенту нужен resize, чтобы перерисоваться под новый размер. */
            send_event(win->owner_pid, VWM_EV_RESIZE, win->id,
                       (uint64_t)win->w, (uint64_t)win->h);
            damage_full = 1;
            needs_redraw = 1;
        }
        break;
    }
    case VWM_UNLOCK:
        if (g_lock_win) {
            /* Снимаем lock И сразу убираем lock-окно (если клиент ещё его не
             * закрыл). Без этого vlogin'овский фуллскрин-сюрфейс продолжал бы
             * рисоваться поверх обоев как обычное окно. */
            vwin_t *lw = find_window(g_lock_win);
            g_lock_win = 0;
            if (lw && lw->owner_pid == m->w[7]) win_drop(lw);
            damage_full = 1;
            needs_redraw = 1;
            panel_send_wins();
        }
        break;
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

    /* 2. Back buffer. На virtio-gpu экран меняется ТОЛЬКО по SYS_FB_PRESENT —
     * промежуточные состояния компоновки никогда не видны, поэтому рисуем
     * СРАЗУ в backing (bb = fb): экономим целый проход bb→fb по площади
     * damage на каждый кадр (на эмулируемом CPU это ~треть кадра при drag).
     * Отдельный bb нужен только на Limine-пути (scanout читает память
     * непрерывно — рисовать в него слоями нельзя) или при pitch != width. */
    uint64_t fb_caps = vos_fb_caps();
    if (fb_caps == (uint64_t)-1) fb_caps = 0;   /* старое ядро без SYS_FB_CAPS */
    if ((fb_caps & VOS_FB_CAP_OFFSCREEN) && fb_stride == fbw) {
        bb = fb;
        puts("vwm: direct compose into virtio backing (no back buffer)\n");
    } else {
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
    }

    /* 2b. Обои: ещё один shm-сегмент, рендерим один раз. Не выделились —
     * не страшно: fill_wall откатится на плоский DESK_BG. */
    wall_shm = vos_shm_create((uint64_t)fbw * fbh * 4);
    if (wall_shm != (uint64_t)-1) {
        wall = (uint32_t *)vos_shm_map(wall_shm);
        if (wall) wall_render();
    }

    /* 3. Становимся WM: сервис + весь ввод наш */
    shadow_lut_init();
    vfont_ui_init();   /* AdwaitaSans для заголовков окон и тултипов дока; 0 -> fallback font8x16 */
    vos_svc_register(VOS_SVC_WM);
    vos_input_grab();

    mouse_x = (int)fbw / 2;
    mouse_y = (int)fbh / 2;

    /* HW-курсор через virtio-gpu cursorq. cursor_move шлёт UPDATE_CURSOR
     * (не MOVE) с resource_id — это needed для visibility на QEMU. */
    hw_cursor_init();

    puts("vwm: userspace window manager up\n");

    /* 4. Первый кадр. vpanel теперь запускает vinit (/etc/vinit/20-panel.svc),
     * автостарт терминала убран по просьбе пользователя — Terminal
     * запускается кликом в dock. */
    render_all();

    /* 5. Event loop: ipc_recv — и очередь событий, и таймер кадра.
     * Спим максимум 1 тик; кадр рисуем не чаще раза в 2 тика (~50 FPS). */
    uint64_t last_frame = 0;
    vos_msg_t m;
    for (;;) {
        /* В покое — ждём 1 тик (10ms) на ipc_recv. На drag/resize — НЕ ждём:
         * mouse-events прилетают быстрее тика, и сон между ними даёт лаг
         * окна за курсором. NOWAIT даёт busy-loop, но фактически блокируется
         * на rendered_frame (~30 FPS), CPU не сгорит. */
        int wait = (drag.active || rz.active) ? VOS_IPC_NOWAIT : 1;
        int got = (int)vos_ipc_recv(&m, wait);
        while (got) {
            handle_msg(&m);
            got = (int)vos_ipc_recv(&m, VOS_IPC_NOWAIT);  /* выгребаем всё */
        }
        uint64_t now = vos_uptime();
        /* На drag/resize рендерим максимально часто — окно следует за
         * курсором без задержки. В покое — раз в 2 тика. */
        int cursor_hot = !hw_cursor_ok && cursor_moved;
        int hot = drag.active || rz.active || anim_any_active() ||
                  cursor_hot || needs_redraw || panel_dirty || dock_dirty;
        uint64_t interval = (drag.active || rz.active) ? 0 : (hot ? 1 : 2);
        if (now - last_frame >= interval) {
            tick_render();
            last_frame = now;
        }
    }
}
