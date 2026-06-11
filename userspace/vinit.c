/* =============================================================================
 * VortexOS — /bin/vinit (этап 4 роадмапа: init система)
 *
 * Первый userspace-процесс: ядро запускает ТОЛЬКО его (аналог PID 1),
 * дальше всё пользовательское пространство поднимает vinit:
 *
 *   1. Читает описания сервисов из .svc-файлов в /etc/vinit (key=value):
 *          # комментарий
 *          name=vwm
 *          exec=/bin/vwm
 *          restart=yes
 *      Нет каталога/файлов — встроенный fallback (vwm + vpanel), чтобы
 *      старые образы диска продолжали грузиться в GUI.
 *
 *   2. Параллельный запуск: все сервисы спавнятся подряд через SYS_SPAWN_EX
 *      с флагом pipe (bit0) — так vinit получает VOS_MSG_CHILD_EXIT, когда
 *      сервис умирает (stdout сервисов при этом глотается: после старта GUI
 *      писать в kernel-консоль поверх экрана нельзя).
 *
 *   3. Перезапуск упавших: restart=yes -> respawn. Защита от crash-loop:
 *      больше VINIT_MAX_CRASH падений за VINIT_CRASH_WINDOW тиков ->
 *      сервис помечается FAILED и больше не трогается (до `vctl start`).
 *
 *   4. Управление: регистрируется сервисом VOS_SVC_INIT и отвечает на
 *      команды /bin/vctl (start|stop|status) по IPC (см. vos_abi.h).
 *      stop = SYS_KILL + restart выключается; start включает обратно.
 * ============================================================================= */
#include <vos.h>
#include <stdio.h>
#include <string.h>

#define MAX_SVCS          16
#define EXEC_MAX          64
#define VINIT_MAX_CRASH    5     /* падений подряд...                  */
#define VINIT_CRASH_WINDOW 1000  /* ...за столько тиков (10 c) = FAILED */

typedef struct {
    char     name[VINIT_NAME_MAX];
    char     exec[EXEC_MAX];
    int      restart;       /* respawn при падении (restart=yes) */
    int      enabled;       /* vctl stop -> 0 (не перезапускать)  */
    int      state;         /* VINIT_ST_* */
    uint32_t pid;           /* 0 = не запущен */
    uint32_t restarts;      /* всего перезапусков (для status)    */
    uint64_t win_start;     /* начало окна подсчёта падений (тики) */
    uint32_t win_crashes;   /* падений в текущем окне             */
} svc_t;

static svc_t svcs[MAX_SVCS];
static int   nsvc;

/* --------------------------- запуск/останов --------------------------- */

static int svc_start(svc_t *s) {
    if (s->state == VINIT_ST_RUNNING) return -2;
    int64_t pid = vos_spawn_ex(s->exec, 1);   /* bit0: получать CHILD_EXIT */
    if (pid < 0) {
        printf("vinit: spawn failed: %s (%s)\n", s->name, s->exec);
        s->state = VINIT_ST_STOPPED;
        s->pid = 0;
        return -3;
    }
    s->pid = (uint32_t)pid;
    s->state = VINIT_ST_RUNNING;
    return 0;
}

static void svc_on_exit(uint32_t pid, uint64_t code) {
    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svcs[i];
        if (s->pid != pid) continue;
        s->pid = 0;
        s->state = VINIT_ST_STOPPED;
        if (!s->enabled || !s->restart) return;   /* остановлен vctl'ом */

        /* Защита от crash-loop: окно в VINIT_CRASH_WINDOW тиков. */
        uint64_t now = vos_uptime();
        if (!s->win_start || now - s->win_start > VINIT_CRASH_WINDOW) {
            s->win_start = now;
            s->win_crashes = 0;
        }
        if (++s->win_crashes > VINIT_MAX_CRASH) {
            printf("vinit: %s crashes too fast (exit %d), giving up\n",
                   s->name, (int)code);
            s->state = VINIT_ST_FAILED;
            return;
        }
        printf("vinit: %s exited (%d), restarting\n", s->name, (int)code);
        s->restarts++;
        svc_start(s);
        return;
    }
}

/* ------------------------- конфиги /etc/vinit ------------------------- */

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int parse_yes(const char *v) {
    return (v[0] == 'y' || v[0] == 'Y' || v[0] == '1' || v[0] == 't');
}

/* Один файл *.svc: строки key=value. Возврат: 1 = сервис добавлен. */
static int load_svc_file(const char *path, const char *fallback_name) {
    static char buf[512];
    int64_t n = vos_fs_read(path, 0, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;

    svc_t *s = &svcs[nsvc];
    memset(s, 0, sizeof(*s));
    s->restart = 1;
    s->enabled = 1;
    str_copy(s->name, fallback_name, VINIT_NAME_MAX);

    char *p = buf;
    while (*p) {
        char *line = p;                      /* выделяем строку */
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = 0;
        while (*line == ' ') line++;
        if (!*line || *line == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if      (!strcmp(key, "name"))    str_copy(s->name, val, VINIT_NAME_MAX);
        else if (!strcmp(key, "exec"))    str_copy(s->exec, val, EXEC_MAX);
        else if (!strcmp(key, "restart")) s->restart = parse_yes(val);
    }
    if (!s->exec[0]) return 0;               /* без exec сервис бессмыслен */
    nsvc++;
    return 1;
}

/* Имя файла -> имя сервиса по умолчанию: "10-VWM.SVC" -> "10-vwm". */
static void file_to_name(const char *fname, char *out) {
    int i = 0;
    while (fname[i] && fname[i] != '.' && i < VINIT_NAME_MAX - 1) {
        char c = fname[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        out[i++] = c;
    }
    out[i] = 0;
}

static int ends_with_svc(const char *fname) {
    int len = (int)strlen(fname);
    if (len < 4) return 0;
    const char *e = fname + len - 4;
    return (e[0] == '.' &&
            (e[1] == 's' || e[1] == 'S') &&
            (e[2] == 'v' || e[2] == 'V') &&
            (e[3] == 'c' || e[3] == 'C'));
}

static void load_config(void) {
    vos_dirent_t de;
    for (uint64_t idx = 0; nsvc < MAX_SVCS; idx++) {
        if (vos_fs_readdir("/etc/vinit", idx, &de) != 0) break;
        if (de.type != VOS_DT_FILE || !ends_with_svc(de.name)) continue;

        char path[64] = "/etc/vinit/";
        char name[VINIT_NAME_MAX];
        int o = (int)strlen(path), i = 0;
        while (de.name[i] && o < (int)sizeof(path) - 1) path[o++] = de.name[i++];
        path[o] = 0;
        file_to_name(de.name, name);
        load_svc_file(path, name);
    }

    if (nsvc) return;

    /* Fallback (нет /etc/vinit): встроенные сервисы — поведение как раньше. */
    printf("vinit: no /etc/vinit configs, using built-in defaults\n");
    static const char *defs[][2] = { { "vwm", "/bin/vwm" },
                                     { "vpanel", "/bin/vpanel" } };
    for (int i = 0; i < 2 && nsvc < MAX_SVCS; i++) {
        svc_t *s = &svcs[nsvc++];
        memset(s, 0, sizeof(*s));
        str_copy(s->name, defs[i][0], VINIT_NAME_MAX);
        str_copy(s->exec, defs[i][1], EXEC_MAX);
        s->restart = 1;
        s->enabled = 1;
    }
}

/* ---------------------------- vctl по IPC ----------------------------- */

static void reply_err(uint32_t dst, uint64_t code) {
    vos_msg_t r;
    memset(&r, 0, sizeof(r));
    r.w[0] = VINIT_R_ERR;
    r.w[1] = code;
    vos_ipc_send(dst, &r);
}

static void reply_info(uint32_t dst, const svc_t *s, uint32_t idx, uint32_t count) {
    vos_msg_t r;
    memset(&r, 0, sizeof(r));
    r.w[0] = VINIT_R_INFO;
    r.w[1] = (uint64_t)s->state;
    r.w[2] = ((uint64_t)idx << 32) | count;
    r.w[3] = ((uint64_t)s->pid << 32) | s->restarts;
    memcpy(&r.w[4], s->name, VINIT_NAME_MAX);
    ((char *)&r.w[4])[VINIT_NAME_MAX - 1] = 0;
    vos_ipc_send(dst, &r);
}

static svc_t *svc_find(const char *name) {
    for (int i = 0; i < nsvc; i++)
        if (!strcmp(svcs[i].name, name)) return &svcs[i];
    return 0;
}

static void handle_cmd(const vos_msg_t *m) {
    uint32_t from = (uint32_t)m->w[7];
    char name[VINIT_NAME_MAX];
    memcpy(name, &m->w[2], VINIT_NAME_MAX);
    name[VINIT_NAME_MAX - 1] = 0;

    switch (m->w[0]) {
    case VINIT_CMD_STATUS:
        if (name[0]) {
            svc_t *s = svc_find(name);
            if (!s) { reply_err(from, 1); return; }
            reply_info(from, s, 0, 1);
        } else {
            if (!nsvc) {
                vos_msg_t r;
                memset(&r, 0, sizeof(r));
                r.w[0] = VINIT_R_INFO;            /* count=0 — пусто */
                vos_ipc_send(from, &r);
                return;
            }
            for (int i = 0; i < nsvc; i++)
                reply_info(from, &svcs[i], (uint32_t)i, (uint32_t)nsvc);
        }
        return;

    case VINIT_CMD_START: {
        svc_t *s = svc_find(name);
        if (!s) { reply_err(from, 1); return; }
        if (s->state == VINIT_ST_RUNNING) { reply_err(from, 2); return; }
        s->enabled = 1;
        s->win_start = 0;                    /* свежий шанс после FAILED */
        s->win_crashes = 0;
        if (svc_start(s) != 0) { reply_err(from, 3); return; }
        reply_info(from, s, 0, 1);
        return;
    }

    case VINIT_CMD_STOP: {
        svc_t *s = svc_find(name);
        if (!s) { reply_err(from, 1); return; }
        s->enabled = 0;                      /* CHILD_EXIT не перезапустит */
        if (s->pid) vos_kill(s->pid);        /* умрёт при своём syscall'е */
        s->state = VINIT_ST_STOPPED;         /* оптимистично для status   */
        reply_info(from, s, 0, 1);
        return;
    }
    }
}

/* -------------------------------- main -------------------------------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("vinit: VortexOS init starting\n");

    vos_svc_register(VOS_SVC_INIT);
    load_config();

    /* Параллельный запуск: спавним все подряд, не дожидаясь друг друга.
     * Зависимые сервисы сами ждут нужное (vpanel ждёт WM-сервис). */
    for (int i = 0; i < nsvc; i++) {
        if (svc_start(&svcs[i]) == 0)
            printf("vinit: started %s (pid %d)\n",
                   svcs[i].name, (int)svcs[i].pid);
    }

    /* Главный цикл: смерти детей + команды vctl. Спим в recv — 0% CPU. */
    vos_msg_t m;
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        switch (m.w[0]) {
        case VOS_MSG_CHILD_EXIT:
            svc_on_exit((uint32_t)m.w[7], m.w[1]);
            break;
        case VOS_MSG_STDOUT:
            break;   /* stdout сервисов глотаем: GUI уже владеет экраном */
        case VINIT_CMD_STATUS:
        case VINIT_CMD_START:
        case VINIT_CMD_STOP:
            handle_cmd(&m);
            break;
        default:
            break;
        }
    }
    return 0;
}
