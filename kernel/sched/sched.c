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
static task_t  *idle_task  = 0;   /* истинный fallback: бежит ТОЛЬКО когда больше некому */
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

static task_t *pick_next(void) {
    if (!run_queue) return 0;
    /* БАГФИКС (зависание при закрытии окна): task_exit() снимает текущую
     * задачу с очереди, и queue_remove обнуляет её ->next. Старт обхода с
     * current->next тогда разыменовывал NULL прямо в IRQ → page fault ядра →
     * вся машина висла намертво при первом же exit процесса. Если текущая
     * задача больше не в кольце — начинаем обход с головы очереди. */
    task_t *start = (current && current->next) ? current->next : run_queue;
    /* Проход 1: предпочитаем ЛЮБУЮ готовую задачу, кроме idle. idle — это
     * настоящий fallback (бесконечный hlt), он НЕ должен воровать кванты у
     * реальной работы (рендер/док). Раньше idle крутился в round-robin как
     * обычная задача и забирал по тику каждый цикл → рендер терял кадры и
     * картина дёргалась. */
    task_t *t = start;
    do { if (t->state == TASK_READY && t != idle_task) return t; t = t->next; } while (t != start);
    /* Проход 2: больше готовых нет — отдаём процессор idle, если он готов. */
    t = start;
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
    current   = idle;
    idle_task = idle;   /* запоминаем idle, чтобы pick_next держал его как fallback */
}

task_t *task_create(const char *name, void (*entry)(void), uint8_t priority) {
    if (priority < 1)  priority = 1;
    if (priority > 10) priority = 10;

    /* Сначала пробуем ПЕРЕИСПОЛЬЗОВАТЬ слот мёртвой задачи (она снята с
     * очереди и никогда больше не получит CPU — слот и её kernel-стек
     * свободны). Иначе после ~сотни запусков/закрытий приложений tasks[]
     * кончался и spawn молча отказывал. kfree для стека не годится:
     * kmalloc_aligned возвращает смещённый указатель — стек просто
     * переиспользуем вместе со слотом. */
    task_t *t = 0;
    for (uint32_t i = 0; i < task_count; i++)
        if (tasks[i].state == TASK_DEAD) { t = &tasks[i]; break; }
    if (!t) {
        if (task_count >= MAX_TASKS) return 0;
        t = &tasks[task_count++];
    }
    t->pid      = next_pid++;
    t->state    = TASK_READY;
    t->priority = priority;
    t->ticks    = priority;
    t->next     = 0;
    t->userdata = 0;   /* в переиспользованном слоте мог остаться мусор */
    t->n_allocs = 0;   /* список kfree-при-выходе: чистый для нового владельца */
    for (int k = 0; k < TASK_MAX_ALLOCS; k++) t->allocs[k] = 0;
    /* По умолчанию задача живёт в kernel-пространстве. usermode-задача
     * перезапишет это своим user-pml4 (см. userspace_elf_loader_task). */
    t->pml4     = (void *)vmm_kernel_pml4;

    int i = 0;
    while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;

    /* Kernel stack — выровнен на 32768 как в VortexOS. У переиспользованного
     * слота стек уже есть — берём его же (владелец мёртв и снят с CPU). */
    if (!t->stack)
        t->stack = (uint8_t *)kmalloc_aligned(TASK_STACK_SIZE, TASK_STACK_SIZE);
    if (!t->stack) { t->state = TASK_DEAD; return 0; }

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

/* Жива ли задача с таким pid (для IPC: не создавать mailbox мертвецам). */
int sched_pid_alive(uint32_t pid) {
    if (!pid) return 0;
    for (uint32_t i = 0; i < task_count; i++)
        if (tasks[i].pid == pid && tasks[i].state != TASK_DEAD) return 1;
    return 0;
}

int task_track_alloc(task_t *t, void *raw) {
    if (!t) t = current;
    if (!t || !raw) return -1;
    if (t->n_allocs >= TASK_MAX_ALLOCS) return -1;
    t->allocs[t->n_allocs++] = raw;
    return 0;
}

void task_exit(void) {
    extern void ipc_on_task_exit(uint32_t pid);
    __asm__ volatile("cli");
    ipc_on_task_exit(current->pid);   /* mailbox, сервисы, grab, shm-refs */

    /* ФИКС УТЕЧКИ: возвращаем ВСЮ память процесса.
     * 1. Уходим с его CR3 (нельзя освобождать таблицы, по которым бежим).
     * 2. Сносим user page table (PML4 + все таблицы нижней половины — PMM).
     * 3. kfree всех heap-аллокаций задачи (ELF-сегменты, user-стек, путь).
     * Раньше всё это жило вечно: каждый запуск приложения навсегда съедал
     * сотни КБ heap + десятки PMM-страниц таблиц. Kernel-стек задачи НЕ
     * освобождаем — он переиспользуется вместе со слотом (см. task_create),
     * и мы прямо сейчас на нём стоим. */
    vmm_switch(vmm_kernel_pml4);
    if (current->pml4 && current->pml4 != (void *)vmm_kernel_pml4) {
        vmm_destroy_user_pml4((pte_t *)current->pml4);
        current->pml4 = (void *)vmm_kernel_pml4;
    }
    for (uint8_t k = 0; k < current->n_allocs; k++) {
        kfree(current->allocs[k]);
        current->allocs[k] = 0;
    }
    current->n_allocs = 0;
    current->userdata = 0;

    current->state = TASK_DEAD;
    queue_remove(current);
    vos_need_resched = 1;
    /* БАГФИКС: hlt с ВЫКЛЮЧЕННЫМИ прерываниями никогда не проснётся — машина
     * висла бы намертво при первом же выходе процесса. Включаем прерывания:
     * первый IRQ вызовет sched_pick, который (раз мы DEAD и сняты с очереди)
     * переключится на другую задачу и сюда больше не вернётся. */
    for (;;) __asm__ volatile("sti\n\thlt");
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

/* "Block, don't poll." Помечает ТЕКУЩУЮ задачу как уснувшую (BLOCKED) и просит
 * планировщик переключиться. Пока задача BLOCKED, pick_next её не выбирает —
 * она вообще не получает процессор и не тратит квант впустую (в отличие от
 * busy-poll цикла, который жрёт весь свой квант). Разбудить можно sched_wake().
 * Вызывать ОБЯЗАТЕЛЬНО с выключенными прерываниями (cli) вместе с проверкой
 * условия и регистрацией будильщика — иначе возможен lost-wakeup. */
void sched_block_current(void) {
    if (!current) return;
    current->state   = TASK_BLOCKED;
    vos_need_resched = 1;
}

/* Будит уснувшую задачу: BLOCKED -> READY и просит планировщик пересмотреть
 * выбор. Зовётся из IRQ (например, клавиатура положила событие в окно). Если
 * задача не спит — ничего не делаем. */
void sched_wake(task_t *t) {
    if (!t) return;
    if (t->state == TASK_BLOCKED) {
        t->state         = TASK_READY;
        vos_need_resched = 1;
    }
}

task_t *sched_current(void) { return current; }
void    sched_clear_resched(void) { vos_need_resched = 0; }

uint64_t sched_pick(uint64_t frame_rsp) {
    current->saved_rsp = frame_rsp;
    /* Только РАБОТАВШУЮ задачу возвращаем в очередь готовых. Если задача сама
     * себя усыпила (TASK_BLOCKED) или завершилась (TASK_DEAD) — НЕ трогаем её
     * состояние, иначе уснувшая задача мгновенно «проснулась» бы обратно и
     * busy-poll вернулся бы. idle никогда не блокируется, поэтому pick_next
     * всегда найдёт кого запустить — взаимной блокировки нет. */
    if (current->state == TASK_RUNNING)
        current->state = TASK_READY;
    vos_need_resched   = 0;

    task_t *next = pick_next();
    if (!next || next == current) {
        /* Других готовых задач нет. Вернуть процессор текущей можно ТОЛЬКО если
         * она готова бежать (не уснула и не умерла). */
        if (current->state == TASK_READY)
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
