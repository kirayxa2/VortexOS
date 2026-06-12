/* /bin/stat path... — тип и размер (SYS_FS_STAT). */
#include "vutil.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: stat path...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        vos_stat_t st;
        if (vos_fs_stat(abs, &st) != 0) {
            printf("stat: %s: not found\n", argv[i]);
            rc = 1;
            continue;
        }
        char m[11];
        vu_modestr(st.type == VOS_DT_DIR, st.mode, m);
        if (st.type == VOS_DT_DIR)
            printf("%s: directory, %s uid=%u gid=%u\n", abs, m, st.uid, st.gid);
        else
            printf("%s: file, %u bytes, %s uid=%u gid=%u\n",
                   abs, st.size, m, st.uid, st.gid);
    }
    return rc;
}
