/* /bin/rm [-r] path... — удаление файла/пустого каталога; -r рекурсивно. */
#include "vutil.h"

static int rm_recursive(const char *abs, int depth) {
    if (depth > 8) { printf("rm: %s: too deep\n", abs); return 1; }
    vos_stat_t st;
    if (vos_fs_stat(abs, &st) != 0) { printf("rm: %s: not found\n", abs); return 1; }
    if (st.type == VOS_DT_DIR) {
        /* выгребаем по одному с индекса 0: после unlink индексы съезжают */
        for (;;) {
            vos_dirent_t de;
            uint64_t idx = 0;
            int found = 0;
            while (vos_fs_readdir(abs, idx, &de) == 0) {
                if (strcmp(de.name, ".") && strcmp(de.name, "..")) { found = 1; break; }
                idx++;
            }
            if (!found) break;
            char child[VU_PATH_MAX];
            vu_join(abs, de.name, child, sizeof(child));
            if (rm_recursive(child, depth + 1)) return 1;
        }
    }
    if (vos_fs_unlink(abs) != 0) { printf("rm: %s: failed\n", abs); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int rflag = 0, rc = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) { rflag = 1; continue; }
        n++;
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        if (rflag) {
            rc |= rm_recursive(abs, 0);
        } else {
            vos_stat_t st;
            if (vos_fs_stat(abs, &st) != 0)       { printf("rm: %s: not found\n", argv[i]); rc = 1; }
            else if (st.type == VOS_DT_DIR && vos_fs_unlink(abs) != 0) {
                printf("rm: %s: directory not empty (use -r)\n", argv[i]); rc = 1;
            } else if (st.type != VOS_DT_DIR && vos_fs_unlink(abs) != 0) {
                printf("rm: %s: failed\n", argv[i]); rc = 1;
            }
        }
    }
    if (!n) { printf("usage: rm [-r] path...\n"); return 1; }
    return rc;
}
