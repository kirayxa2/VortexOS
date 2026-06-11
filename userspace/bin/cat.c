/* /bin/cat [-n] file... — вывод файлов (SYS_FS_READ чанками). */
#include "vutil.h"

static char buf[1024];
static int lineno = 1, at_bol = 1;

static int cat_one(const char *arg, int nflag) {
    char abs[VU_PATH_MAX];
    vu_resolve(arg, abs, sizeof(abs));
    vos_stat_t st;
    if (vos_fs_stat(abs, &st) != 0) { printf("cat: %s: not found\n", arg); return 1; }
    if (st.type == VOS_DT_DIR)      { printf("cat: %s: is a directory\n", arg); return 1; }

    uint64_t off = 0;
    for (;;) {
        int64_t n = vos_fs_read(abs, off, buf, sizeof(buf));
        if (n < 0) { printf("cat: %s: read error\n", arg); return 1; }
        if (n == 0) break;
        if (!nflag) {
            vos_write(1, buf, (uint64_t)n);
        } else {
            for (int64_t i = 0; i < n; i++) {
                if (at_bol) { printf("%6d  ", lineno++); at_bol = 0; }
                putchar(buf[i]);
                if (buf[i] == '\n') at_bol = 1;
            }
        }
        off += (uint64_t)n;
        if (n < (int64_t)sizeof(buf)) break;
    }
    return 0;
}

int main(int argc, char **argv) {
    int nflag = 0, rc = 0, files = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-n")) nflag = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) continue;
        files++;
        rc |= cat_one(argv[i], nflag);
    }
    if (!files) { printf("usage: cat [-n] file...\n"); return 1; }
    return rc;
}
