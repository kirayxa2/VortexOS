#ifndef VOS_SCHED_H
#define VOS_SCHED_H

#include "types.h"

#define TASK_RUNNING  0
#define TASK_READY    1
#define TASK_BLOCKED  2
#define TASK_DEAD     3

#define MAX_TASKS       128
#define TASK_STACK_SIZE (32 * 1024)

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
} task_t;

void    sched_init(void);
void    sched_irq_tick(void);
void    sched_yield(void);
void    sched_block_current(void);
void    sched_wake(task_t *t);
void    sched_clear_resched(void);
task_t *task_create(const char *name, void (*entry)(void), uint8_t priority);
void    task_exit(void);
task_t *sched_current(void);
uint64_t sched_pick(uint64_t frame_rsp);

extern int vos_need_resched;
extern void context_switch_to(uint64_t rsp);

#endif
