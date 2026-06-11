/* =============================================================================
 * VortexOS — userspace/bin/vutil.h
 * Общие хелперы утилит /bin (ls, cat, rm, find, ...): cwd-кэш, разворачивание
 * относительных путей (vos_abspath из vos_abi.h), basename, склейка путей,
 * '*'-маски для find. Все утилиты — libc-приложения: int main(argc, argv),
 * argv разбирает crt0 из SYS_GETARGS.
 * ============================================================================= */
#ifndef VUTIL_H
#define VUTIL_H

#include <vos.h>
#include <stdio.h>
#include <string.h>

#define VU_PATH_MAX 256

static char vu_cwd_buf[64];

static inline const char *vu_cwd(void) {
    if (!vu_cwd_buf[0]) {
        if (vos_getcwd(vu_cwd_buf, sizeof(vu_cwd_buf)) <= 0)
            strcpy(vu_cwd_buf, "/");
    }
    return vu_cwd_buf;
}

/* Любой аргумент-путь -> абсолютный (cwd наследуется от шелла при spawn). */
static inline void vu_resolve(const char *path, char *out, int max) {
    vos_abspath(vu_cwd(), path, out, max);
}

static inline const char *vu_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : (s ? s : p);
}

/* dir + "/" + name (dir уже абсолютный) */
static inline void vu_join(const char *dir, const char *name,
                           char *out, int max) {
    int o = 0;
    while (dir[o] && o < max - 1) { out[o] = dir[o]; o++; }
    if (o > 0 && out[o - 1] != '/' && o < max - 1) out[o++] = '/';
    int i = 0;
    while (name[i] && o < max - 1) out[o++] = name[i++];
    out[o] = 0;
}

/* Маска с '*' (любая подстрока) и '?' (один символ) — для find -name. */
static inline int vu_match(const char *pat, const char *s) {
    if (*pat == 0) return *s == 0;
    if (*pat == '*') {
        for (;;) {
            if (vu_match(pat + 1, s)) return 1;
            if (!*s) return 0;
            s++;
        }
    }
    if (!*s) return 0;
    if (*pat == '?' || *pat == *s) return vu_match(pat + 1, s + 1);
    return 0;
}

static inline int vu_is_dir(const char *abs) {
    vos_stat_t st;
    return vos_fs_stat(abs, &st) == 0 && st.type == VOS_DT_DIR;
}

#endif /* VUTIL_H */
