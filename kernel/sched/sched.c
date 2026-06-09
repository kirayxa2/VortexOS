/* =============================================================================
 * VOS — kernel/sched/sched.c
 * Фрейм задачи точно как VortexOS/src/sys/process.c (kernel thread)
 * ============================================================================= */

#include "sched.h"
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"

/* syscall-стек (точка входа SYSCALL) — переключаем его по-задачно тут же, где и
 * TSS.rsp0, иначе два usermode-процесса делят один kernel-стек на syscall. */
extern void syscall_set_kernel_stack(uint64_t rsp0);

static task_t   tasks[MAX_TASKS];
static uint32_t task_count = 0;
static task_t  *current    = 0;
static task_t  *run_queue  = 0;
static uint32_t next_pid   = 1;

int vos_need_resched = 0;

static void queue_push(task_t *t) {
    t->state = TASK_READY;
    if (!run_queue) { run_queue = t; t->next = t; return; }
    task_t *tail = run_queue;
    while (tail->next != run_queue) tail = tail->next;
    tail->next = t; t->next = run_queue;
}

static void queue_remove(task_t *t) {
    if (!run_queue) return;
    if (run_queue == t && t->next == t) { run_queue = 0; return; }
    task_t *prev = run_queue;
    while (prev->next != t && prev->next != run_queue) prev = prev->next;
    if (prev->next != t) return;
    prev->next = t->next;
    if (run_queue == t) run_queue = t->next;
    t->next = 0;
}

/* Задача рендера — регистрируется в kmain. Когда PIT просит кадр
 * (pit_render_pending), планировщик отдаёт процессор ИМЕННО ей в обход обычной
 * round-robin-очереди — иначе курсор замирал, пока крутится busy-loop чужой
 * задачи (vsh) на весь её квант. Рендер сам сдаёт процессор (sched_yield) сразу
 * после кадра, так что в промежутках (~20 мс) нормально работают vsh/dock/idle. */
static task_t *g_render_task = 0;
void sched_register_render(task_t *t) { g_render_task = t; }

static task_t *pick_next(void) {
    if (!run_queue) return 0;

    /* Приоритет задаче рендера, если есть невзятый запрос на кадр. */
    extern int pit_render_pending(void);
    if (g_render_task && g_render_task->state == TASK_READY && pit_render_pending())
        return g_render_task;

    task_t *start = current ? current->next : run_queue;
    task_t *t = start;
    do { if (t->state == TASK_READY) return t; t = t->next; } while (t != start);
    return 0;
}

void sched_init(void) {
    task_t *idle     = &tasks[task_count++];
    idle->pid        = next_pid++;
    idle->state      = TASK_RUNNING;
    idle->priority   = 1;
    idle->ticks      = 1;
    idle->stack      = 0;
    idle->saved_rsp  = 0;
    idle->name[0] = 'i'; idle->name[1] = 'd';
    idle->name[2] = 'l'; idle->name[3] = 'e'; idle->name[4] = 0;
    idle->pml4       = (void *)vmm_kernel_pml4;  /* idle живёт в kernel-пространстве */
    queue_push(idle);
    idle->state = TASK_RUNNING;
    current = idle;
}

task_t *task_create(const char *name, void (*entry)(void), uint8_t priority) {
    if (task_count >= MAX_TASKS) return 0;
    if (priority < 1)  priority = 1;
    if (priority > 10) priority = 10;

    task_t *t = &tasks[task_count++];
    t->pid      = next_pid++;
    t->state    = TASK_READY;
    t->priority = priority;
    t->ticks    = priority;
    t->next     = 0;
    /* По умолчанию задача живёт в kernel-пространстве. usermode-задача
     * перезапишет это своим user-pml4 (см. userspace_elf_loader_task). */
    t->pml4     = (void *)vmm_kernel_pml4;

    int i = 0;
    while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;

    /* Kernel stack — выровнен на 32768 как в VortexOS */
    t->stack = (uint8_t *)kmalloc_aligned(TASK_STACK_SIZE, TASK_STACK_SIZE);
    if (!t->stack) return 0;

    /*
     * Строим фрейм точно как VortexOS kernel thread в process_create():
     *
     * uint64_t* stack_ptr = top_of_stack;
     * *(--stack_ptr) = 0x10;           // SS
     * stack_ptr--;
     * *stack_ptr = (uint64_t)stack_ptr; // RSP — указывает сам на себя
     * *(--stack_ptr) = 0x202;           // RFLAGS
     * *(--stack_ptr) = 0x08;            // CS
     * *(--stack_ptr) = entry_point;     // RIP
     * *(--stack_ptr) = 0;               // int_no
     * *(--stack_ptr) = 0;               // err_code
     * for 15: *(--stack_ptr) = 0;       // GPR r15..rax
     * stack_ptr -= 512/8;               // fxsave (обнулить)
     */
    uint64_t *sp = (uint64_t *)(t->stack + TASK_STACK_SIZE);

    *(--sp) = 0x10;                  /* SS  */
    sp--;
    *sp = (uint64_t)sp;              /* RSP = указывает сам на себя */
    *(--sp) = 0x202;                 /* RFLAGS */
    *(--sp) = 0x08;                  /* CS */
    *(--sp) = (uint64_t)entry;       /* RIP */
    *(--sp) = 0;                     /* int_no */
    *(--sp) = 0;                     /* err_code */

    /* 15 GPR (r15..rax) */
    for (int k = 0; k < 15; k++) *(--sp) = 0;

    /* fxsave area — 512 байт */
    sp = (uint64_t *)((uint8_t *)sp - 512);
    __builtin_memset(sp, 0, 512);

    t->saved_rsp = (uint64_t)sp;

    queue_push(t);
    return t;
}

void task_exit(void) {
    __asm__ volatile("cli");
    current->state = TASK_DEAD;
    queue_remove(current);
    vos_need_resched = 1;
    for (;;) __asm__ volatile("hlt");
}

void sched_irq_tick(void) {
    if (!current) return;
    if (--current->ticks > 0) return;
    current->ticks = current->priority;
    vos_need_resched = 1;
}

void sched_yield(void) {
    vos_need_resched = 1;
}

task_t *sched_current(void) { return current; }
void    sched_clear_resched(void) { vos_need_resched = 0; }

uint64_t sched_pick(uint64_t frame_rsp) {
    current->saved_rsp = frame_rsp;
    current->state     = TASK_READY;
    vos_need_resched   = 0;

    task_t *next = pick_next();
    if (!next || next == current) {
        current->state = TASK_RUNNING;
        return current->saved_rsp;
    }

    current        = next;
    current->state = TASK_RUNNING;

    /* Kernel-стек следующей задачи: и TSS.RSP0 (ловушки/IRQ из ring3), и
     * syscall_kernel_stack (вход через инструкцию SYSCALL) указываем на ЕЁ
     * собственный стек. Раньше syscall-стек был глобальным и выставлялся раз
     * при старте процесса → два usermode-процесса делили его и портили друг
     * друга. Теперь он переключается по-задачно — можно много процессов. */
    if (current->stack) {
        uint64_t ktop = (uint64_t)(current->stack + TASK_STACK_SIZE);
        gdt_set_kernel_stack(ktop);
        syscall_set_kernel_stack(ktop);
    }

    /* Адресное пространство (CR3) следующей задачи. У каждого usermode-процесса
     * своя таблица страниц → своя память. Ядро замаплено в КАЖДЫЙ pml4 (верхняя
     * половина), поэтому код/стек ядра остаются доступны после смены CR3. */
    if (current->pml4) {
        vmm_switch((pte_t *)current->pml4);
    }

    return current->saved_rsp;
}
