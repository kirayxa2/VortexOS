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

static void shm_drop_ref(int id, uint32_t pid);   /* нужна в ipc_on_task_exit */

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
    /* Мёртвым/несуществующим pid mailbox не создаём: иначе каждое сообщение
     * умершему клиенту (close, события) навсегда занимало слот mailbox'а. */
    if (!sched_pid_alive((uint32_t)dst_pid)) return (uint64_t)-1;
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

/* Отправка сообщения ИЗ ЯДРА от имени задачи sender_pid (sys_write → stdout
 * шелла, task_exit → CHILD_EXIT). Тот же путь, что ipc_sys_send, но сообщение
 * уже в kernel-памяти. Безопасна и при включённых, и при выключенных
 * прерываниях: IF сохраняется и восстанавливается. 0 = доставлено. */
uint64_t ipc_kernel_send(uint32_t dst_pid, const uint64_t *msg8, uint32_t sender_pid) {
    if (!msg8 || !sched_pid_alive(dst_pid)) return (uint64_t)-1;
    uint64_t ok = (uint64_t)-1, flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags));
    ipc_mailbox_t *mb = mailbox_get(dst_pid, 1);
    if (mb && mailbox_push(mb, msg8, sender_pid)) ok = 0;
    if (flags & 0x200) __asm__ volatile("sti");
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
        /* SYS_KILL: убитая задача не должна снова заснуть в recv — выходим
         * из syscall'а, диспетчер увидит pending_kill и вызовет task_exit. */
        if (self->pending_kill) return 0;

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

/* SYS_KILL: разбудить задачу, спящую в ipc_sys_recv, чтобы она дошла до
 * проверки pending_kill (иначе recv(FOREVER) спал бы до первого сообщения). */
void ipc_force_wake(uint32_t pid) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags));
    ipc_mailbox_t *mb = mailbox_get(pid, 0);
    if (mb && mb->waiter) {
        sched_wake((task_t *)mb->waiter);
        mb->waiter = 0;
        mb->wait_deadline = 0;
    }
    if (flags & 0x200) __asm__ volatile("sti");
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

/* Задача умерла (task_exit, прерывания уже выключены): освобождаем её
 * mailbox, сервисы и input grab. Без этого каждый запуск/закрытие приложения
 * навсегда съедал слот mailbox'а (32 циклов открыл-закрыл терминал — и IPC
 * мёртв), а сервис/grab продолжали указывать на труп. */
void ipc_on_task_exit(uint32_t pid) {
    if (!pid) return;
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        if (mailboxes[i].owner_pid == pid) {
            mailboxes[i].owner_pid = 0;
            mailboxes[i].head = mailboxes[i].tail = 0;
            mailboxes[i].waiter = 0;
            mailboxes[i].wait_deadline = 0;
        }
    }
    for (int i = 0; i < IPC_SVC_MAX; i++)
        if (svc_table[i] == pid) svc_table[i] = 0;
    if (input_grabber_pid == pid) {
        input_grabber_pid = 0;
        input_grabber_mb  = 0;
    }
    /* ФИКС УТЕЧКИ: отпускаем все shm-ссылки умершего процесса. Последний
     * держатель освобождает страницы сегмента обратно в PMM — раньше каждое
     * окно навсегда съедало слот (24 окна за сессию — и shm кончался). */
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        shm_drop_ref(i, pid);
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
 *
 * Жизненный цикл (ФИКС УТЕЧКИ — раньше сегменты жили вечно, ~24 окна на
 * сессию и конец): на каждом сегменте refcount в виде списка pid'ов, которые
 * его держат (создатель + все, кто сделал shm_map). Ссылку отпускают:
 *   - смерть процесса (ipc_on_task_exit), или
 *   - явный sys_shm_release — им vwm отдаёт поверхность закрытого окна.
 * Когда pid'ов не осталось — страницы уходят обратно в PMM, слот свободен.
 * ---------------------------------------------------------------------- */
typedef struct {
    uint64_t size;                  /* 0 = слот свободен */
    uint32_t pids[SHM_REF_MAX];     /* кто держит сегмент (0 = пусто) */
} shm_seg_t;

static shm_seg_t shm_segs[SHM_MAX_SEGS];

/* Вернуть PMM первые n_pages страниц слота и снять их маппинг из kernel pml4.
 * Вызывать под cli. */
static void shm_free_pages(int id, uint64_t n_pages) {
    uint64_t kvaddr = SHM_KERNEL_BASE + (uint64_t)id * SHM_SLOT_SIZE;
    for (uint64_t p = 0; p < n_pages; p++) {
        uint64_t phys = vmm_virt_to_phys(vmm_kernel_pml4, kvaddr + p * 4096);
        if (!phys) continue;
        vmm_unmap(vmm_kernel_pml4, kvaddr + p * 4096);
        pmm_free(phys);
    }
}

/* pid отпускает ссылку на сегмент; последний гасит свет (страницы — в PMM).
 * Вызывать под cli. */
static void shm_drop_ref(int id, uint32_t pid) {
    shm_seg_t *s = &shm_segs[id];
    if (s->size == 0 || s->size == (uint64_t)-1) return;  /* свободен/строится */
    int had = 0, left = 0;
    for (int r = 0; r < SHM_REF_MAX; r++) {
        if (s->pids[r] == pid) { s->pids[r] = 0; had = 1; }
        if (s->pids[r]) left = 1;
    }
    if (had && !left) {
        shm_free_pages(id, (s->size + 4095) / 4096);
        s->size = 0;
    }
}

/* Добавить pid в держатели сегмента (идемпотентно). Вызывать под cli.
 * Возврат: 0 = ок, -1 = список полон (сегмент не дадим, иначе утечёт). */
static int shm_add_ref(int id, uint32_t pid) {
    shm_seg_t *s = &shm_segs[id];
    int free_slot = -1;
    for (int r = 0; r < SHM_REF_MAX; r++) {
        if (s->pids[r] == pid) return 0;
        if (s->pids[r] == 0 && free_slot < 0) free_slot = r;
    }
    if (free_slot < 0) return -1;
    s->pids[free_slot] = pid;
    return 0;
}

uint64_t ipc_sys_shm_create(uint64_t size) {
    if (size == 0 || size > SHM_SLOT_SIZE) return (uint64_t)-1;
    task_t *self = sched_current();
    if (!self) return (uint64_t)-1;

    int id = -1;
    __asm__ volatile("cli");
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        if (shm_segs[i].size == 0) { id = i; break; }
    if (id >= 0) {
        shm_segs[id].size = (uint64_t)-1;   /* бронь слота на время маппинга */
        for (int r = 0; r < SHM_REF_MAX; r++) shm_segs[id].pids[r] = 0;
    }
    __asm__ volatile("sti");
    if (id < 0) return (uint64_t)-1;

    uint64_t pages = (size + 4095) / 4096;
    uint64_t kvaddr = SHM_KERNEL_BASE + (uint64_t)id * SHM_SLOT_SIZE;

    for (uint64_t p = 0; p < pages; p++) {
        uint64_t phys = pmm_alloc();
        if (!phys) {
            fb_puts("[IPC] shm_create: out of memory\n");
            /* ФИКС: уже выделенные страницы — обратно в PMM, слот — свободен
             * (раньше они навсегда оставались висеть на слоте). */
            __asm__ volatile("cli");
            shm_free_pages(id, p);
            shm_segs[id].size = 0;
            __asm__ volatile("sti");
            return (uint64_t)-1;
        }
        vmm_map(vmm_kernel_pml4, kvaddr + p * 4096, phys,
                VMM_PRESENT | VMM_WRITABLE);
    }

    /* Обнуляем сегмент (свежие PMM-страницы могут содержать мусор). */
    uint64_t *z = (uint64_t *)kvaddr;
    for (uint64_t i = 0; i < pages * 4096 / 8; i++) z[i] = 0;

    __asm__ volatile("cli");
    shm_segs[id].size = size;
    shm_add_ref(id, self->pid);        /* создатель держит первую ссылку */
    __asm__ volatile("sti");
    return (uint64_t)id;
}

/* Маппит сегмент в адресное пространство ТЕКУЩЕГО процесса на фиксированный
 * адрес SHM_USER_BASE + id*16MB (одинаковый во всех процессах). */
uint64_t ipc_sys_shm_map(uint64_t shm_id) {
    extern uint64_t hhdm_offset;
    if (shm_id >= SHM_MAX_SEGS) return 0;
    if (shm_segs[shm_id].size == 0 ||
        shm_segs[shm_id].size == (uint64_t)-1) return 0;  /* свободен/строится */
    task_t *self = sched_current();
    if (!self) return 0;

    /* Берём ссылку ДО маппинга: если список держателей полон — отказ,
     * иначе сегмент стал бы невозможно освободить. */
    __asm__ volatile("cli");
    int ref_ok = shm_add_ref((int)shm_id, self->pid);
    __asm__ volatile("sti");
    if (ref_ok < 0) return 0;

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

/* Явно отпустить сегмент: снять маппинг из СВОЕГО адресного пространства и
 * отдать ссылку. Этим vwm освобождает поверхность закрытого окна — иначе его
 * ссылка держала бы сегмент до конца жизни vwm, и слоты бы кончились так же,
 * как раньше (просто медленнее). После release трогать буфер нельзя. */
uint64_t ipc_sys_shm_release(uint64_t shm_id) {
    extern uint64_t hhdm_offset;
    if (shm_id >= SHM_MAX_SEGS) return (uint64_t)-1;
    if (shm_segs[shm_id].size == 0 ||
        shm_segs[shm_id].size == (uint64_t)-1) return (uint64_t)-1;
    task_t *self = sched_current();
    if (!self) return (uint64_t)-1;

    uint64_t pages  = (shm_segs[shm_id].size + 4095) / 4096;
    uint64_t uvaddr = SHM_USER_BASE + shm_id * SHM_SLOT_SIZE;

    /* Снимаем user-маппинг (CR3 сейчас наш — invlpg попадает куда надо). */
    uint64_t cr3_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    cr3_phys &= ~0xFFFULL;
    pte_t *pml4 = (pte_t *)(cr3_phys + hhdm_offset);
    for (uint64_t p = 0; p < pages; p++)
        vmm_unmap(pml4, uvaddr + p * 4096);

    __asm__ volatile("cli");
    shm_drop_ref((int)shm_id, self->pid);
    __asm__ volatile("sti");
    return 0;
}
