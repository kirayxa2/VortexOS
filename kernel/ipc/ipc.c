/* =============================================================================
 * VortexOS — kernel/ipc/ipc.c
 * IPC-сообщения + service registry + input grab + shared memory.
 *
 * Это фундамент userspace window manager'а: ядро больше не обязано рисовать
 * окна само — оно лишь доставляет сообщения, ввод и расшаренную память.
 *
 * Модель синхронизации: все очереди мутируются ТОЛЬКО под cli (из IRQ
 * прерывания уже выключены; syscall-пути выключают их явно). Один потребитель
 * на mailbox (его владелец), производителей много (IRQ + другие задачи).
 * Блокировка получателя — тот же паттерн «block, don't poll» c защитой от
 * lost-wakeup, что и в wm_wait_event: проверка очереди, регистрация waiter и
 * sched_block_current() под cli; `sti; hlt` неделим.
 * ============================================================================= */

#include "ipc.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"

extern void fb_puts(const char *);

/* -------------------------------------------------------------------------
 * Mailbox'ы
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t owner_pid;                 /* 0 = слот свободен */
    uint32_t head, tail;                /* кольцевая очередь */
    uint64_t msgs[IPC_QUEUE_LEN][IPC_MSG_WORDS];
    void    *waiter;                    /* task_t*, спит в ipc_sys_recv */
    uint64_t wait_deadline;             /* pit_ticks дедлайн; 0 = без таймаута */
} ipc_mailbox_t;

#define MAX_MAILBOXES 32
static ipc_mailbox_t mailboxes[MAX_MAILBOXES];

static uint32_t svc_table[IPC_SVC_MAX];   /* svc_id -> pid (0 = не зарегистрирован) */

static uint32_t input_grabber_pid = 0;    /* pid userspace WM, забравшего ввод */
static ipc_mailbox_t *input_grabber_mb = 0;

void ipc_init(void) {
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        mailboxes[i].owner_pid = 0;
        mailboxes[i].head = mailboxes[i].tail = 0;
        mailboxes[i].waiter = 0;
        mailboxes[i].wait_deadline = 0;
    }
    for (int i = 0; i < IPC_SVC_MAX; i++) svc_table[i] = 0;
    input_grabber_pid = 0;
    input_grabber_mb  = 0;
}

/* Найти mailbox процесса; создать, если ещё нет (лениво, при первом обращении).
 * Вызывать под cli. */
static ipc_mailbox_t *mailbox_get(uint32_t pid, int create) {
    if (!pid) return 0;
    for (int i = 0; i < MAX_MAILBOXES; i++)
        if (mailboxes[i].owner_pid == pid) return &mailboxes[i];
    if (!create) return 0;
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        if (mailboxes[i].owner_pid == 0) {
            mailboxes[i].owner_pid = pid;
            mailboxes[i].head = mailboxes[i].tail = 0;
            mailboxes[i].waiter = 0;
            mailboxes[i].wait_deadline = 0;
            return &mailboxes[i];
        }
    }
    return 0;
}

/* Положить сообщение в mailbox и разбудить ждущего. Вызывать под cli.
 * Возвращает указатель на слот (для коалесинга) или 0, если очередь полна. */
static uint64_t *mailbox_push(ipc_mailbox_t *mb, const uint64_t *msg, uint32_t sender_pid) {
    uint32_t next = (mb->tail + 1) % IPC_QUEUE_LEN;
    if (next == mb->head) return 0;            /* очередь полна — теряем */
    uint64_t *slot = mb->msgs[mb->tail];
    for (int i = 0; i < IPC_MSG_WORDS - 1; i++) slot[i] = msg[i];
    slot[7] = sender_pid;                      /* ядро вписывает отправителя */
    mb->tail = next;
    if (mb->waiter) {
        sched_wake((task_t *)mb->waiter);
        mb->waiter = 0;
    }
    return slot;
}

/* -------------------------------------------------------------------------
 * Syscall: send / recv
 * ---------------------------------------------------------------------- */
uint64_t ipc_sys_send(uint64_t dst_pid, uint64_t user_msg) {
    if (!user_msg) return (uint64_t)-1;
    task_t *self = sched_current();
    uint32_t sender = self ? self->pid : 0;

    /* Копируем сообщение из userspace ДО cli (page fault тут безопаснее). */
    uint64_t tmp[IPC_MSG_WORDS];
    const uint64_t *src = (const uint64_t *)user_msg;
    for (int i = 0; i < IPC_MSG_WORDS; i++) tmp[i] = src[i];

    uint64_t ok = (uint64_t)-1;
    __asm__ volatile("cli");
    ipc_mailbox_t *mb = mailbox_get((uint32_t)dst_pid, 1);
    if (mb && mailbox_push(mb, tmp, sender)) ok = 0;
    __asm__ volatile("sti");
    return ok;
}

/* timeout_ticks: ~0ULL = ждать вечно, 0 = не блокироваться (poll),
 * N = ждать максимум N тиков PIT. Возврат: 1 = сообщение получено, 0 = таймаут. */
uint64_t ipc_sys_recv(uint64_t user_msg, uint64_t timeout_ticks) {
    extern uint64_t pit_ticks(void);
    if (!user_msg) return 0;
    task_t *self = sched_current();
    if (!self) return 0;

    uint64_t deadline = 0;
    if (timeout_ticks != (uint64_t)-1)
        deadline = pit_ticks() + timeout_ticks;

    for (;;) {
        __asm__ volatile("cli");
        ipc_mailbox_t *mb = mailbox_get(self->pid, 1);
        if (!mb) { __asm__ volatile("sti"); return 0; }

        if (mb->head != mb->tail) {
            /* Есть сообщение — забираем под cli в локальный буфер. */
            uint64_t tmp[IPC_MSG_WORDS];
            for (int i = 0; i < IPC_MSG_WORDS; i++) tmp[i] = mb->msgs[mb->head][i];
            mb->head = (mb->head + 1) % IPC_QUEUE_LEN;
            __asm__ volatile("sti");
            uint64_t *dst = (uint64_t *)user_msg;
            for (int i = 0; i < IPC_MSG_WORDS; i++) dst[i] = tmp[i];
            return 1;
        }

        if (timeout_ticks == 0 ||
            (deadline && pit_ticks() >= deadline)) {
            __asm__ volatile("sti");
            return 0;                          /* пусто и ждать нельзя/хватит */
        }

        /* Пусто — засыпаем. Лost-wakeup невозможен: waiter и BLOCKED ставятся
         * под cli, а sti;hlt неделим (как в wm_wait_event). */
        mb->waiter = self;
        mb->wait_deadline = deadline;          /* ipc_tick разбудит по таймауту */
        sched_block_current();
        __asm__ volatile("sti\n\thlt");
        /* Проснулись (сообщение или таймаут) — новый круг перечитает очередь. */
    }
}

/* Зовётся из PIT IRQ0 (прерывания выключены): будим получателей с истёкшим
 * таймаутом, иначе recv с timeout проспит дедлайн до следующего сообщения. */
void ipc_tick(void) {
    extern uint64_t pit_ticks(void);
    uint64_t now = pit_ticks();
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        ipc_mailbox_t *mb = &mailboxes[i];
        if (mb->owner_pid && mb->waiter && mb->wait_deadline &&
            now >= mb->wait_deadline) {
            sched_wake((task_t *)mb->waiter);
            mb->waiter = 0;
            mb->wait_deadline = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * Service registry
 * ---------------------------------------------------------------------- */
uint64_t ipc_sys_svc_register(uint64_t svc_id) {
    if (svc_id >= IPC_SVC_MAX) return (uint64_t)-1;
    task_t *self = sched_current();
    if (!self) return (uint64_t)-1;
    svc_table[svc_id] = self->pid;
    return 0;
}

uint64_t ipc_sys_svc_lookup(uint64_t svc_id) {
    if (svc_id >= IPC_SVC_MAX) return 0;
    return svc_table[svc_id];
}

/* -------------------------------------------------------------------------
 * Input grab — весь ввод уходит сообщениями в userspace WM
 * ---------------------------------------------------------------------- */
uint64_t ipc_sys_input_grab(void) {
    task_t *self = sched_current();
    if (!self) return (uint64_t)-1;
    __asm__ volatile("cli");
    input_grabber_pid = self->pid;
    input_grabber_mb  = mailbox_get(self->pid, 1);
    __asm__ volatile("sti");
    fb_puts("[IPC] input grabbed by userspace WM\n");
    return input_grabber_mb ? 0 : (uint64_t)-1;
}

int ipc_input_grabbed(void) {
    return input_grabber_mb != 0;
}

/* Из IRQ клавиатуры (прерывания выключены). */
void ipc_input_push_key(char ascii, int pressed) {
    if (!input_grabber_mb) return;
    uint64_t msg[IPC_MSG_WORDS] = {0};
    msg[0] = IPC_MSG_INPUT_KEY;
    msg[1] = (uint64_t)(uint8_t)ascii;
    msg[2] = (uint64_t)pressed;
    mailbox_push(input_grabber_mb, msg, 0);
}

/* Из IRQ мыши (прерывания выключены). Чистое движение КОАЛЕСИРУЕМ: если
 * последнее непрочитанное сообщение — тоже движение с теми же кнопками,
 * просто суммируем dx/dy. Иначе PS/2 на высокой частоте переполнит очередь
 * быстрее, чем WM успеет отрисовать кадр. Нажатия кнопок не коалесируются. */
void ipc_input_push_mouse(int dx, int dy, uint8_t buttons, int btn_changed) {
    if (!input_grabber_mb) return;
    ipc_mailbox_t *mb = input_grabber_mb;

    if (!btn_changed && mb->head != mb->tail) {
        uint32_t last = (mb->tail + IPC_QUEUE_LEN - 1) % IPC_QUEUE_LEN;
        uint64_t *m = mb->msgs[last];
        if (m[0] == IPC_MSG_INPUT_MOUSE && m[4] == 0 && m[3] == buttons) {
            m[1] = (uint64_t)((int64_t)m[1] + dx);
            m[2] = (uint64_t)((int64_t)m[2] + dy);
            return;
        }
    }

    uint64_t msg[IPC_MSG_WORDS] = {0};
    msg[0] = IPC_MSG_INPUT_MOUSE;
    msg[1] = (uint64_t)(int64_t)dx;
    msg[2] = (uint64_t)(int64_t)dy;
    msg[3] = buttons;
    msg[4] = (uint64_t)btn_changed;
    mailbox_push(mb, msg, 0);
}

/* -------------------------------------------------------------------------
 * Shared memory
 *
 * Страницы берём у PMM по одной (без требования физической смежности) и
 * маппим в kernel pml4 на фиксированный слот SHM_KERNEL_BASE + id*16MB.
 * Физический адрес каждой страницы потом восстанавливаем walk'ом kernel pml4 —
 * никаких массивов phys-страниц хранить не нужно.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint64_t size;      /* 0 = слот свободен */
} shm_seg_t;

static shm_seg_t shm_segs[SHM_MAX_SEGS];

uint64_t ipc_sys_shm_create(uint64_t size) {
    if (size == 0 || size > SHM_SLOT_SIZE) return (uint64_t)-1;

    int id = -1;
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        if (shm_segs[i].size == 0) { id = i; break; }
    if (id < 0) return (uint64_t)-1;

    uint64_t pages = (size + 4095) / 4096;
    uint64_t kvaddr = SHM_KERNEL_BASE + (uint64_t)id * SHM_SLOT_SIZE;

    for (uint64_t p = 0; p < pages; p++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            fb_puts("[IPC] shm_create: out of memory\n");
            return (uint64_t)-1;   /* уже замапленные страницы оставляем слоту */
        }
        vmm_map(vmm_kernel_pml4, kvaddr + p * 4096, phys,
                VMM_PRESENT | VMM_WRITABLE);
    }

    /* Обнуляем сегмент (свежие PMM-страницы могут содержать мусор). */
    uint64_t *z = (uint64_t *)kvaddr;
    for (uint64_t i = 0; i < pages * 4096 / 8; i++) z[i] = 0;

    shm_segs[id].size = size;
    return (uint64_t)id;
}

/* Маппит сегмент в адресное пространство ТЕКУЩЕГО процесса на фиксированный
 * адрес SHM_USER_BASE + id*16MB (одинаковый во всех процессах). */
uint64_t ipc_sys_shm_map(uint64_t shm_id) {
    extern uint64_t hhdm_offset;
    if (shm_id >= SHM_MAX_SEGS || shm_segs[shm_id].size == 0) return 0;

    uint64_t pages  = (shm_segs[shm_id].size + 4095) / 4096;
    uint64_t kvaddr = SHM_KERNEL_BASE + shm_id * SHM_SLOT_SIZE;
    uint64_t uvaddr = SHM_USER_BASE   + shm_id * SHM_SLOT_SIZE;

    /* Текущий user pml4 — из CR3 (как в sys_fb_map). */
    uint64_t cr3_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    cr3_phys &= ~0xFFFULL;
    pte_t *pml4 = (pte_t *)(cr3_phys + hhdm_offset);

    for (uint64_t p = 0; p < pages; p++) {
        uint64_t phys = vmm_virt_to_phys(vmm_kernel_pml4, kvaddr + p * 4096);
        if (!phys) return 0;
        vmm_map(pml4, uvaddr + p * 4096, phys,
                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }
    return uvaddr;
}
