/* /bin/mv src dst — переместить/переименовать файл (cp + unlink:
 * у FAT32-драйвера нет rename). Каталоги пока не переносит. */
#include "vutil.h"

static char buf[1024];

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: mv src dst\n"); return 1; }
    char src[VU_PATH_MAX], dst[VU_PATH_MAX];
    vu_resolve(argv[1], src, sizeof(src));
    vu_resolve(argv[2], dst, sizeof(dst));

    vos_stat_t st;
    if (vos_fs_stat(src, &st) != 0) { printf("mv: %s: not found\n", argv[1]); return 1; }
    if (st.type == VOS_DT_DIR)      { printf("mv: directories not supported yet\n"); return 1; }

    vos_stat_t dt;
    if (vos_fs_stat(dst, &dt) == 0 && dt.type == VOS_DT_DIR) {
        char tmp[VU_PATH_MAX];
        vu_join(dst, vu_basename(src), tmp, sizeof(tmp));
        strlcpy(dst, tmp, sizeof(dst));
    }
    if (!strcmp(src, dst)) return 0;
    if (vos_fs_stat(dst, &dt) == 0 && vos_fs_unlink(dst) != 0) {
        printf("mv: %s: cannot overwrite\n", dst); return 1;
    }
    if (vos_fs_create(dst, 0) != 0) { printf("mv: %s: cannot create\n", dst); return 1; }

    uint64_t off = 0;
    for (;;) {
        int64_t n = vos_fs_read(src, off, buf, sizeof(buf));
        if (n < 0)  { printf("mv: %s: read error\n", argv[1]); return 1; }
        if (n == 0) break;
        if (vos_fs_write(dst, off, buf, (uint64_t)n) != n) {
            printf("mv: %s: write error\n", dst); return 1;
        }
        off += (uint64_t)n;
        if (n < (int64_t)sizeof(buf)) break;
    }
    if (vos_fs_unlink(src) != 0) { printf("mv: %s: cannot remove source\n", argv[1]); return 1; }
    return 0;
}
