/* /bin/mkdir [-p] dir... — создать каталог; -p со всеми родителями. */
#include "vutil.h"

static int mkdir_p(const char *abs) {
    char part[VU_PATH_MAX];
    int i = 1;
    part[0] = '/';
    while (abs[i]) {
        int o = i;
        while (abs[o] && abs[o] != '/') o++;
        for (int k = 0; k < o; k++) part[k] = abs[k];
        part[o] = 0;
        vos_stat_t st;
        if (vos_fs_stat(part, &st) != 0) {
            if (vos_fs_create(part, 1) != 0) return -1;
        } else if (st.type != VOS_DT_DIR) return -1;
        i = abs[o] ? o + 1 : o;
    }
    return 0;
}

int main(int argc, char **argv) {
    int pflag = 0, rc = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { pflag = 1; continue; }
        n++;
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        int err = pflag ? mkdir_p(abs) : (int)vos_fs_create(abs, 1);
        if (err != 0) { printf("mkdir: %s: failed\n", argv[i]); rc = 1; }
    }
    if (!n) { printf("usage: mkdir [-p] dir...\n"); return 1; }
    return rc;
}
