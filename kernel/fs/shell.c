/* =============================================================================
 * VortexOS — kernel/fs/shell.c
 * Минимальный встроенный shell (vos-sh).
 * Команды: ls, cat, mkdir, touch, rm, echo, clear, help, uname
 * Позже будет заменён ELF-бинарником из /bin/vos-sh
 * ============================================================================= */

#include "shell.h"
#include "vfs.h"
#include "elf.h"
#include "vmm.h"
#include "fb.h"
#include "keyboard.h"

/* -------------------------------------------------------------------------
 * Вспомогательные строковые функции (нет libc в ядре)
 * ------------------------------------------------------------------------- */

static int sh_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static int sh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int sh_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (!a[i] && !b[i]) return 0;
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static void sh_strcpy(char *d, const char *s) {
    while ((*d++ = *s++));
}

/* Разбиваем строку на токены по пробелам */
#define MAX_ARGS 8
#define MAX_LINE 256

static int sh_split(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
        if (argc >= max) break;
    }
    return argc;
}

/* Текущая рабочая директория */
static char cwd[512] = "/";

/* Строит полный путь: cwd + "/" + name */
static void sh_fullpath(char *out, const char *name) {
    if (name[0] == '/') {
        sh_strcpy(out, name);
        return;
    }
    sh_strcpy(out, cwd);
    int len = sh_strlen(out);
    if (out[len-1] != '/') { out[len] = '/'; out[len+1] = 0; len++; }
    sh_strcpy(out + len, name);
}

/* -------------------------------------------------------------------------
 * Команды
 * ------------------------------------------------------------------------- */

static void cmd_help(void) {
    fb_puts("VortexOS Shell v0.1\n");
    fb_puts("Commands:\n");
    fb_puts("  ls [path]      - list directory\n");
    fb_puts("  cat <file>     - print file contents\n");
    fb_puts("  mkdir <dir>    - create directory\n");
    fb_puts("  touch <file>   - create empty file\n");
    fb_puts("  rm <file>      - remove file\n");
    fb_puts("  echo <text>    - print text\n");
    fb_puts("  write <f> <t>  - write text to file\n");
    fb_puts("  cd <path>      - change directory\n");
    fb_puts("  pwd            - print working directory\n");
    fb_puts("  clear          - clear screen\n");
    fb_puts("  uname          - system info\n");
    fb_puts("  exec <file>    - load and run ELF binary\n");
    fb_puts("  help           - this message\n");
}

static void cmd_ls(const char *path) {
    vfs_node_t *dir = vfs_open(path, 0);
    if (!dir) {
        fb_puts("ls: no such directory: "); fb_puts(path); fb_putchar('\n');
        return;
    }
    if (dir->type != VFS_DIR) {
        fb_puts("ls: not a directory: "); fb_puts(path); fb_putchar('\n');
        return;
    }
    fb_puts(path); fb_puts(":\n");
    uint32_t i = 0;
    for (;;) {
        const char *name = vfs_readdir(dir, i++);
        if (!name) break;

        /* Определяем тип — ищем дочернюю ноду */
        char full[512];
        sh_fullpath(full, name);
        /* Строим путь относительно path */
        char childpath[512];
        sh_strcpy(childpath, path);
        int plen = sh_strlen(childpath);
        if (childpath[plen-1] != '/') { childpath[plen]='/'; childpath[plen+1]=0; plen++; }
        sh_strcpy(childpath+plen, name);

        vfs_node_t *child = vfs_open(childpath, 0);
        if (child && child->type == VFS_DIR) {
            fb_puts("  [DIR]  "); fb_puts(name);
        } else {
            fb_puts("  [FILE] "); fb_puts(name);
            if (child) {
                /* печатаем размер */
                fb_puts(" (");
                uint32_t sz = child->size;
                char nbuf[12]; int ni = 10; nbuf[11]=0;
                if (!sz) nbuf[ni--]='0';
                else while(sz){nbuf[ni--]='0'+sz%10;sz/=10;}
                fb_puts(&nbuf[ni+1]);
                fb_puts(" B)");
            }
        }
        fb_putchar('\n');
    }
}

static void cmd_cat(const char *path) {
    vfs_node_t *f = vfs_open(path, 0);
    if (!f || f->type != VFS_FILE) {
        fb_puts("cat: cannot open: "); fb_puts(path); fb_putchar('\n');
        return;
    }
    uint8_t chunk[128];
    uint32_t off = 0;
    for (;;) {
        int32_t n = vfs_read(f, off, sizeof(chunk), chunk);
        if (n <= 0) break;
        for (int32_t i = 0; i < n; i++) fb_putchar((char)chunk[i]);
        off += (uint32_t)n;
    }
}

static void cmd_write(const char *path, const char *text) {
    vfs_node_t *f = vfs_open(path, 0);
    if (!f) {
        /* файл не существует — создаём */
        char parent[512]; sh_strcpy(parent, path);
        vfs_create(path);
        f = vfs_open(path, 0);
    }
    if (!f) { fb_puts("write: cannot create file\n"); return; }
    uint32_t len = (uint32_t)sh_strlen(text);
    vfs_write(f, 0, len, (const uint8_t *)text);
    fb_puts("write: OK\n");
}

static void cmd_clear(void) {
    /* ANSI clear — просто много новых строк */
    for (int i = 0; i < 50; i++) fb_putchar('\n');
}

static void cmd_cd(const char *path) {
    char full[512];
    if (path[0] == '/') sh_strcpy(full, path);
    else sh_fullpath(full, path);

    vfs_node_t *d = vfs_open(full, 0);
    if (!d || d->type != VFS_DIR) {
        fb_puts("cd: no such directory: "); fb_puts(full); fb_putchar('\n');
        return;
    }
    sh_strcpy(cwd, full);
    /* Убираем trailing slash кроме корня */
    int len = sh_strlen(cwd);
    if (len > 1 && cwd[len-1] == '/') cwd[len-1] = 0;
}

/* -------------------------------------------------------------------------
 * Главный цикл shell
 * ------------------------------------------------------------------------- */

static void sh_prompt(void) {
    fb_puts("\nvos-sh:");
    fb_puts(cwd);
    fb_puts("$ ");
}

void shell_run(void) {
    fb_puts("\n=============================\n");
    fb_puts("  VortexOS Shell (vos-sh)\n");
    fb_puts("  Type 'help' for commands\n");
    fb_puts("=============================\n");

    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    for (;;) {
        sh_prompt();

        /* Читаем строку с клавиатуры */
        int pos = 0;
        for (;;) {
            char c = keyboard_getchar();
            if (c == '\n' || c == '\r') {
                fb_putchar('\n');
                break;
            }
            if ((c == '\b' || c == 127) && pos > 0) {
                pos--;
                /* backspace — затираем символ */
                fb_putchar('\b'); fb_putchar(' '); fb_putchar('\b');
                continue;
            }
            if (c >= 32 && pos < MAX_LINE - 1) {
                line[pos++] = c;
                fb_putchar(c);
            }
        }
        line[pos] = 0;
        if (pos == 0) continue;

        int argc = sh_split(line, argv, MAX_ARGS);
        if (argc == 0) continue;

        /* Диспетчер команд */
        if (!sh_strcmp(argv[0], "help")) {
            cmd_help();

        } else if (!sh_strcmp(argv[0], "ls")) {
            char path[512];
            if (argc > 1) sh_fullpath(path, argv[1]);
            else sh_strcpy(path, cwd);
            cmd_ls(path);

        } else if (!sh_strcmp(argv[0], "cat")) {
            if (argc < 2) { fb_puts("Usage: cat <file>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            cmd_cat(path);

        } else if (!sh_strcmp(argv[0], "mkdir")) {
            if (argc < 2) { fb_puts("Usage: mkdir <dir>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            if (vfs_mkdir(path) == 0) fb_puts("mkdir: OK\n");
            else fb_puts("mkdir: failed\n");

        } else if (!sh_strcmp(argv[0], "touch")) {
            if (argc < 2) { fb_puts("Usage: touch <file>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            if (vfs_create(path) == 0) fb_puts("touch: OK\n");
            else fb_puts("touch: failed\n");

        } else if (!sh_strcmp(argv[0], "rm")) {
            if (argc < 2) { fb_puts("Usage: rm <file>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            if (vfs_unlink(path) == 0) fb_puts("rm: OK\n");
            else fb_puts("rm: failed\n");

        } else if (!sh_strcmp(argv[0], "echo")) {
            for (int i = 1; i < argc; i++) {
                if (i > 1) fb_putchar(' ');
                fb_puts(argv[i]);
            }
            fb_putchar('\n');

        } else if (!sh_strcmp(argv[0], "write")) {
            if (argc < 3) { fb_puts("Usage: write <file> <text>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            cmd_write(path, argv[2]);

        } else if (!sh_strcmp(argv[0], "cd")) {
            if (argc < 2) { fb_puts("Usage: cd <path>\n"); continue; }
            cmd_cd(argv[1]);

        } else if (!sh_strcmp(argv[0], "pwd")) {
            fb_puts(cwd); fb_putchar('\n');

        } else if (!sh_strcmp(argv[0], "clear")) {
            cmd_clear();

        } else if (!sh_strcmp(argv[0], "uname")) {
            fb_puts("VortexOS v0.1 x86_64\n");

        } else if (!sh_strcmp(argv[0], "exec")) {
            if (argc < 2) { fb_puts("Usage: exec <file>\n"); continue; }
            char path[512]; sh_fullpath(path, argv[1]);
            elf_load_result_t result = elf_load(path);
            if (result.entry_point) {
                fb_puts("exec: userspace execution not yet supported from shell\n");
                // TODO: переключиться в usermode и выполнить
            }
            /* exec пока не поддержан — не оставляем за собой page table */
            if (result.user_pml4)
                vmm_destroy_user_pml4((pte_t *)result.user_pml4);

        } else {
            fb_puts(argv[0]); fb_puts(": command not found\n");
        }
    }
}
