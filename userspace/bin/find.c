/* /bin/find [path] [-name pattern] [-type f|d] — рекурсивный поиск.
 * pattern: '*' = любая подстрока, '?' = один символ. */
#include "vutil.h"

static const char *g_name = 0;
static int g_type = 0;   /* 0 = любой, 'f' / 'd' */
static int g_matches = 0;

static void walk(const char *abs, int depth) {
    vos_stat_t st;
    if (vos_fs_stat(abs, &st) != 0) return;
    int is_dir = (st.type == VOS_DT_DIR);
    const char *base = vu_basename(abs);

    int ok = 1;
    if (g_name && !vu_match(g_name, base)) ok = 0;
    if (g_type == 'f' && is_dir) ok = 0;
    if (g_type == 'd' && !is_dir) ok = 0;
    if (ok) { printf("%s\n", abs); g_matches++; }

    if (!is_dir || depth > 8) return;
    vos_dirent_t de;
    for (uint64_t i = 0; ; i++) {
        if (vos_fs_readdir(abs, i, &de) != 0) break;
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        char child[VU_PATH_MAX];
        vu_join(abs, de.name, child, sizeof(child));
        walk(child, depth + 1);
    }
}

int main(int argc, char **argv) {
    const char *start = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc)      g_name = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) g_type = argv[++i][0];
        else if (argv[i][0] != '-')                          start = argv[i];
        else { printf("usage: find [path] [-name pat] [-type f|d]\n"); return 1; }
    }
    char abs[VU_PATH_MAX];
    vu_resolve(start, abs, sizeof(abs));
    vos_stat_t st;
    if (vos_fs_stat(abs, &st) != 0) { printf("find: %s: not found\n", start); return 1; }
    walk(abs, 0);
    return g_matches ? 0 : 1;
}
