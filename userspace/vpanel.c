/* =============================================================================
 * VortexOS — userspace/vpanel.c
 * vpanel — верхняя панель как ОТДЕЛЬНЫЙ ring3-процесс (вынесена из vwm).
 * Это НЕ обычное окно: своя shm-поверхность во всю ширину экрана высотой
 * VWM_PANEL_H, vwm блендит её поверх обоев per-pixel alpha (без титлбара,
 * фокуса и z-порядка). Протокол: VWM_PANEL_* в vos_abi.h.
 *
 * Рисует: логотип, имя ОС, заголовок активного окна, чипы-таскбар ВСЕХ окон
 * (фокусное подсвечено, свёрнутые приглушены; клик -> VWM_PANEL_ACTIVATE)
 * и часы (SYS_RTC). Список окон присылает vwm (VWM_PANEL_WINS), клики в
 * панель форвардит VWM_PANEL_CLICK.
 *
 * Поверхность ARGB НЕпремультиплицированная: фон полупрозрачный
 * (0xD20D0E15 — как старая плашка vwm), текст/чипы блендятся локально
 * поверх базы, альфа аккумулируется (src-over).
 * ============================================================================= */

#include "vos_abi.h"
#include "font8x16.h"

#define ACCENT     0xFF5B8CFF
#define BASE_BG    0xD20D0E15u
#define LINE_BG    (0xB0000000u | (ACCENT & 0x00FFFFFF))

#define CHIP_H        18
#define CHIP_TITLE_MAX 12
#define MAX_WINS      16

static uint32_t *surf = 0;
static int W = 0, H = 0;
static uint64_t wm_pid = 0;

/* ---- копия списка окон от vwm (staging -> active по последнему msg) ---- */
typedef struct { uint64_t id; int minimized, focused; char title[24]; } pwin_t;
static pwin_t wins[MAX_WINS], stage[MAX_WINS];
static int nwins = 0;

/* --------------------- рисование в ARGB-поверхность --------------------- */
static void px_over(int x, int y, uint32_t src) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint32_t sa = src >> 24;
    if (sa == 0) return;
    uint32_t *p = &surf[(uint32_t)y * W + x];
    if (sa == 255) { *p = src; return; }
    uint32_t d = *p;
    uint32_t da = d >> 24;
    uint32_t inv = 255 - sa;
    uint32_t r = ((src >> 16 & 0xFF) * sa + (d >> 16 & 0xFF) * inv) / 255;
    uint32_t g = ((src >> 8 & 0xFF) * sa + (d >> 8 & 0xFF) * inv) / 255;
    uint32_t b = ((src & 0xFF) * sa + (d & 0xFF) * inv) / 255;
    uint32_t a = sa + da * inv / 255;
    *p = (a << 24) | (r << 16) | (g << 8) | b;
}

static void fill_over(int x, int y, int w, int h, uint32_t src) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px_over(x + i, y + j, src);
}

/* скруглённый прямоугольник без AA (панель маленькая — лесенки не видно) */
static void fill_round(int x, int y, int w, int h, int r, uint32_t src) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int cx = -1, cy = -1;
            if (i < r)           cx = r;     else if (i >= w - r) cx = w - r - 1;
            if (j < r)           cy = r;     else if (j >= h - r) cy = h - r - 1;
            if (cx >= 0 && cy >= 0) {
                int dx = i - cx, dy = j - cy;
                if (dx * dx + dy * dy > r * r) continue;
            }
            px_over(x + i, y + j, src);
        }
}

static void draw_text(int x, int y, const char *s, uint32_t fg) {
    int cx = x;
    while (*s) {
        uint8_t idx = (uint8_t)*s;
        if (idx >= 128) idx = '?';
        const unsigned char *glyph = vos_font[idx];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) px_over(cx + col, y + row, fg);
        }
        cx += 8;
        s++;
    }
}

/* --------------------- раскладка чипов (как в старом vwm) ---------------- */
typedef struct { int x, w; uint64_t id; } chip_t;

static int chips_layout(chip_t *out) {
    int n = 0;
    for (int i = 0; i < nwins && n < MAX_WINS; i++) {
        int len = 0;
        while (wins[i].title[len] && len < CHIP_TITLE_MAX) len++;
        if (len == 0) len = 1;
        out[n].w = len * 8 + 14;
        out[n].id = wins[i].id;
        n++;
    }
    int x = W - 8 * 8 - 24;            /* правый край пачки — перед часами */
    for (int k = n - 1; k >= 0; k--) { x -= out[k].w + 6; out[k].x = x; }
    return n;
}

/* --------------------- кадр --------------------- */
static void compose(void) {
    for (int j = 0; j < H - 1; j++)
        for (int i = 0; i < W; i++)
            surf[(uint32_t)j * W + i] = BASE_BG;
    for (int i = 0; i < W; i++)
        surf[(uint32_t)(H - 1) * W + i] = LINE_BG;

    int ty = (H - 16) / 2;
    fill_round(8, ty, 16, 16, 5, ACCENT);
    draw_text(12, ty, "V", 0xFFFFFFFF);
    int lx = 30;
    draw_text(lx, ty, "VortexOS", 0xFFE8EAF2);
    lx += 8 * 8;

    for (int i = 0; i < nwins; i++)
        if (wins[i].focused && wins[i].title[0]) {
            fill_over(lx + 8, ty + 1, 1, 14, 0xFF3A3F55);
            draw_text(lx + 16, ty, wins[i].title, 0xFFAFC6E8);
            break;
        }

    /* чипы-таскбар: все окна; фокусное — акцент, свёрнутые — приглушены */
    chip_t chips[MAX_WINS];
    int n = chips_layout(chips);
    int cy = (H - CHIP_H) / 2;
    for (int k = 0; k < n; k++) {
        const pwin_t *w = &wins[k];
        uint32_t bg = w->focused ? 0xFF31427A : (w->minimized ? 0xC42A2E3E : 0xE63A4156);
        fill_round(chips[k].x, cy, chips[k].w, CHIP_H, 9, bg);
        char buf[CHIP_TITLE_MAX + 1];
        int i = 0;
        while (w->title[i] && i < CHIP_TITLE_MAX) { buf[i] = w->title[i]; i++; }
        buf[i] = 0;
        draw_text(chips[k].x + 7, (H - 16) / 2, buf,
                  w->minimized ? 0xFF8B93A8 : 0xFFBDD0F0);
    }

    /* часы */
    uint32_t hms[3];
    vos_rtc(hms);
    char clk[9];
    clk[0] = '0' + hms[0] / 10; clk[1] = '0' + hms[0] % 10; clk[2] = ':';
    clk[3] = '0' + hms[1] / 10; clk[4] = '0' + hms[1] % 10; clk[5] = ':';
    clk[6] = '0' + hms[2] / 10; clk[7] = '0' + hms[2] % 10; clk[8] = 0;
    draw_text(W - 8 * 8 - 10, ty, clk, 0xFFF2F4FA);
}

static void commit_all(void) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_PANEL_COMMIT;
    m.w[1] = 0; m.w[2] = 0; m.w[3] = (uint64_t)W; m.w[4] = (uint64_t)H;
    vos_ipc_send(wm_pid, &m);
}

/* --------------------- события --------------------- */
static void on_wins_msg(vos_msg_t *m) {
    uint64_t idx = m->w[2] >> 32;
    uint64_t count = m->w[2] & 0xFFFFFFFFu;
    if (count > MAX_WINS) count = MAX_WINS;
    if (count == 0) { nwins = 0; return; }
    if (idx < count) {
        pwin_t *w = &stage[idx];
        w->id = m->w[1];
        w->minimized = (int)(m->w[3] & 1);
        w->focused   = (int)((m->w[3] >> 1) & 1);
        const char *t = (const char *)&m->w[4];
        int i = 0;
        while (t[i] && i < 23) { w->title[i] = t[i]; i++; }
        w->title[i] = 0;
    }
    if (idx == count - 1) {            /* батч пришёл целиком — применяем */
        for (uint64_t k = 0; k < count; k++) wins[k] = stage[k];
        nwins = (int)count;
    }
}

static void on_click(int mx, int my, int buttons) {
    if (!(buttons & 1)) return;
    chip_t chips[MAX_WINS];
    int n = chips_layout(chips);
    int cy = (H - CHIP_H) / 2;
    if (my < cy || my >= cy + CHIP_H) return;
    for (int k = 0; k < n; k++)
        if (mx >= chips[k].x && mx < chips[k].x + chips[k].w) {
            vos_msg_t m;
            for (int i = 0; i < 8; i++) m.w[i] = 0;
            m.w[0] = VWM_PANEL_ACTIVATE;
            m.w[1] = chips[k].id;
            vos_ipc_send(wm_pid, &m);
            return;
        }
}

/* --------------------- main --------------------- */
void _start(void) {
    struct { uint64_t phys; uint32_t w, h, pitch, bpp; } info;
    syscall1(SYS_FB_INFO, (uint64_t)&info);
    if (!info.w) { puts("vpanel: no framebuffer\n"); exit(1); }
    W = (int)info.w;
    H = VWM_PANEL_H;

    uint64_t shm = vos_shm_create((uint64_t)W * H * 4);
    if (shm == (uint64_t)-1) { puts("vpanel: shm_create failed\n"); exit(1); }
    surf = (uint32_t *)vos_shm_map(shm);
    if (!surf) { puts("vpanel: shm_map failed\n"); exit(1); }

    wm_pid = vwm_wait_for_wm();

    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_PANEL_ATTACH;
    m.w[2] = shm;
    vos_ipc_send(wm_pid, &m);

    /* ждём подтверждение (vwm замапил поверхность) */
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        if (m.w[0] == VWM_PANEL_OK) {
            if (!m.w[1]) { puts("vpanel: wm refused\n"); exit(1); }
            W = (int)(m.w[1] >> 32);
            H = (int)(m.w[1] & 0xFFFFFFFFu);
            break;
        }
        if (m.w[0] == VWM_PANEL_WINS) on_wins_msg(&m);
    }

    compose();
    commit_all();

    uint64_t last_sec = (uint64_t)-1;
    for (;;) {
        int got = (int)vos_ipc_recv(&m, 25);   /* тиков: часы тикают ~4 Гц */
        int dirty = 0;
        while (got) {
            switch (m.w[0]) {
            case VWM_PANEL_WINS:
                on_wins_msg(&m);
                dirty = 1;
                break;
            case VWM_PANEL_CLICK:
                on_click((int)m.w[1], (int)m.w[2], (int)m.w[3]);
                break;
            default:
                break;
            }
            got = (int)vos_ipc_recv(&m, VOS_IPC_NOWAIT);
        }
        uint64_t sec = vos_uptime() / 100;
        if (sec != last_sec) { last_sec = sec; dirty = 1; }
        if (dirty) { compose(); commit_all(); }
    }
}
