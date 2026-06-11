#ifndef VOS_SCHED_H
#define VOS_SCHED_H

#include "types.h"

#define TASK_RUNNING  0
#define TASK_READY    1
#define TASK_BLOCKED  2
#define TASK_DEAD     3

#define MAX_TASKS       128
#define TASK_STACK_SIZE (32 * 1024)
#define TASK_MAX_ALLOCS 8   /* ELF-сегменты + user-стек + путь spawn'а */

#define TASK_CMDLINE_MAX 128  /* полная командная строка spawn_ex ("ls -l /bin") */
#define TASK_CWD_MAX     64   /* текущий каталог процесса (наследуется при spawn) */

typedef struct task {
    uint32_t        pid;
    uint8_t         state;
    uint8_t         priority;
    uint32_t        ticks;
    uint64_t        saved_rsp;
    uint8_t        *stack;
    char            name[32];
    struct task    *next;
    void           *userdata;  /* Произвольные данные (например, путь к ELF) */
    void           *pml4;      /* Адресное пространство задачи (pte_t*, vaddr).
                                * Планировщик грузит его в CR3 при свитче, чтобы
                                * у каждого usermode-процесса была своя память. */
    /* Сырые kmalloc-указатели, которые надо kfree при выходе задачи (ELF-
     * сегменты, user-стек, скопированный путь spawn'а). Без этого каждый
     * запуск приложения навсегда съедал сотни КБ kernel heap. */
    void           *allocs[TASK_MAX_ALLOCS];
    uint8_t         n_allocs;

    /* --- процессная обвязка для шелла (vsh: утилиты в /bin) --- */
    uint32_t        stdout_pid; /* куда зеркалить SYS_WRITE IPC-сообщениями
                                 * (VOS_MSG_STDOUT); 0 = только консоль ядра */
    uint64_t        exit_code;  /* код выхода (sys_exit) для VOS_MSG_CHILD_EXIT */
    uint8_t         pending_kill; /* SYS_KILL: задача завершится при следующем
                                   * syscall (диспетчер вызовет task_exit) */
    char            cmdline[TASK_CMDLINE_MAX]; /* argv процесса (SYS_GETARGS) */
    char            cwd[TASK_CWD_MAX];         /* текущий каталог (SYS_GETCWD) */
} task_t;

void    sched_init(void);
void    sched_irq_tick(void);
void    sched_yield(void);
void    sched_block_current(void);
void    sched_wake(task_t *t);
void    sched_clear_resched(void);
task_t *task_create(const char *name, void (*entry)(void), uint8_t priority);
void    task_exit(void);
/* Зарегистрировать kmalloc-указатель для kfree при выходе задачи.
 * t == 0 — текущая задача. Возврат: 0 = ок, -1 = список полон. */
int     task_track_alloc(task_t *t, void *raw);
task_t *sched_current(void);
int     sched_pid_alive(uint32_t pid);
task_t *sched_find_task(uint32_t pid);   /* живая задача по pid (или 0) */
uint64_t sched_pick(uint64_t frame_rsp);

extern int vos_need_resched;
extern void context_switch_to(uint64_t rsp);

#endif
