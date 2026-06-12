/* /bin/chmod <octal> path... — права файла (SYS_FS_CHMOD, владелец/root). */
#include "vutil.h"

static int parse_octal(const char *s, uint32_t *out) {
    if (!s[0]) return -1;
    uint32_t v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '7' || i >= 4) return -1;
        v = v * 8 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    uint32_t mode;
    if (argc < 3 || parse_octal(argv[1], &mode) != 0) {
        printf("usage: chmod <octal> path...   (chmod 755 /bin/tool)\n");
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        if (vos_fs_chmod(abs, mode) != 0) {
            printf("chmod: %s: failed (not found / not owner / no perms support)\n",
                   argv[i]);
            rc = 1;
        }
    }
    return rc;
}
