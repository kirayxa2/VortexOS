/* =============================================================================
 * VortexOS — userspace/vfont.h
 * Растеризация TrueType шрифтов через stb_truetype, без libc.
 * Читает .ttf через vos_fs_read, все зависимости — inline внутри файла.
 *
 * Использование в vterm.c:
 *   vfont_t *font = vfont_load("/etc/fonts/JetBrainsMono-Regular.ttf", 15.0f);
 *   // font->ch_w, font->ch_h, font->ascent — метрики
 *   vfont_draw(surf, win_w, win_h, x, y, "hello", FG, BG_OR_0, font);
 * ============================================================================= */
#ifndef VFONT_H
#define VFONT_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * 1. Наши реализации всего что нужно stb_truetype (без libc)
 * -------------------------------------------------------------------------- */

typedef unsigned long size_t;
#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef UINT_MAX
#define UINT_MAX 0xFFFFFFFFu
#endif

static void *vf__memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char*)d;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}
static void *vf__memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = (unsigned char*)d;
    const unsigned char *q = (const unsigned char*)s;
    for (size_t i = 0; i < n; i++) p[i] = q[i];
    return d;
}
static unsigned int vf__strlen(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

/* --- математика ---------------------------------------------------------- */
static inline float vf__fabs(float x)  { return x < 0.f ? -x : x; }
static inline int   vf__ifloor(float x){ int i = (int)x; return (float)i > x ? i - 1 : i; }
static inline int   vf__iceil(float x) { int i = (int)x; return (float)i < x ? i + 1 : i; }

/* sqrt методом Ньютона — 8 итераций, точность ~1e-6 */
static inline float vf__sqrt(float x) {
    if (x <= 0.f) return 0.f;
    float r = x > 1.f ? x * 0.5f : 1.f;
    for (int i = 0; i < 8; i++) r = (r + x / r) * 0.5f;
    return r;
}
/* fmod — нужен только для stbtt_GetGlyphSDF (мы не используем) */
static inline float vf__fmod(float x, float y) {
    if (y == 0.f) return 0.f;
    int q = (int)(x / y);
    return x - (float)q * y;
}
/* pow, cos, acos — только для CFF/Type2 шрифтов, TTF не вызывает */
static inline float vf__pow(float x, float y)  { (void)x; (void)y; return 1.f; }
static inline float vf__cos(float x)            { (void)x; return 1.f; }
static inline float vf__acos(float x)           { (void)x; return 0.f; }

/* --- bump-аллокатор 512 KB ------------------------------------------------
 * stb_truetype аллоцирует временные буферы при растеризации (~30-60 KB).
 * Мы растеризуем один раз при старте — после этого аллокации не нужны.
 * free — нет-оп: стек не нужен, bump достаточен. */
#define VF_HEAP_SZ (512u * 1024u)
static unsigned char vf__heap[VF_HEAP_SZ] __attribute__((aligned(16)));
static unsigned int  vf__htop = 0;

/* malloc с заголовком (16 байт): хранит размер блока для free.
 * free — LIFO-reclaim: если освобождаемый блок последний в куче,
 * откатываем vf__htop назад. stb_truetype аллоцирует/освобождает
 * временные буферы строго парами при растеризации каждого глифа —
 * без этого куча росла монотонно и кончалась к ~79-му из 96 глифов,
 * из-за чего o..z (и дальше) не растеризовались (NULL из malloc). */
static void *vf__malloc(size_t sz, void *u) {
    (void)u;
    size_t total = sz + 16u;
    total = (total + 15u) & ~(size_t)15u;
    if (vf__htop + (unsigned int)total > VF_HEAP_SZ) return NULL;
    unsigned char *base = vf__heap + vf__htop;
    *(unsigned int*)base = (unsigned int)total;
    vf__htop += (unsigned int)total;
    return base + 16;
}
static void vf__free(void *p, void *u) {
    (void)u;
    if (!p) return;
    unsigned char *base = (unsigned char*)p - 16;
    unsigned int total = *(unsigned int*)base;
    if (base + total == vf__heap + vf__htop) {
        vf__htop -= total;
    }
}

/* --------------------------------------------------------------------------
 * 2. Конфигурируем stb_truetype — все внешние символы переопределены
 * -------------------------------------------------------------------------- */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_ifloor(x)      vf__ifloor(x)
#define STBTT_iceil(x)       vf__iceil(x)
#define STBTT_sqrt(x)        vf__sqrt(x)
#define STBTT_pow(x,y)       vf__pow(x,y)
#define STBTT_cos(x)         vf__cos(x)
#define STBTT_acos(x)        vf__acos(x)
#define STBTT_fabs(x)        vf__fabs(x)
#define STBTT_fmod(x,y)      vf__fmod(x,y)
#define STBTT_malloc(x,u)    vf__malloc((size_t)(x), u)
#define STBTT_free(x,u)      vf__free(x, u)
#define STBTT_assert(x)      /* no-op */
#define STBTT_strlen(s)      vf__strlen(s)
#define STBTT_memcpy(d,s,n)  vf__memcpy(d, s, (size_t)(n))
#define STBTT_memset(d,c,n)  vf__memset(d, c, (size_t)(n))
/* Блокируем системные заголовки которые stb пытается подключить */
#define __STDLIB_H
#define __STRING_H
#define __MATH_H

#include "stb_truetype.h"

/* --------------------------------------------------------------------------
 * 3. Атлас глифов
 * -------------------------------------------------------------------------- */
#define VF_ATLAS_W  512
#define VF_ATLAS_H  512
#define VF_FIRST    32    /* пробел */
#define VF_COUNT    96    /* ASCII 32..127 */

typedef struct {
    stbtt_fontinfo  info;
    float           scale;
    int             ch_w;      /* ширина символьной ячейки */
    int             ch_h;      /* высота строки */
    int             ascent;    /* базовая линия от верха ячейки */
    unsigned char   atlas[VF_ATLAS_W * VF_ATLAS_H];
    stbtt_bakedchar glyphs[VF_COUNT];
    int             ok;
} vfont_t;

static vfont_t vfont_instance __attribute__((aligned(16)));

/* --------------------------------------------------------------------------
 * 4. vfont_load: читает TTF с VortexFS, растеризует ASCII в атлас
 * -------------------------------------------------------------------------- */
#include "vos_abi.h"

/* TTF-буфер — статический, в BSS (не в стеке: JetBrains Mono ~270 KB) */
static unsigned char vf__ttf_buf[1536 * 1024] __attribute__((aligned(16)));

static vfont_t *vfont_load(const char *path, float size_px) {
    vfont_t *f = &vfont_instance;
    f->ok = 0;

    /* Читаем файл кусками по vos_fs_read */
    int64_t total = 0;
    int64_t chunk;
    do {
        chunk = vos_fs_read(path, (uint64_t)total,
                            vf__ttf_buf + total,
                            (uint64_t)(sizeof(vf__ttf_buf) - (unsigned int)total));
        if (chunk > 0) total += chunk;
    } while (chunk > 0 && total < (int64_t)sizeof(vf__ttf_buf));

    if (total <= 0) return NULL;

    /* Инициализируем stbtt */
    if (!stbtt_InitFont(&f->info, vf__ttf_buf,
                        stbtt_GetFontOffsetForIndex(vf__ttf_buf, 0)))
        return NULL;

    /* Растеризуем ASCII в атлас */
    vf__memset(f->atlas, 0, sizeof(f->atlas));

    int ret = stbtt_BakeFontBitmap(vf__ttf_buf, 0, size_px,
                                   f->atlas, VF_ATLAS_W, VF_ATLAS_H,
                                   VF_FIRST, VF_COUNT, f->glyphs);
    /* ret < 0: не все глифы влезли в атлас — уменьшаем px и пробуем ещё раз */
    if (ret < 0) {
        vf__htop = 0; /* сброс кучи */
        vf__memset(f->atlas, 0, sizeof(f->atlas));
        size_px *= 0.8f;
        ret = stbtt_BakeFontBitmap(vf__ttf_buf, 0, size_px,
                                   f->atlas, VF_ATLAS_W, VF_ATLAS_H,
                                   VF_FIRST, VF_COUNT, f->glyphs);
        if (ret < 0) return NULL;
        f->scale = stbtt_ScaleForPixelHeight(&f->info, size_px);
    }

    /* Метрики строки (scale уже посчитан выше если был fallback) */
    f->scale = stbtt_ScaleForPixelHeight(&f->info, size_px);
    int asc, desc, lgap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &lgap);
    f->ascent = (int)(asc * f->scale + 0.5f);
    int descent = (int)((-desc) * f->scale + 0.5f);
    f->ch_h = f->ascent + descent + (int)(lgap * f->scale + 0.5f);
    if (f->ch_h < (int)(size_px + 2.f))
        f->ch_h = (int)(size_px + 2.f);

    /* ch_w: xadvance уже в пикселях из BakeFontBitmap, все символы одинаковы для mono */
    f->ch_w = (int)(f->glyphs['A' - VF_FIRST].xadvance + 0.5f);
    if (f->ch_w < 4) f->ch_w = (int)(size_px * 0.6f + 0.5f);
    if (f->ch_w < 1) f->ch_w = 1;

    f->ok = 1;
    return f;
}

/* --------------------------------------------------------------------------
 * 5. vfont_draw: рендер строки в ARGB поверхность окна с альфа-блендингом
 *    bg = 0  → фон не заливать (прозрачный)
 * -------------------------------------------------------------------------- */
static void vfont_draw(uint32_t *surf, int sw, int sh,
                       int x, int y,
                       const char *s, uint32_t fg, uint32_t bg,
                       vfont_t *f) {
    if (!f || !f->ok) return;

    unsigned int fr = (fg >> 16) & 0xFF;
    unsigned int fgc = (fg >>  8) & 0xFF;
    unsigned int fb  =  fg        & 0xFF;

    /* Заливка фона всей строки если задан */
    if (bg) {
        const char *p = s;
        int len = 0;
        while (*p++) len++;
        for (int py = y; py < y + f->ch_h && py < sh; py++) {
            if (py < 0) continue;
            for (int px = x; px < x + len * f->ch_w && px < sw; px++) {
                if (px < 0) continue;
                surf[(unsigned int)py * sw + px] = bg;
            }
        }
    }

    float cur_x = (float)x;
    int baseline_y = y + f->ascent;

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

        /* Клипуем глиф к окну заранее: типичный случай — текст внутри
         * поверхности, и тогда горячий per-pixel цикл идёт без if'ов. */
        int r0 = 0, r1 = gh, c0 = 0, c1 = gw;
        if (sy     < 0)  r0 = -sy;
        if (sy+gh  > sh) r1 = sh - sy;
        if (sx     < 0)  c0 = -sx;
        if (sx+gw  > sw) c1 = sw - sx;
        if (r0 >= r1 || c0 >= c1) { cur_x += bc->xadvance; continue; }

        for (int row = r0; row < r1; row++) {
            int py = sy + row;
            int ay = (int)bc->y0 + row;
            const unsigned char *arow = f->atlas + (unsigned int)ay * VF_ATLAS_W;
            uint32_t *drow = &surf[(unsigned int)py * sw + sx];
            int ax_base = (int)bc->x0;

            for (int col = c0; col < c1; col++) {
                unsigned int a = arow[ax_base + col];
                if (a == 0) continue;
                uint32_t *dst = &drow[col];
                if (a == 255) {
                    *dst = fg;
                } else {
                    uint32_t d = *dst;
                    unsigned int dr  = (d >> 16) & 0xFF;
                    unsigned int dg  = (d >>  8) & 0xFF;
                    unsigned int dbl =  d        & 0xFF;
                    unsigned int ia = 255u - a;
                    unsigned int r = (fr  * a + dr  * ia) / 255u;
                    unsigned int g = (fgc * a + dg  * ia) / 255u;
                    unsigned int b = (fb  * a + dbl * ia) / 255u;
                    *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        cur_x += bc->xadvance;
    }
}

#endif /* VFONT_H */
