/* =============================================================================
 * VortexOS — userspace/vlogin.c
 * Lock-screen в стиле macOS Tahoe: обои (та же процедурная заливка, что у
 * vwm) + большие часы сверху + аватарка/имя/пароль внизу.
 *
 * Пароль (если задан) — простой FNV-1a hash в /etc/shadow одной строкой.
 * Нет файла / пусто — вход по клику или Enter.
 * ============================================================================= */
#include "vos_abi.h"
#include "font8x16.h"
#include "vfont.h"
#include "vfont_ui.h"

#define FG       0xFFFFFFFFu
#define DIM      0xC0E8EAF2u   /* приглушённый текст с прозрачностью */
#define ACCENT   0xFF5B8CFFu
#define ERR_COL  0xFFFF5F56u

static uint64_t  wm_pid = 0;
static uint64_t  win_id = 0;
static uint32_t *surf  = 0;
static int W = 1024, H = 768;

static char    pwd_hash[96];
static int     pwd_required;
static char    input[64];
static int     inlen = 0;
static int     bad_attempt = 0;
static uint64_t last_clock_sec = (uint64_t)-1;
static char    last_clock_str[8] = {0};

/* Кешированный фон (обои + затемнение). Рендерится один раз при старте/
 * resize, дальше render() копирует из него только меняющиеся зоны (часы,
 * поле пароля). Без этого каждый tick перерисовывались 800k пикселей —
 * мерцание ровно по часам. */
static uint32_t *bg_cache = 0;
static int       bg_cache_w = 0, bg_cache_h = 0;

/* Большой шрифт для часов (загружается отдельно от vfont_ui). vfont_t —
 * 256 KB + struct, держим один экземпляр-копию: bake → memcpy → reload
 * vfont_ui под обычный размер. */
static vfont_t g_clock_font_buf __attribute__((aligned(16)));
static vfont_t *g_clock_font = 0;

/* ----- мини-libc ----- */
static int s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void *m_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

/* ----- примитивы ----- */
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) continue;
        uint32_t *row = surf + (uint32_t)py * W;
        for (int i = 0; i < w; i++) {
            int px_ = x + i;
            if (px_ < 0 || px_ >= W) continue;
            row[px_] = c;
        }
    }
}
static void blend_pixel(int x, int y, uint32_t argb) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint32_t a = (argb >> 24) & 0xFF;
    if (!a) return;
    uint32_t *p = &surf[(uint32_t)y * W + x];
    if (a == 0xFF) { *p = argb; return; }
    uint32_t d = *p;
    uint32_t fr = (argb >> 16) & 0xFF, fg = (argb >> 8) & 0xFF, fb = argb & 0xFF;
    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    uint32_t r = (fr * a + dr * (255 - a)) / 255;
    uint32_t g = (fg * a + dg * (255 - a)) / 255;
    uint32_t b = (fb * a + db * (255 - a)) / 255;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}
static void fill_circle(int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++) {
        int span = 0;
        while (span * span + j * j <= r * r) span++;
        span--;
        if (span <= 0) continue;
        fill_rect(cx - span, cy + j, span * 2 + 1, 1, c);
    }
}

/* ----- та же процедурная заливка обоев что в vwm ----- */
static const unsigned char bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static void render_wallpaper(void) {
    const int r0 = 0x0E, g0 = 0x11, b0 = 0x24;
    const int r1 = 0x2C, g1 = 0x16, b1 = 0x46;
    int gx1 = W * 82 / 100, gy1 = H * 6 / 100;
    int gx2 = W * 10 / 100, gy2 = H * 96 / 100;
    int R1 = W * 55 / 100, R1sq = R1 * R1;
    int R2 = W * 42 / 100, R2sq = R2 * R2;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int t = x * 128 / (W - 1) + y * 128 / (H - 1);
            int rf = r0 * (256 - t) + r1 * t;
            int gf = g0 * (256 - t) + g1 * t;
            int bf = b0 * (256 - t) + b1 * t;
            int dx = x - gx1, dy = y - gy1;
            int d2 = dx * dx + dy * dy;
            if (d2 < R1sq) {
                int a = (((int64_t)(R1sq - d2)) << 8) / R1sq;
                a = a * a >> 8;
                rf += 0x10 * a; gf += 0x26 * a; bf += 0x5E * a;
            }
            dx = x - gx2; dy = y - gy2;
            d2 = dx * dx + dy * dy;
            if (d2 < R2sq) {
                int a = (((int64_t)(R2sq - d2)) << 8) / R2sq;
                a = a * a >> 8;
                rf += 0x06 * a; gf += 0x30 * a; bf += 0x2E * a;
            }
            int dth = bayer4[y & 3][x & 3] * 16;
            int r = (rf + dth) >> 8; if (r > 255) r = 255;
            int g = (gf + dth) >> 8; if (g > 255) g = 255;
            int b = (bf + dth) >> 8; if (b > 255) b = 255;
            surf[(uint32_t)y * W + x] =
                0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* Простое затемнение всей поверхности (макос-style — обои чуть притушены
 * на lock screen для контраста с белым текстом). */
static void darken_overlay(int amount) {
    for (int j = 0; j < H; j++) {
        uint32_t *row = surf + (uint32_t)j * W;
        for (int i = 0; i < W; i++) {
            uint32_t p = row[i];
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            r = r * (255 - amount) / 255;
            g = g * (255 - amount) / 255;
            b = b * (255 - amount) / 255;
            row[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ----- текст ----- */
static int text_w_with(vfont_t *f, const char *s) {
    if (!f) return s_len(s) * 8;
    int w = 0;
    while (*s) {
        int cp = (unsigned char)*s++;
        if (cp < VF_FIRST || cp >= VF_FIRST + VF_COUNT) w += f->ch_w;
        else w += (int)(f->glyphs[cp - VF_FIRST].xadvance + 0.5f);
    }
    return w;
}
static void draw_text(int x, int y, const char *s, uint32_t fg) {
    if (vfont_ui) vfont_draw(surf, W, H, x, y, s, fg, 0, vfont_ui);
    else {
        int cx = x;
        while (*s) {
            uint8_t idx = (uint8_t)*s;
            if (idx >= 128) idx = '?';
            const unsigned char *gl = vos_font[idx];
            for (int row = 0; row < 16; row++) {
                int py = y + row;
                if (py < 0 || py >= H) continue;
                uint8_t bits = gl[row];
                uint32_t *line = surf + (uint32_t)py * W;
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int px_ = cx + col;
                    if (px_ >= 0 && px_ < W) line[px_] = fg;
                }
            }
            cx += 8; s++;
        }
    }
}
static int text_w(const char *s) { return text_w_with(vfont_ui, s); }
static int text_h(void) { return vfont_ui ? vfont_ui->ch_h : 16; }

/* ----- хеш паролей ----- */
static void simple_hash(const char *s, char out[64]) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x100000001b3ULL; }
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hex[(h >> (60 - i * 4)) & 0xF];
    out[16] = 0;
}

static void load_password(void) {
    int64_t n = vos_fs_read("/etc/shadow", 0, (uint8_t*)pwd_hash, sizeof(pwd_hash) - 1);
    if (n <= 0) { pwd_required = 0; pwd_hash[0] = 0; return; }
    pwd_hash[n] = 0;
    while (n > 0 && (pwd_hash[n-1] == '\n' || pwd_hash[n-1] == '\r' ||
                     pwd_hash[n-1] == ' ')) pwd_hash[--n] = 0;
    pwd_required = (n > 0);
}

/* ----- bake двух размеров шрифта -----
 * vfont.h использует один глобальный vfont_instance: грузим 56px, копируем
 * целиком, потом грузим 14px поверх — итог: g_clock_font (большой) +
 * vfont_ui (обычный) живут параллельно. */
static void load_fonts(void) {
    vfont_t *big = vfont_load("/etc/fonts/AdwaitaSans-Regular.ttf", 56.0f);
    if (big && big->ok) {
        m_memcpy(&g_clock_font_buf, big, sizeof(vfont_t));
        g_clock_font = &g_clock_font_buf;
    }
    /* обычный размер для остального текста */
    vfont_ui_init();
}

/* ----- большой текст из g_clock_font (свой mini vfont_draw) ----- */
static int big_text_w(const char *s) { return text_w_with(g_clock_font, s); }
static int big_text_h(void) { return g_clock_font ? g_clock_font->ch_h : 56; }

static void draw_big_text(int x, int y, const char *s, uint32_t fg) {
    if (!g_clock_font || !g_clock_font->ok) {
        draw_text(x, y, s, fg);
        return;
    }
    vfont_draw(surf, W, H, x, y, s, fg, 0, g_clock_font);
}

/* ----- RTC -----
 * vos_rtc(uint32_t hms[3]) — часы/минуты/секунды локального времени. */
static void make_clock(char *out) {
    uint32_t hms[3] = {0, 0, 0};
    vos_rtc(hms);
    out[0] = '0' + (hms[0] / 10);
    out[1] = '0' + (hms[0] % 10);
    out[2] = ':';
    out[3] = '0' + (hms[1] / 10);
    out[4] = '0' + (hms[1] % 10);
    out[5] = 0;
}

/* Перезалить bg_cache (вызывается раз — при старте или после resize). */
static void rebuild_bg_cache(void) {
    if (!bg_cache || bg_cache_w != W || bg_cache_h != H) {
        /* Лимит размера: 1920×1200×4 = ~9 MB. Если bg_cache не выделен,
         * используем static-буфер в BSS. */
        static uint32_t s_buf[1920 * 1200];
        if ((uint64_t)W * H > 1920ULL * 1200ULL) {
            bg_cache = 0;   /* fallback: рендерим каждый кадр */
            return;
        }
        bg_cache = s_buf;
        bg_cache_w = W; bg_cache_h = H;
    }
    /* временно подменяем surf, чтобы переиспользовать render_wallpaper. */
    uint32_t *real = surf;
    surf = bg_cache;
    render_wallpaper();
    darken_overlay(80);
    surf = real;
}

/* Восстановить кусок фона из bg_cache в surf. */
static void blit_bg(int x, int y, int w, int h) {
    if (!bg_cache) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t *s = &bg_cache[(uint32_t)(y + j) * bg_cache_w + x];
        uint32_t *d = &surf[(uint32_t)(y + j) * W + x];
        for (int i = 0; i < w; i++) d[i] = s[i];
    }
}

/* Полная перерисовка всего экрана (старт, resize, плохой пароль и т.п.). */
static void full_render(void) {
    if (!bg_cache) {
        render_wallpaper();
        darken_overlay(80);
    } else {
        blit_bg(0, 0, W, H);
    }

    int cx = W / 2;

    /* Часы — крупно по центру в верхней трети */
    char clk[8];
    make_clock(clk);
    int cw = big_text_w(clk);
    int ch = big_text_h();
    int clock_y = H * 14 / 100;
    draw_big_text(cx - cw / 2, clock_y, clk, FG);

    /* Подпись «VortexOS» под часами мелко */
    {
        const char *brand = "VortexOS";
        int bw = text_w(brand);
        draw_text(cx - bw / 2, clock_y + ch + 6, brand, DIM);
    }

    /* Нижняя зона: аватар + имя + поле/подсказка */
    int bottom_y = H * 78 / 100;
    int r = 36;
    fill_circle(cx, bottom_y, r + 2, 0xFFE8EAF2u);
    fill_circle(cx, bottom_y, r, ACCENT);
    {
        const char *initial = "V";
        int iw = text_w(initial);
        draw_text(cx - iw / 2, bottom_y - text_h() / 2 + 1, initial, 0xFFFFFFFFu);
    }

    const char *name = "root";
    int nw = text_w(name);
    draw_text(cx - nw / 2, bottom_y + r + 12, name, FG);

    int y_below = bottom_y + r + 12 + text_h() + 14;

    if (pwd_required) {
        /* Овальное поле пароля, узкое, под именем */
        int fw = 240;
        int fh = 32;
        int fx = cx - fw / 2;
        int fy = y_below;
        /* подложка с лёгкой прозрачностью */
        for (int j = 0; j < fh; j++)
            for (int i = 0; i < fw; i++)
                blend_pixel(fx + i, fy + j, 0x661A1E2Au);
        /* border (тонкая рамка) */
        for (int i = 0; i < fw; i++) {
            blend_pixel(fx + i, fy,          0x80FFFFFFu);
            blend_pixel(fx + i, fy + fh - 1, 0x40FFFFFFu);
        }
        for (int j = 0; j < fh; j++) {
            blend_pixel(fx,          fy + j, 0x60FFFFFFu);
            blend_pixel(fx + fw - 1, fy + j, 0x60FFFFFFu);
        }

        if (inlen == 0) {
            const char *ph = "Enter Password";
            int pw = text_w(ph);
            draw_text(cx - pw / 2, fy + (fh - text_h()) / 2, ph, DIM);
        } else {
            char mask[64];
            int n = inlen < 63 ? inlen : 63;
            for (int i = 0; i < n; i++) mask[i] = '*';
            mask[n] = 0;
            int mw = text_w(mask);
            draw_text(cx - mw / 2, fy + (fh - text_h()) / 2, mask, FG);
        }
        if (bad_attempt) {
            const char *e = "Incorrect password";
            int ew = text_w(e);
            draw_text(cx - ew / 2, fy + fh + 10, e, ERR_COL);
        }
    } else {
        const char *hint = "Click or press Enter to continue";
        int hw = text_w(hint);
        draw_text(cx - hw / 2, y_below, hint, DIM);
    }

    vwm_commit(wm_pid, win_id, 0, 0, W, H);
}

/* Обновить ТОЛЬКО зону часов (вызывается раз в секунду). Восстанавливаем
 * фон под старой шириной строки часов и рисуем новую. damage-rect ровно
 * по этой полосе — vwm копирует ~6 KB вместо целого окна. */
static void tick_clock(void) {
    if (!bg_cache) { full_render(); return; }
    int cx = W / 2;
    int clock_y = H * 14 / 100;
    int ch = big_text_h();

    /* Берём максимальную ширину часов как «99:99» — стабильный диапазон, чтобы
     * не оставалось хвостов от прошлых цифр. */
    int wide = big_text_w("99:99");
    int x = cx - wide / 2 - 4;
    int y = clock_y - 4;
    int w = wide + 8;
    int h = ch + 8;
    blit_bg(x, y, w, h);

    char clk[8];
    make_clock(clk);
    int cw = big_text_w(clk);
    draw_big_text(cx - cw / 2, clock_y, clk, FG);

    /* строка «VortexOS» под часами не меняется — её НЕ перерисовываем. */

    vwm_commit(wm_pid, win_id, x, y, w, h);
}

static void unlock_and_quit(void) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_UNLOCK;
    vos_ipc_send(wm_pid, &m);
    exit(0);
}

static void try_login(void) {
    if (!pwd_required) { unlock_and_quit(); }
    char h[64];
    input[inlen] = 0;
    simple_hash(input, h);
    if (s_eq(h, pwd_hash)) { unlock_and_quit(); }
    bad_attempt = 1;
    inlen = 0;
}

void _start(void) {
    wm_pid = vwm_wait_for_wm();
    load_fonts();
    load_password();

    struct { uint64_t phys; uint32_t w, h, pitch, bpp; } info;
    syscall1(SYS_FB_INFO, (uint64_t)&info);
    if (info.w && info.h) { W = (int)info.w; H = (int)info.h; }

    win_id = vwm_create_window(wm_pid, "vlogin", W, H, &surf);
    if (!win_id) { puts("vlogin: no window\n"); exit(1); }

    /* lock сцену */
    {
        vos_msg_t m;
        for (int i = 0; i < 8; i++) m.w[i] = 0;
        m.w[0] = VWM_LOCK;
        m.w[1] = win_id;
        vos_ipc_send(wm_pid, &m);
    }

    rebuild_bg_cache();
    full_render();
    last_clock_sec = vos_uptime() / 100;
    make_clock(last_clock_str);

    vos_msg_t m;
    for (;;) {
        /* Ждём максимум 25 тиков — обновляем часы раз в секунду. */
        int got = (int)vos_ipc_recv(&m, 25);
        int dirty = 0;
        while (got) {
            switch (m.w[0]) {
            case VWM_EV_KEY:
                if (m.w[1] == win_id && m.w[3]) {
                    char c = (char)(m.w[2] & 0xFF);
                    if (c == '\n' || c == '\r') {
                        try_login();
                        dirty = 1;
                    } else if (c == '\b' || c == 127) {
                        if (inlen > 0) inlen--;
                        bad_attempt = 0;
                        dirty = 1;
                    } else if (c >= 32 && c < 127 && pwd_required) {
                        if (inlen < (int)sizeof(input) - 1) input[inlen++] = c;
                        bad_attempt = 0;
                        dirty = 1;
                    }
                }
                break;
            case VWM_EV_MOUSE:
                if (m.w[1] == win_id && (m.w[4] & 1) && !pwd_required) {
                    try_login();
                }
                break;
            case VWM_EV_RESIZE:
                if (m.w[1] == win_id) {
                    W = (int)m.w[2];
                    H = (int)m.w[3];
                    rebuild_bg_cache();
                    dirty = 1;
                }
                break;
            case VWM_EV_CLOSE:
                exit(0);
                break;
            }
            got = (int)vos_ipc_recv(&m, VOS_IPC_NOWAIT);
        }
        /* Тик часов: если строка действительно изменилась — узкий
         * commit только зоны часов. dirty (ввод/resize/попытка входа) —
         * полная перерисовка. */
        if (dirty) {
            full_render();
            last_clock_sec = vos_uptime() / 100;
            make_clock(last_clock_str);
        } else {
            uint64_t sec = vos_uptime() / 100;
            if (sec != last_clock_sec) {
                char now[8];
                make_clock(now);
                last_clock_sec = sec;
                if (!s_eq(now, last_clock_str)) {
                    for (int i = 0; i < 8; i++) last_clock_str[i] = now[i];
                    tick_clock();
                }
            }
        }
    }
}
