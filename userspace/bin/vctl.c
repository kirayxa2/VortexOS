/* /bin/vctl — управление сервисами init-системы /bin/vinit.
 *
 *   vctl status          — таблица всех сервисов
 *   vctl status <name>   — один сервис
 *   vctl start  <name>   — запустить (и снова включить перезапуск)
 *   vctl stop   <name>   — остановить (SYS_KILL) и выключить перезапуск
 *   vctl list            — синоним status
 *
 * Общение по IPC (протокол VINIT_* в vos_abi.h): vinit найден через
 * SYS_SVC_LOOKUP(VOS_SVC_INIT), имя пакуется в w2..w4 (24 байта). */
#include "vutil.h"

static const char *state_str(uint64_t st) {
    switch (st) {
    case VINIT_ST_RUNNING: return "running";
    case VINIT_ST_FAILED:  return "failed";
    default:               return "stopped";
    }
}

static void print_info(const vos_msg_t *m, int header) {
    char name[VINIT_NAME_MAX];
    memcpy(name, &m->w[4], VINIT_NAME_MAX);
    name[VINIT_NAME_MAX - 1] = 0;
    uint32_t pid      = (uint32_t)(m->w[3] >> 32);
    uint32_t restarts = (uint32_t)(m->w[3] & 0xFFFFFFFFu);

    if (header)
        printf("%-16s %-8s %5s %9s\n", "SERVICE", "STATE", "PID", "RESTARTS");
    printf("%-16s %-8s %5d %9d\n", name, state_str(m->w[1]),
           (int)pid, (int)restarts);
}

static int print_err(uint64_t code, const char *name) {
    if      (code == 1) printf("vctl: no such service: %s\n", name);
    else if (code == 2) printf("vctl: %s is already running\n", name);
    else if (code == 3) printf("vctl: failed to start %s\n", name);
    else                printf("vctl: error %d\n", (int)code);
    return 1;
}

int main(int argc, char **argv) {
    const char *op   = (argc > 1) ? argv[1] : "status";
    const char *name = (argc > 2) ? argv[2] : "";

    uint64_t cmd;
    if      (!strcmp(op, "status") || !strcmp(op, "list")) cmd = VINIT_CMD_STATUS;
    else if (!strcmp(op, "start"))                         cmd = VINIT_CMD_START;
    else if (!strcmp(op, "stop"))                          cmd = VINIT_CMD_STOP;
    else {
        printf("usage: vctl status [name] | start <name> | stop <name>\n");
        return 1;
    }
    if (cmd != VINIT_CMD_STATUS && !name[0]) {
        printf("usage: vctl %s <name>\n", op);
        return 1;
    }
    if ((int)strlen(name) >= VINIT_NAME_MAX) {
        printf("vctl: service name too long\n");
        return 1;
    }

    uint64_t init_pid = vos_svc_lookup(VOS_SVC_INIT);
    if (!init_pid) {
        printf("vctl: vinit is not running\n");
        return 1;
    }

    vos_msg_t m;
    memset(&m, 0, sizeof(m));
    m.w[0] = cmd;
    memcpy(&m.w[2], name, strlen(name) + 1);
    if (vos_ipc_send(init_pid, &m) != 0) {
        printf("vctl: cannot reach vinit\n");
        return 1;
    }

    /* Ответ: VINIT_R_ERR или серия VINIT_R_INFO (w2 = (idx<<32)|count).
     * Чужие сообщения (stdout от vsh и т.п.) пропускаем. Дедлайн ~2 c. */
    uint64_t deadline = vos_uptime() + 200;
    uint32_t shown = 0, count = 1;
    int header = 1;
    while (shown < count) {
        if (vos_uptime() >= deadline) {
            printf("vctl: vinit did not reply\n");
            return 1;
        }
        if (!vos_ipc_recv(&m, 20)) continue;
        if ((uint64_t)m.w[7] != init_pid) continue;
        if (m.w[0] == VINIT_R_ERR) return print_err(m.w[1], name);
        if (m.w[0] != VINIT_R_INFO) continue;

        count = (uint32_t)(m.w[2] & 0xFFFFFFFFu);
        if (count == 0) { printf("vctl: no services\n"); return 0; }
        print_info(&m, header);
        header = 0;
        shown++;
    }
    return 0;
}
