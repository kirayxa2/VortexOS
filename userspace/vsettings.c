/* =============================================================================
 * VortexOS — userspace/vsettings.c
 * vsettings — «Настройки», GNOME-style: sidebar слева с категориями, контент
 * справа. Разделы:
 *
 *   About        — версия ОС, ядро, uptime
 *   Display      — смена разрешения (как было — переехало в content pane)
 *   Appearance   — выбор темы/акцент-цвета/шрифтов (UI готов, не сохраняется)
 *   System Info  — диагностика: arch, virtio-gpu, mem-totals
 *   Login        — пароль на вход (toggle); хранение в /etc/shadow (TODO)
 *
 * Большая часть Appearance/Login пока заглушки — UI на месте, бэкенд позже.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <vui.h>

#define START_W 760
#define START_H 520
#define SIDEBAR_W 220

/* ----- разделы ----- */
enum {
    SEC_ABOUT = 0,
    SEC_DISPLAY,
    SEC_APPEARANCE,
    SEC_SYSINFO,
    SEC_LOGIN,
    NSEC
};
static const char *section_names[NSEC] = {
    "About", "Display", "Appearance", "System Info", "Login"
};
static int current_section = SEC_ABOUT;

/* ----- разрешения ----- */
typedef struct { int w, h; } mode_t_;
static const mode_t_ modes[] = {
    {  800,  600 }, { 1024,  768 }, { 1280,  720 }, { 1280,  800 },
    { 1366,  768 }, { 1440,  900 }, { 1600,  900 }, { 1920, 1080 },
};
#define NMODES ((int)(sizeof(modes) / sizeof(modes[0])))

static vui_win_t *win;
static int display_err = 0;
static int display_pending = -1;

/* Appearance заглушки */
static int accent_idx = 0;
static const uint32_t accent_colors[] = {
    0xFF5B8CFF,  /* синий — текущий */
    0xFF3CB371,  /* зелёный */
    0xFFE8B64C,  /* жёлтый */
    0xFFD9534F,  /* красный */
    0xFFB57EDC,  /* фиолетовый */
    0xFF7A7A9A,  /* серый */
};
#define NACCENT ((int)(sizeof(accent_colors) / sizeof(accent_colors[0])))

/* Login заглушки */
static int login_password_enabled = 0;

/* ---------------------------------------------------------------------------
 * Текущее разрешение из ядра.
 * ------------------------------------------------------------------------- */
static void cur_mode(int *w, int *h) {
    struct { unsigned long long phys; unsigned int w, h, pitch, bpp; } info;
    info.w = info.h = 0;
    syscall1(SYS_FB_INFO, (unsigned long long)&info);
    *w = (int)info.w; *h = (int)info.h;
}

/* ---------------------------------------------------------------------------
 * Sidebar item — кастомная плашка категории (vui_button даёт frame, который
 * нам тут не нужен — sidebar выглядит ровно когда выделение «заливкой»).
 * ------------------------------------------------------------------------- */
static int sidebar_item(int x, int y, int w, int h, const char *label, int active) {
    int hit = vui_click_in(win, x, y, w, h);
    if (hit) win->click_pending = 0;

    if (active) {
        vui_rect(win, x + 8, y + 4, w - 16, h - 8, accent_colors[accent_idx]);
    } else if (hit) {
        vui_rect(win, x + 8, y + 4, w - 16, h - 8, 0xFF3A3A4A);
    }
    vui_text(win, x + 22, y + (h - VUI_TEXT_H) / 2, label,
             active ? 0xFFFFFFFF : VUI_COL_FG);
    return hit;
}

/* ---------------------------------------------------------------------------
 * Колор-кружок для Appearance (накладываем простой круг — без AA, мелко но
 * читаемо). Возвращает 1 при клике.
 * ------------------------------------------------------------------------- */
/* Залить круг hline-полосами (vui_hline сам делает damage_add по строке). */
static void fill_circle_inline(int cx, int cy, int r, uint32_t color) {
    for (int j = -r; j <= r; j++) {
        int span = 0;
        while (span * span + j * j <= r * r) span++;
        span--;
        if (span <= 0) continue;
        vui_hline(win, cx - span, cy + j, span * 2 + 1, color);
    }
}

static int color_dot(int cx, int cy, int r, uint32_t color, int selected) {
    int hit = vui_click_in(win, cx - r - 4, cy - r - 4, (r + 4) * 2, (r + 4) * 2);
    if (hit) win->click_pending = 0;
    if (selected) {
        /* Сначала белая «подложка» большего диаметра: дешёвый способ
         * нарисовать кольцо вокруг главного кружка. */
        fill_circle_inline(cx, cy, r + 3, 0xFFFFFFFF);
    }
    fill_circle_inline(cx, cy, r, color);
    return hit;
}

/* ===========================================================================
 * Рендер контент-секций
 * ========================================================================= */
static void render_about(int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    vui_text(win, cx + 20, cy + 20, "About VortexOS", VUI_COL_FG);

    /* большая «V» как логотип */
    int lx = cx + 30, ly = cy + 60;
    vui_rect(win, lx, ly, 60, 60, accent_colors[accent_idx]);
    vui_text(win, lx + 22, ly + 22, "V", 0xFFFFFFFF);

    int tx = cx + 110;
    vui_text(win, tx, cy + 60,  "VortexOS",                 VUI_COL_FG);
    vui_text(win, tx, cy + 82,  "Version 0.4 (development)", VUI_COL_DIM);
    vui_text(win, tx, cy + 102, "Kernel: VOS x86_64",       VUI_COL_DIM);

    int y = cy + 160;
    vui_text(win, cx + 20, y, "System", VUI_COL_DIM); y += 22;
    char buf[64];
    int dw, dh; cur_mode(&dw, &dh);
    snprintf(buf, sizeof(buf), "Display:   %d x %d", dw, dh);
    vui_text(win, cx + 20, y, buf, VUI_COL_FG); y += 18;

    unsigned long long up = vos_uptime();   /* PIT-тики 100Гц */
    unsigned long long sec = up / 100;
    snprintf(buf, sizeof(buf), "Uptime:    %llu m %llu s", sec / 60, sec % 60);
    vui_text(win, cx + 20, y, buf, VUI_COL_FG); y += 18;

    snprintf(buf, sizeof(buf), "Compositor: vwm (userspace)");
    vui_text(win, cx + 20, y, buf, VUI_COL_FG); y += 18;

    snprintf(buf, sizeof(buf), "Window protocol: VWM IPC");
    vui_text(win, cx + 20, y, buf, VUI_COL_FG);
}

static void render_display(int cx, int cy, int cw, int ch) {
    (void)ch;
    vui_text(win, cx + 20, cy + 20, "Display", VUI_COL_FG);

    int dw, dh; cur_mode(&dw, &dh);
    char buf[64];
    snprintf(buf, sizeof(buf), "Current: %d x %d", dw, dh);
    vui_text(win, cx + 20, cy + 50, buf, VUI_COL_OK);

    vui_text(win, cx + 20, cy + 80, "Resolution", VUI_COL_DIM);

    int bw = (cw - 20 * 2 - 12) / 2;
    int bh = 30;
    for (int i = 0; i < NMODES; i++) {
        int col = i % 2, row = i / 2;
        int x = cx + 20 + col * (bw + 12);
        int y = cy + 104 + row * (bh + 10);
        int active = (modes[i].w == dw && modes[i].h == dh);
        snprintf(buf, sizeof(buf), "%d x %d", modes[i].w, modes[i].h);
        if (vui_button(win, x, y, bw, bh, buf,
                       active ? accent_colors[accent_idx] : VUI_COL_BTN,
                       VUI_COL_FG))
            display_pending = i;
    }

    int y = cy + 104 + ((NMODES + 1) / 2) * (bh + 10) + 14;
    if (display_err) {
        vui_text(win, cx + 20, y,      "Can't switch mode here :(", VUI_COL_ERR);
        vui_text(win, cx + 20, y + 20, "Resolution switching needs virtio-gpu.", VUI_COL_DIM);
    } else {
        vui_text(win, cx + 20, y, "Click a mode to apply it instantly.", VUI_COL_DIM);
    }
}

static void render_appearance(int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    vui_text(win, cx + 20, cy + 20, "Appearance", VUI_COL_FG);

    /* Theme */
    vui_text(win, cx + 20, cy + 60, "Theme", VUI_COL_DIM);
    int bw = 110, bh = 30;
    if (vui_button(win, cx + 20,  cy + 80, bw, bh, "Dark",  accent_colors[accent_idx], 0xFFFFFFFF)) {}
    if (vui_button(win, cx + 142, cy + 80, bw, bh, "Light", VUI_COL_BTN, VUI_COL_DIM)) {}

    /* Accent */
    vui_text(win, cx + 20, cy + 130, "Accent color", VUI_COL_DIM);
    int ax = cx + 30;
    int ay = cy + 162;
    int dot_r = 14;
    int gap = 44;
    for (int i = 0; i < NACCENT; i++) {
        if (color_dot(ax + i * gap, ay, dot_r, accent_colors[i], i == accent_idx))
            accent_idx = i;
    }

    /* Fonts */
    vui_text(win, cx + 20, cy + 220, "Fonts", VUI_COL_DIM);
    vui_text(win, cx + 30, cy + 246, "UI: Adwaita Sans",   VUI_COL_FG);
    vui_text(win, cx + 30, cy + 268, "Mono: Adwaita Mono", VUI_COL_FG);

    vui_text(win, cx + 20, cy + 312, "Saving these settings persistently — TODO.",
             VUI_COL_DIM);
}

static void render_sysinfo(int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    vui_text(win, cx + 20, cy + 20, "System Information", VUI_COL_FG);

    int y = cy + 60;
    vui_text(win, cx + 20, y, "Architecture", VUI_COL_DIM); y += 22;
    vui_text(win, cx + 30, y, "x86_64 (long mode, 4-level paging)", VUI_COL_FG); y += 22;
    vui_text(win, cx + 30, y, "NX bit enabled, demand paging for ELF", VUI_COL_FG); y += 30;

    vui_text(win, cx + 20, y, "Graphics", VUI_COL_DIM); y += 22;
    unsigned long long caps = vos_fb_caps();
    if (caps & 1)
        vui_text(win, cx + 30, y, "virtio-gpu (offscreen + present)", VUI_COL_FG);
    else
        vui_text(win, cx + 30, y, "Limine framebuffer (linear)", VUI_COL_FG);
    y += 30;

    int dw, dh; cur_mode(&dw, &dh);
    char buf[64];
    snprintf(buf, sizeof(buf), "Framebuffer: %d x %d (32 bpp BGRA)", dw, dh);
    vui_text(win, cx + 30, y, buf, VUI_COL_FG); y += 30;

    vui_text(win, cx + 20, y, "Filesystems", VUI_COL_DIM); y += 22;
    vui_text(win, cx + 30, y, "VortexFS v3 (root)", VUI_COL_FG); y += 18;
    vui_text(win, cx + 30, y, "FAT32 (legacy disk.img)", VUI_COL_FG);
}

static void render_login(int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    vui_text(win, cx + 20, cy + 20, "Login", VUI_COL_FG);

    vui_text(win, cx + 20, cy + 60, "User", VUI_COL_DIM);
    vui_text(win, cx + 30, cy + 84, "Default user (uid 0, root)", VUI_COL_FG);

    vui_text(win, cx + 20, cy + 130, "Password", VUI_COL_DIM);
    if (vui_checkbox(win, cx + 30, cy + 156, "Require password on startup",
                     login_password_enabled)) {
        login_password_enabled = !login_password_enabled;
    }

    if (login_password_enabled) {
        vui_text(win, cx + 30, cy + 192, "New password:", VUI_COL_DIM);
        vui_rect(win, cx + 30, cy + 214, 260, 28, VUI_COL_PANEL);
        vui_frame(win, cx + 30, cy + 214, 260, 28, VUI_COL_DIM);
        vui_text(win, cx + 38, cy + 220, "(input field — TODO)", VUI_COL_DIM);

        if (vui_button(win, cx + 30, cy + 256, 110, 30, "Save",
                       accent_colors[accent_idx], 0xFFFFFFFF)) {
            /* TODO: записать sha256(password) в /etc/shadow */
        }
    } else {
        vui_text(win, cx + 30, cy + 196, "Login screen will show without password prompt.",
                 VUI_COL_DIM);
    }
}

/* ===========================================================================
 * Главный рендер
 * ========================================================================= */
static void render(void) {
    vui_clear(win, VUI_COL_BG);

    /* Sidebar */
    vui_rect(win, 0, 0, SIDEBAR_W, win->h, VUI_COL_PANEL);
    vui_rect(win, SIDEBAR_W - 1, 0, 1, win->h, 0xFF1A1A24);

    vui_text(win, 20, 20, "Settings", VUI_COL_FG);

    /* Сначала прогоняем клики, чтобы current_section обновился ДО отрисовки —
     * иначе в одном render-проходе мы успеваем нарисовать старую активную
     * вкладку синей, а новую (только что кликнутую) серой. */
    for (int i = 0; i < NSEC; i++) {
        int y = 60 + i * 44;
        if (vui_click_in(win, 0, y, SIDEBAR_W, 44)) {
            win->click_pending = 0;
            current_section = i;
            break;
        }
    }
    for (int i = 0; i < NSEC; i++) {
        int y = 60 + i * 44;
        sidebar_item(0, y, SIDEBAR_W, 44, section_names[i],
                     current_section == i);
    }

    /* Content */
    int cx = SIDEBAR_W;
    int cy = 0;
    int cw = win->w - SIDEBAR_W;
    int ch = win->h;

    switch (current_section) {
    case SEC_ABOUT:      render_about(cx, cy, cw, ch);      break;
    case SEC_DISPLAY:    render_display(cx, cy, cw, ch);    break;
    case SEC_APPEARANCE: render_appearance(cx, cy, cw, ch); break;
    case SEC_SYSINFO:    render_sysinfo(cx, cy, cw, ch);    break;
    case SEC_LOGIN:      render_login(cx, cy, cw, ch);      break;
    }

    vui_flush(win);
}

int main(void) {
    win = vui_open("Settings", START_W, START_H);
    if (!win) {
        puts("vsettings: failed to create window");
        return 1;
    }
    render();

    vui_event_t ev;
    while (vui_wait_event(win, &ev)) {
        switch (ev.type) {
        case VUI_EV_MOUSE:
            if (ev.buttons & 1) {
                render();
                if (display_pending >= 0) {
                    int i = display_pending; display_pending = -1;
                    long long r = vos_display_set_mode(
                        (unsigned long long)modes[i].w,
                        (unsigned long long)modes[i].h);
                    display_err = (r != 0);
                    render();
                }
            }
            break;
        case VUI_EV_KEY:
        case VUI_EV_RESIZE:
            render();
            break;
        default:
            break;
        }
    }
    vui_close(win);
    return 0;
}
