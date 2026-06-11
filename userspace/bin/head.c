/* /bin/head [-n N] file... — первые N строк (по умолчанию 10). */
#include "vutil.h"
#include <stdlib.h>

static char buf[1024];

static int head_one(const char *arg, int limit) {
    char abs[VU_PATH_MAX];
    vu_resolve(arg, abs, sizeof(abs));
    vos_stat_t st;
    if (vos_fs_stat(abs, &st) != 0) { printf("head: %s: not found\n", arg); return 1; }
    if (st.type == VOS_DT_DIR)      { printf("head: %s: is a directory\n", arg); return 1; }

    uint64_t off = 0;
    int lines = 0;
    while (lines < limit) {
        int64_t n = vos_fs_read(abs, off, buf, sizeof(buf));
        if (n <= 0) break;
        int64_t upto = n;
        for (int64_t i = 0; i < n; i++)
            if (buf[i] == '\n' && ++lines >= limit) { upto = i + 1; break; }
        vos_write(1, buf, (uint64_t)upto);
        off += (uint64_t)n;
        if (n < (int64_t)sizeof(buf)) break;
    }
    return 0;
}

int main(int argc, char **argv) {
    int limit = 10, rc = 0, files = 0;
    const char *names[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (argv[i][0] != '-' && files < 16)   names[files++] = argv[i];
    }
    if (!files || limit < 0) { printf("usage: head [-n N] file...\n"); return 1; }
    for (int i = 0; i < files; i++) {
        if (files > 1) printf("==> %s <==\n", names[i]);
        rc |= head_one(names[i], limit);
    }
    return rc;
}
