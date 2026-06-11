/* =============================================================================
 * VortexOS — userspace/vuidemo.c
 * vuidemo — витрина libc + libvui: то, как теперь выглядит «нормальное»
 * приложение под VortexOS.
 *
 * Сравни с vdemo.c: никакого ручного IPC, ручного draw_rect/draw_text и
 * самодельного _start. Обычный main(), printf, snprintf, malloc, кнопки.
 *
 * Что умеет:
 *   - счётчик с кнопками [-1] [+1] [RND] (rand из libc);
 *   - чекбокс, переключающий акцентную полосу;
 *   - прогрессбар, привязанный к счётчику;
 *   - эхо последней нажатой клавиши;
 *   - printf-лог в serial при кликах (смотри консоль QEMU).
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <vui.h>

#define START_W 420
#define START_H 320

static vui_win_t *win;
static int counter = 0;
static int stripe_on = 1;
static char last_key = 0;

static void render(void) {
    vui_clear(win, VUI_COL_BG);
    if (stripe_on)
        vui_rect(win, 0, 0, win->w, 4, VUI_COL_ACCENT);

    vui_text(win, 16, 18, "libc + libvui demo", VUI_COL_FG);
    vui_text(win, 16, 40, "Window, widgets, printf, malloc", VUI_COL_DIM);

    /* счётчик + кнопки */
    char buf[48];
    snprintf(buf, sizeof(buf), "counter = %d", counter);
    vui_text(win, 16, 78, buf, VUI_COL_FG);

    if (vui_button(win, 16, 104, 64, 30, "-1", VUI_COL_BTN, VUI_COL_FG)) {
        counter--;
        printf("vuidemo: counter -> %d\n", counter);
    }
    if (vui_button(win, 88, 104, 64, 30, "+1", VUI_COL_BTN, VUI_COL_FG)) {
        counter++;
        printf("vuidemo: counter -> %d\n", counter);
    }
    if (vui_button(win, 160, 104, 64, 30, "RND", VUI_COL_BTN, VUI_COL_FG)) {
        counter = rand() % 101;
        printf("vuidemo: counter -> %d (rand)\n", counter);
    }

    /* прогрессбар: счётчик как 0..100 */
    int pct = counter < 0 ? 0 : (counter > 100 ? 100 : counter);
    vui_progress(win, 16, 150, win->w - 32, 18, pct);

    /* чекбокс */
    if (vui_checkbox(win, 16, 186, "accent stripe", stripe_on))
        stripe_on = !stripe_on;

    /* эхо клавиатуры */
    if (last_key >= 0x20 && last_key < 0x7F) {
        snprintf(buf, sizeof(buf), "last key: '%c'", last_key);
        vui_text(win, 16, 216, buf, VUI_COL_OK);
    } else {
        vui_text(win, 16, 216, "type something...", VUI_COL_DIM);
    }

    vui_text(win, 16, win->h - 26, "Red button closes me.", VUI_COL_DIM);
    vui_flush(win);
}

int main(void) {
    win = vui_open("vuidemo - libc + libvui", START_W, START_H);
    if (!win) {
        puts("vuidemo: failed to create window");
        return 1;
    }
    printf("vuidemo: window %lu up, heap demo: %p\n",
           win->win_id, malloc(64));
    render();

    vui_event_t ev;
    while (vui_wait_event(win, &ev)) {
        switch (ev.type) {
        case VUI_EV_MOUSE:
            if (ev.buttons & 1) render();      /* клик — виджеты сами решат */
            break;
        case VUI_EV_KEY:
            if (ev.pressed) { last_key = ev.ch; render(); }
            break;
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
