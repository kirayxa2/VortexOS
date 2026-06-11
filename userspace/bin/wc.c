/* /bin/wc [-l|-w|-c] file... — строки/слова/байты. */
#include "vutil.h"

static char buf[1024];

int main(int argc, char **argv) {
    int fl = 0, fw = 0, fc = 0, files = 0, rc = 0;
    const char *names[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) fl = 1;
        else if (!strcmp(argv[i], "-w")) fw = 1;
        else if (!strcmp(argv[i], "-c")) fc = 1;
        else if (argv[i][0] != '-' && files < 16) names[files++] = argv[i];
        else { printf("usage: wc [-l|-w|-c] file...\n"); return 1; }
    }
    if (!fl && !fw && !fc) fl = fw = fc = 1;
    if (!files) { printf("usage: wc [-l|-w|-c] file...\n"); return 1; }

    for (int i = 0; i < files; i++) {
        char abs[VU_PATH_MAX];
        vu_resolve(names[i], abs, sizeof(abs));
        vos_stat_t st;
        if (vos_fs_stat(abs, &st) != 0) { printf("wc: %s: not found\n", names[i]); rc = 1; continue; }
        if (st.type == VOS_DT_DIR)      { printf("wc: %s: is a directory\n", names[i]); rc = 1; continue; }

        uint32_t lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        uint64_t off = 0;
        for (;;) {
            int64_t n = vos_fs_read(abs, off, buf, sizeof(buf));
            if (n <= 0) break;
            for (int64_t k = 0; k < n; k++) {
                char c = buf[k];
                bytes++;
                if (c == '\n') lines++;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
            off += (uint64_t)n;
            if (n < (int64_t)sizeof(buf)) break;
        }
        if (fl) printf("%7u ", lines);
        if (fw) printf("%7u ", words);
        if (fc) printf("%7u ", bytes);
        printf("%s\n", names[i]);
    }
    return rc;
}
