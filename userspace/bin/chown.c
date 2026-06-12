/* /bin/chown <uid[:gid]> path... — владелец файла (SYS_FS_CHOWN, root only). */
#include "vutil.h"

static int parse_u32(const char *s, int len, uint32_t *out) {
    if (len <= 0) return -1;
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: chown <uid[:gid]> path...   (chown 1000:1000 /home/f)\n");
        return 1;
    }
    const char *spec = argv[1];
    int colon = -1;
    for (int i = 0; spec[i]; i++)
        if (spec[i] == ':') { colon = i; break; }

    uint32_t uid, gid;
    int speclen = (int)strlen(spec);
    if (colon < 0) {
        if (parse_u32(spec, speclen, &uid) != 0) {
            printf("chown: bad uid '%s'\n", spec);
            return 1;
        }
        gid = uid;
    } else if (parse_u32(spec, colon, &uid) != 0 ||
               parse_u32(spec + colon + 1, speclen - colon - 1, &gid) != 0) {
        printf("chown: bad uid:gid '%s'\n", spec);
        return 1;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        char abs[VU_PATH_MAX];
        vu_resolve(argv[i], abs, sizeof(abs));
        if (vos_fs_chown(abs, uid, gid) != 0) {
            printf("chown: %s: failed (not found / not root / uid>255)\n",
                   argv[i]);
            rc = 1;
        }
    }
    return rc;
}
