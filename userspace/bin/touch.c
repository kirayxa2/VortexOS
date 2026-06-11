/* /bin/touch file... — создать пустой файл, если его нет. */
#include "vutil.h"

int main(int argc, char **argv) {
    int rc = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        n++;
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        vos_stat_t st;
        if (vos_fs_stat(abs, &st) == 0) continue;   /* уже есть — ок */
        if (vos_fs_create(abs, 0) != 0) { printf("touch: %s: failed\n", argv[i]); rc = 1; }
    }
    if (!n) { printf("usage: touch file...\n"); return 1; }
    return rc;
}
