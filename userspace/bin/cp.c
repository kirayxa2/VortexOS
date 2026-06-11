/* /bin/cp src dst — копия файла. dst-каталог -> dst/basename(src).
 * Существующий dst-файл перезаписывается (unlink + create). */
#include "vutil.h"

static char buf[1024];

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: cp src dst\n"); return 1; }
    char src[VU_PATH_MAX], dst[VU_PATH_MAX];
    vu_resolve(argv[1], src, sizeof(src));
    vu_resolve(argv[2], dst, sizeof(dst));

    vos_stat_t st;
    if (vos_fs_stat(src, &st) != 0)  { printf("cp: %s: not found\n", argv[1]); return 1; }
    if (st.type == VOS_DT_DIR)       { printf("cp: %s: is a directory\n", argv[1]); return 1; }

    vos_stat_t dt;
    if (vos_fs_stat(dst, &dt) == 0) {
        if (dt.type == VOS_DT_DIR) {           /* cp f dir -> dir/f */
            char tmp[VU_PATH_MAX];
            vu_join(dst, vu_basename(src), tmp, sizeof(tmp));
            strlcpy(dst, tmp, sizeof(dst));
            if (vos_fs_stat(dst, &dt) == 0 && vos_fs_unlink(dst) != 0) {
                printf("cp: %s: cannot overwrite\n", dst); return 1;
            }
        } else if (vos_fs_unlink(dst) != 0) {  /* перезапись файла */
            printf("cp: %s: cannot overwrite\n", argv[2]); return 1;
        }
    }
    if (!strcmp(src, dst)) { printf("cp: %s and %s are the same\n", argv[1], argv[2]); return 1; }
    if (vos_fs_create(dst, 0) != 0) { printf("cp: %s: cannot create\n", dst); return 1; }

    uint64_t off = 0;
    for (;;) {
        int64_t n = vos_fs_read(src, off, buf, sizeof(buf));
        if (n < 0)  { printf("cp: %s: read error\n", argv[1]); return 1; }
        if (n == 0) break;
        if (vos_fs_write(dst, off, buf, (uint64_t)n) != n) {
            printf("cp: %s: write error\n", dst); return 1;
        }
        off += (uint64_t)n;
        if (n < (int64_t)sizeof(buf)) break;
    }
    return 0;
}
