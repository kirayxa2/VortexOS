/* =============================================================================
 * VortexOS — userspace/vsettings.c
 * vsettings — «Настройки»: пока одна страница, Display (смена разрешения).
 *
 * Как это работает:
 *   - текущий режим берём из SYS_FB_INFO;
 *   - клик по кнопке -> vos_display_set_mode(w, h) (SYS_DISPLAY_SET_MODE);
 *   - ядро меняет режим у virtio-gpu и шлёт vwm событие VIN_DISPLAY —
 *     рабочий стол (обои, панель, окна, включая НАШЕ) перестраивается сам,
 *     мы получим VUI_EV_RESIZE как обычное приложение.
 *
 * ВАЖНО: работает только под virtio-gpu (`make run-gpu`). На чистом
 * Limine-фреймбуфере режим прошивается загрузчиком при старте и на лету не
 * меняется — syscall вернёт -1, покажем подсказку.
 * ============================================================================= */
#include <stdio.h>
#include <vui.h>

#define START_W 420
#define START_H 360

typedef struct { int w, h; } mode_t_;
static const mode_t_ modes[] = {
    {  800,  600 },
    { 1024,  768 },
    { 1280,  720 },
    { 1280,  800 },
    { 1366,  768 },
    { 1440,  900 },
    { 1600,  900 },
    { 1920, 1080 },
};
#define NMODES ((int)(sizeof(modes) / sizeof(modes[0])))

static vui_win_t *win;
static int err = 0;            /* 1 = последняя смена не удалась */
static int pending = -1;       /* индекс режима, который попросили */

static void cur_mode(int *w, int *h) {
    struct { unsigned long long phys; unsigned int w, h, pitch, bpp; } info;
    info.w = info.h = 0;
    syscall1(SYS_FB_INFO, (unsigned long long)&info);
    *w = (int)info.w; *h = (int)info.h;
}

static void render(void) {
    int cw, ch;
    cur_mode(&cw, &ch);

    vui_clear(win, VUI_COL_BG);
    vui_rect(win, 0, 0, win->w, 4, VUI_COL_ACCENT);

    vui_text(win, 16, 18, "Settings", VUI_COL_FG);
    vui_text(win, 16, 40, "Display: screen resolution", VUI_COL_DIM);

    char buf[64];
    snprintf(buf, sizeof(buf), "Current mode: %d x %d", cw, ch);
    vui_text(win, 16, 66, buf, VUI_COL_OK);

    /* сетка кнопок 2 x 4 */
    int bw = (win->w - 16 * 2 - 12) / 2;
    int bh = 30;
    for (int i = 0; i < NMODES; i++) {
        int col = i % 2, row = i / 2;
        int x = 16 + col * (bw + 12);
        int y = 96 + row * (bh + 10);
        int active = (modes[i].w == cw && modes[i].h == ch);
        snprintf(buf, sizeof(buf), "%d x %d", modes[i].w, modes[i].h);
        if (vui_button(win, x, y, bw, bh, buf,
                       active ? VUI_COL_ACCENT : VUI_COL_BTN, VUI_COL_FG))
            pending = i;   /* применяем ПОСЛЕ flush — см. main loop */
    }

    int y = 96 + ((NMODES + 1) / 2) * (bh + 10) + 8;
    if (err) {
        vui_text(win, 16, y,      "Can't switch mode here :(", VUI_COL_ERR);
        vui_text(win, 16, y + 20, "Resolution switching needs virtio-gpu:", VUI_COL_DIM);
        vui_text(win, 16, y + 38, "run the OS with `make run-gpu`.", VUI_COL_DIM);
    } else {
        vui_text(win, 16, y, "Click a mode to apply it instantly.", VUI_COL_DIM);
        vui_text(win, 16, y + 20, "(needs `make run-gpu` / -vga virtio)", VUI_COL_DIM);
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
                render();                       /* кнопки отработают клик */
                if (pending >= 0) {
                    int i = pending; pending = -1;
                    long long r = vos_display_set_mode((unsigned long long)modes[i].w,
                                                       (unsigned long long)modes[i].h);
                    err = (r != 0);
                    /* при успехе vwm пришлёт RESIZE/перерисовку сам; при
                     * ошибке просто показываем подсказку */
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
