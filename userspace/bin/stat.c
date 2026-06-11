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
        if (st.type == VOS_DT_DIR)
            printf("%s: directory\n", abs);
        else
            printf("%s: file, %u bytes\n", abs, st.size);
    }
    return rc;
}
