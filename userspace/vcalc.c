/* VortexOS — vcalc: простой калькулятор на libvui. */
#include <stdio.h>
#include <string.h>
#include <vui.h>

#define W 280
#define H 380

static vui_win_t *win;
static double  acc = 0.0;          /* накопитель */
static double  cur = 0.0;          /* текущий ввод */
static int     cur_started = 0;    /* был ли ввод цифры в cur */
static int     cur_frac = 0;       /* делитель дробной части (10, 100…) */
static char    pending = 0;        /* + - * /  или 0 */
static int     just_eq = 0;        /* после '=' любой ввод цифры обнуляет cur */

static void fmt(double v, char *out) {
    if (v < 0) { *out++ = '-'; v = -v; }
    /* Целая часть */
    long long iv = (long long)v;
    char tmp[24]; int n = 0;
    if (iv == 0) tmp[n++] = '0';
    while (iv > 0) { tmp[n++] = '0' + (int)(iv % 10); iv /= 10; }
    for (int i = n - 1; i >= 0; i--) *out++ = tmp[i];
    /* Дробная — до 4 знаков, обрезаем хвостовые нули */
    double frac = v - (long long)v;
    if (frac > 0.00001) {
        *out++ = '.';
        char fr[5]; int fn = 0;
        for (int i = 0; i < 4; i++) {
            frac *= 10;
            int d = (int)frac;
            fr[fn++] = '0' + d;
            frac -= d;
        }
        while (fn > 1 && fr[fn - 1] == '0') fn--;
        for (int i = 0; i < fn; i++) *out++ = fr[i];
    }
    *out = 0;
}

static double display_value(void) {
    return cur_started ? cur : acc;
}

static void apply_pending(void) {
    if (!pending) { acc = cur; return; }
    switch (pending) {
    case '+': acc = acc + cur; break;
    case '-': acc = acc - cur; break;
    case '*': acc = acc * cur; break;
    case '/': acc = (cur != 0.0) ? acc / cur : 0.0; break;
    }
}

static void on_digit(int d) {
    if (just_eq) { cur = 0; cur_started = 0; cur_frac = 0; just_eq = 0; }
    if (!cur_started) { cur = 0; cur_started = 1; }
    if (cur_frac) {
        cur = cur + (double)d / cur_frac;
        cur_frac *= 10;
    } else {
        cur = cur * 10 + d;
    }
}
static void on_dot(void) {
    if (just_eq) { cur = 0; cur_started = 0; just_eq = 0; }
    if (!cur_started) { cur = 0; cur_started = 1; }
    if (!cur_frac) cur_frac = 10;
}
static void on_op(char op) {
    if (cur_started || pending == 0) {
        if (pending) apply_pending();
        else         acc = cur;
    }
    pending = op;
    cur = 0; cur_started = 0; cur_frac = 0;
    just_eq = 0;
}
static void on_eq(void) {
    apply_pending();
    cur = acc; cur_started = 0; cur_frac = 0;
    pending = 0; just_eq = 1;
}
static void on_clear(void) {
    acc = 0; cur = 0; cur_started = 0; cur_frac = 0; pending = 0; just_eq = 0;
}
static void on_neg(void) {
    if (cur_started) cur = -cur;
    else             acc = -acc;
}

static int btn(int x, int y, int w, int h, const char *label, uint32_t bg) {
    return vui_button(win, x, y, w, h, label, bg, VUI_COL_FG);
}

static void render(void) {
    vui_clear(win, VUI_COL_BG);

    /* Дисплей */
    int dh = 70;
    vui_rect(win, 10, 10, W - 20, dh, 0xFF101218);
    vui_frame(win, 10, 10, W - 20, dh, VUI_COL_PANEL);
    char buf[32];
    fmt(display_value(), buf);
    int tw = vui_text_width(buf);
    vui_text(win, W - 20 - tw, 10 + (dh - VUI_TEXT_H) / 2, buf, VUI_COL_FG);

    /* Кнопки 4×5 */
    int bx0 = 10, by0 = 100;
    int bw = (W - 20 - 3 * 6) / 4;
    int bh = (H - by0 - 10 - 4 * 6) / 5;
    int gap = 6;

    /* Row 0: C ± / × */
    if (btn(bx0 + 0 * (bw + gap), by0,                 bw, bh, "C",  VUI_COL_WARN)) on_clear();
    if (btn(bx0 + 1 * (bw + gap), by0,                 bw, bh, "+/-",VUI_COL_BTN))  on_neg();
    if (btn(bx0 + 2 * (bw + gap), by0,                 bw, bh, "/",  VUI_COL_ACCENT))on_op('/');
    if (btn(bx0 + 3 * (bw + gap), by0,                 bw, bh, "*",  VUI_COL_ACCENT))on_op('*');
    /* Row 1: 7 8 9 - */
    if (btn(bx0 + 0 * (bw + gap), by0 + 1 * (bh + gap), bw, bh, "7", VUI_COL_BTN)) on_digit(7);
    if (btn(bx0 + 1 * (bw + gap), by0 + 1 * (bh + gap), bw, bh, "8", VUI_COL_BTN)) on_digit(8);
    if (btn(bx0 + 2 * (bw + gap), by0 + 1 * (bh + gap), bw, bh, "9", VUI_COL_BTN)) on_digit(9);
    if (btn(bx0 + 3 * (bw + gap), by0 + 1 * (bh + gap), bw, bh, "-", VUI_COL_ACCENT)) on_op('-');
    /* Row 2: 4 5 6 + */
    if (btn(bx0 + 0 * (bw + gap), by0 + 2 * (bh + gap), bw, bh, "4", VUI_COL_BTN)) on_digit(4);
    if (btn(bx0 + 1 * (bw + gap), by0 + 2 * (bh + gap), bw, bh, "5", VUI_COL_BTN)) on_digit(5);
    if (btn(bx0 + 2 * (bw + gap), by0 + 2 * (bh + gap), bw, bh, "6", VUI_COL_BTN)) on_digit(6);
    if (btn(bx0 + 3 * (bw + gap), by0 + 2 * (bh + gap), bw, bh, "+", VUI_COL_ACCENT)) on_op('+');
    /* Row 3: 1 2 3 = (растянуто на 2 строки) */
    if (btn(bx0 + 0 * (bw + gap), by0 + 3 * (bh + gap), bw, bh, "1", VUI_COL_BTN)) on_digit(1);
    if (btn(bx0 + 1 * (bw + gap), by0 + 3 * (bh + gap), bw, bh, "2", VUI_COL_BTN)) on_digit(2);
    if (btn(bx0 + 2 * (bw + gap), by0 + 3 * (bh + gap), bw, bh, "3", VUI_COL_BTN)) on_digit(3);
    if (btn(bx0 + 3 * (bw + gap), by0 + 3 * (bh + gap), bw,
            bh * 2 + gap, "=", VUI_COL_OK)) on_eq();
    /* Row 4: 0 (×2) . */
    if (btn(bx0 + 0 * (bw + gap), by0 + 4 * (bh + gap),
            bw * 2 + gap, bh, "0", VUI_COL_BTN)) on_digit(0);
    if (btn(bx0 + 2 * (bw + gap), by0 + 4 * (bh + gap), bw, bh, ".", VUI_COL_BTN)) on_dot();

    vui_flush(win);
}

static void on_key(char c) {
    if (c >= '0' && c <= '9') on_digit(c - '0');
    else if (c == '.') on_dot();
    else if (c == '+' || c == '-' || c == '*' || c == '/') on_op(c);
    else if (c == '\n' || c == '\r' || c == '=') on_eq();
    else if (c == 'c' || c == 'C' || c == 27) on_clear();
    else if (c == '\b' || c == 127) on_clear();
}

int main(void) {
    win = vui_open("Calculator", W, H);
    if (!win) return 1;
    render();
    vui_event_t ev;
    while (vui_wait_event(win, &ev)) {
        if (ev.type == VUI_EV_KEY && ev.pressed) on_key(ev.ch);
        render();
    }
    vui_close(win);
    return 0;
}
