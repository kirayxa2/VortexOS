/* /bin/ls [-l] [-a] [path...] — листинг каталога (SYS_FS_READDIR). */
#include "vutil.h"

#define MAX_ENT 256

typedef struct { char name[32]; uint32_t type, size; } ent_t;
static ent_t ents[MAX_ENT];

static int list_dir(const char *abs, int lflag, int aflag) {
    int n = 0;
    vos_dirent_t de;
    for (uint64_t i = 0; n < MAX_ENT; i++) {
        if (vos_fs_readdir(abs, i, &de) != 0) break;
        if (!aflag && de.name[0] == '.') continue;
        strlcpy(ents[n].name, de.name, sizeof(ents[n].name));
        ents[n].type = de.type;
        ents[n].size = de.size;
        n++;
    }
    /* сортировка по имени (вставками — записей немного) */
    for (int i = 1; i < n; i++) {
        ent_t key = ents[i];
        int j = i - 1;
        while (j >= 0 && strcmp(ents[j].name, key.name) > 0) {
            ents[j + 1] = ents[j];
            j--;
        }
        ents[j + 1] = key;
    }
    if (lflag) {
        for (int i = 0; i < n; i++) {
            if (ents[i].type == VOS_DT_DIR)
                printf("d %10s  %s/\n", "-", ents[i].name);
            else
                printf("- %10u  %s\n", ents[i].size, ents[i].name);
        }
    } else {
        int col = 0;
        for (int i = 0; i < n; i++) {
            int w = (int)strlen(ents[i].name) + (ents[i].type == VOS_DT_DIR ? 1 : 0);
            if (col && col + w + 2 > 76) { putchar('\n'); col = 0; }
            if (col) { printf("  "); col += 2; }
            printf("%s%s", ents[i].name, ents[i].type == VOS_DT_DIR ? "/" : "");
            col += w;
        }
        if (col) putchar('\n');
    }
    return 0;
}

int main(int argc, char **argv) {
    int lflag = 0, aflag = 0, npaths = 0;
    const char *paths[16];
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int k = 1; argv[i][k]; k++) {
                if (argv[i][k] == 'l') lflag = 1;
                else if (argv[i][k] == 'a') aflag = 1;
                else { printf("ls: unknown option -%c\n", argv[i][k]); return 1; }
            }
        } else if (npaths < 16) paths[npaths++] = argv[i];
    }
    if (npaths == 0) paths[npaths++] = ".";

    int rc = 0;
    for (int i = 0; i < npaths; i++) {
        char abs[VU_PATH_MAX];
        vu_resolve(paths[i], abs, sizeof(abs));
        vos_stat_t st;
        if (vos_fs_stat(abs, &st) != 0) {
            printf("ls: %s: not found\n", paths[i]);
            rc = 1;
            continue;
        }
        if (st.type != VOS_DT_DIR) {
            printf("%s\n", paths[i]);
            continue;
        }
        if (npaths > 1) printf("%s:\n", paths[i]);
        list_dir(abs, lflag, aflag);
        if (npaths > 1 && i != npaths - 1) putchar('\n');
    }
    return rc;
}
