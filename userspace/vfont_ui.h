/* =============================================================================
 * VortexOS — userspace/vfont_ui.h
 * Обёртка над vfont.h для UI-шрифта (AdwaitaSans). Подключается в vwm.c и
 * vpanel.c. Каждое из этих приложений живёт в своём адресном пространстве,
 * поэтому общий singleton vfont_instance из vfont.h не конфликтует.
 *
 * AdwaitaSans пропорциональный — ui_text_width() суммирует xadvance каждого
 * глифа, не len*ch_w, иначе центрирование чипов панели и заголовков поедет.
 * ============================================================================= */
#ifndef VFONT_UI_H
#define VFONT_UI_H

#include "vfont.h"

#ifndef VFONT_UI_PATH
#define VFONT_UI_PATH "/etc/fonts/AdwaitaSans-Regular.ttf"
#endif
#ifndef VFONT_UI_PX
#define VFONT_UI_PX   14.0f
#endif

static vfont_t *vfont_ui = 0;

static inline void vfont_ui_init(void) {
    vfont_ui = vfont_load(VFONT_UI_PATH, VFONT_UI_PX);
}

/* Ширина строки в пикселях: сумма xadvance каждого глифа.
 * Для пропорционального шрифта (AdwaitaSans) даёт точную метрику для
 * центрирования и расчёта ширины чипов панели/тултипов. Если шрифт не
 * загрузился — fallback на font8x16 (8 пикселей на символ). */
static inline int vfont_ui_text_width(const char *s) {
    if (!vfont_ui || !vfont_ui->ok) {
        int n = 0; while (s && s[n]) n++;
        return n * 8;
    }
    float w = 0.f;
    while (*s) {
        int cp = (unsigned char)*s++;
        if (cp < VF_FIRST || cp >= VF_FIRST + VF_COUNT)
            w += vfont_ui->ch_w;
        else
            w += vfont_ui->glyphs[cp - VF_FIRST].xadvance;
    }
    return (int)(w + 0.5f);
}

/* Высота одной строки текста; для центрирования в плашке высотой H:
 *   y_text = y + (H - vfont_ui_line_height()) / 2 */
static inline int vfont_ui_line_height(void) {
    if (!vfont_ui || !vfont_ui->ok) return 16;
    return vfont_ui->ch_h;
}

#endif /* VFONT_UI_H */
