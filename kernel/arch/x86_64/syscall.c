/* =============================================================================
 * VortexOS — kernel/arch/x86_64/syscall.c
 * Системные вызовы через инструкцию SYSCALL (MSR-based, ring0→ring3).
 *
 * Номера syscall:
 *   0  — sys_write(fd, buf, len)  → выводит на экран (fd=1)
 *   1  — sys_exit(code)           → завершает задачу
 *   2  — sys_getpid()             → возвращает PID
 *   3  — sys_sleep(ticks)         → спит N тиков PIT
 * ============================================================================= */

#include "syscall.h"
#include "fb.h"
#include "sched.h"
#include "pit.h"
#include "vmm.h"
#include "simple_wm.h"
#include "virtio_gpu.h"
#include "ipc.h"
#include <stddef.h>  // для size_t

/* MSR адреса */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081  /* CS/SS для syscall/sysret */
#define MSR_LSTAR  0xC0000082  /* RIP обработчика syscall  */
#define MSR_SFMASK 0xC0000084  /* маска RFLAGS             */

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
        :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Объявлена в syscall_entry.asm */
extern void syscall_entry(void);
extern uint64_t syscall_kernel_stack; /* kernel stack для syscall handler */

/* -------------------------------------------------------------------------
 * Обработчики системных вызовов
 * ------------------------------------------------------------------------- */

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    (void)fd; /* пока только stdout */
    const char *s = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) fb_putchar(s[i]);
    return len;
}

static uint64_t sys_exit(uint64_t code) {
    fb_puts("[SYSCALL] sys_exit(");
    fb_puthex(code);
    fb_puts(") — process terminated\n");
    
    /* Переключаемся обратно на kernel PML4 */
    vmm_switch(vmm_kernel_pml4);

    /* БАГФИКС: раньше тут был cli + hlt навсегда — это вешало ВСЮ машину
     * (с выключенными прерываниями hlt никогда не проснётся, планировщик не
     * получает IRQ0 и не может снять задачу с CPU). Теперь честно завершаем
     * задачу: помечаем DEAD, снимаем с очереди и ждём первый IRQ с
     * ВКЛЮЧЁННЫМИ прерываниями — sched_pick переключит на другую задачу и
     * сюда больше никогда не вернётся. */
    task_exit();
    __builtin_unreachable();
}

static uint64_t sys_getpid(void) {
    task_t *t = sched_current();
    return t ? t->pid : 0;
}

static uint64_t sys_sleep(uint64_t ticks) {
    uint64_t end = pit_ticks() + ticks;
    while (pit_ticks() < end)
        __asm__ volatile("hlt");
    return 0;
}

/* Припарковать текущую задачу навсегда (0% CPU). Для приложений, которые
 * нарисовали окно и больше ничего не делают (например, test_window): вместо
 * busy-loop `for(;;) pause`, который жрёт весь квант, задача уходит в TASK_BLOCKED
 * и её больше не ставят на процессор. Никто её не будит — она просто спит, не
 * мешая рендеру и остальным. */
static uint64_t sys_block(void) {
    for (;;) {
        sched_block_current();          /* state=BLOCKED, need_resched=1 */
        __asm__ volatile("sti\n\thlt"); /* неделимо; первый IRQ переключит */
    }
    return 0; /* недостижимо */
}

/* Структура для возврата информации о framebuffer */
typedef struct {
    uint64_t phys_addr;  /* Физический адрес framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} fb_info_t;

static uint64_t sys_fb_info(uint64_t user_buf_addr) {
    extern uint32_t *fb_addr;
    extern uint32_t fb_width, fb_height;
    extern uint64_t fb_pitch;
    
    fb_info_t info;
    
    /* Если активен virtio-gpu — «настоящий» экран это его backing-буфер
     * (Limine fb больше не сканируется). Отдаём его геометрию, иначе
     * userspace-рисование уйдёт в невидимый буфер. */
    if (virtio_gpu_active()) {
        info.phys_addr = vmm_virt_to_phys(vmm_kernel_pml4,
                                          (uint64_t)virtio_gpu_framebuffer());
        info.width  = virtio_gpu_width();
        info.height = virtio_gpu_height();
        info.pitch  = virtio_gpu_pitch();
        info.bpp    = 32;
    } else {
        /* Получаем физический адрес framebuffer */
        info.phys_addr = vmm_virt_to_phys(vmm_kernel_pml4, (uint64_t)fb_addr);
        info.width = fb_width;
        info.height = fb_height;
        info.pitch = fb_pitch;
        info.bpp = 32; /* BGRA 8888 */
    }
    
    /* Копируем в userspace побайтово */
    fb_info_t *user_buf = (fb_info_t *)user_buf_addr;
    uint8_t *src = (uint8_t *)&info;
    uint8_t *dst = (uint8_t *)user_buf;
    for (size_t i = 0; i < sizeof(fb_info_t); i++) {
        dst[i] = src[i];
    }
    
    return 0;
}

static uint64_t sys_fb_map(void) {
    extern uint32_t *fb_addr;
    extern uint32_t fb_width, fb_height;
    extern uint64_t fb_pitch;
    extern uint64_t hhdm_offset; /* HHDM offset из vmm */

    /* Какой буфер на самом деле показывается? При активном virtio-gpu это его
     * backing (kmalloc-память ядра, физически НЕ смежная — phys ищем walk'ом
     * на каждую страницу). Иначе — линейный Limine framebuffer (физически
     * смежный). */
    uint64_t base_virt, fb_size;
    int contiguous;
    if (virtio_gpu_active()) {
        base_virt  = (uint64_t)virtio_gpu_framebuffer();
        fb_size    = (uint64_t)virtio_gpu_height() * virtio_gpu_pitch();
        contiguous = 0;
    } else {
        base_virt  = (uint64_t)fb_addr;
        fb_size    = fb_height * fb_pitch;
        contiguous = 1;
    }

    uint64_t fb_phys = 0;
    if (contiguous) {
        if (base_virt >= hhdm_offset) {
            /* HHDM адрес — используем простую арифметику */
            fb_phys = base_virt - hhdm_offset;
        } else {
            /* Попытка через page table walk */
            fb_phys = vmm_virt_to_phys(vmm_kernel_pml4, base_virt);
            if (!fb_phys) return 0;
        }
    }

    /* Мапим framebuffer в userspace на фиксированный адрес 0x80000000 */
    uint64_t user_fb_vaddr = 0x80000000;
    uint64_t num_pages = (fb_size + 4095) / 4096;

    /* Получаем текущий PML4 из CR3 (физический адрес) */
    uint64_t cr3_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
    cr3_phys &= ~0xFFF; /* Убираем флаги */

    /* Преобразуем физический адрес PML4 в виртуальный через HHDM */
    pte_t *user_pml4 = (pte_t *)(cr3_phys + hhdm_offset);

    /* Мапим страницы framebuffer в USER page table */
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys;
        if (contiguous) {
            page_phys = fb_phys + i * 4096;
        } else {
            page_phys = vmm_virt_to_phys(vmm_kernel_pml4, base_virt + i * 4096);
            if (!page_phys) return 0;
        }
        vmm_map(user_pml4,
                user_fb_vaddr + i * 4096,
                page_phys,
                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }
    
    return user_fb_vaddr;
}

/* -------------------------------------------------------------------------
 * Syscall'ы для userspace window manager'а (feat/userspace-wm)
 * ------------------------------------------------------------------------- */

/* Запустить ELF с диска: sys_spawn("/vterm"). Путь копируем в kernel-память —
 * userdata читается загрузчиком уже в ДРУГОМ адресном пространстве. */
static uint64_t sys_spawn(uint64_t user_path) {
    extern void userspace_elf_loader_task(void);
    extern void *kmalloc(uint64_t);
    if (!user_path) return (uint64_t)-1;

    const char *src = (const char *)user_path;
    char *path = (char *)kmalloc(64);
    if (!path) return (uint64_t)-1;
    int i = 0;
    while (src[i] && i < 63) { path[i] = src[i]; i++; }
    path[i] = 0;
    if (i == 0) return (uint64_t)-1;

    task_t *t = task_create("app", userspace_elf_loader_task, 10);
    if (!t) return (uint64_t)-1;
    t->userdata = path;
    return t->pid;
}

/* Текущее время RTC (CMOS) — для часов на панели userspace WM. */
static inline uint8_t cmos_read_sc(uint8_t reg) {
    uint8_t v;
    __asm__ volatile("outb %0, $0x70" :: "a"(reg));
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

static uint64_t sys_rtc(uint64_t user_buf) {
    if (!user_buf) return (uint64_t)-1;
    uint8_t h = cmos_read_sc(0x04), m = cmos_read_sc(0x02), s = cmos_read_sc(0x00);
    uint8_t status_b = cmos_read_sc(0x0B);
    if (!(status_b & 0x04)) {   /* BCD-режим — конвертируем */
        h = (uint8_t)((h >> 4) * 10 + (h & 0x0F));
        m = (uint8_t)((m >> 4) * 10 + (m & 0x0F));
        s = (uint8_t)((s >> 4) * 10 + (s & 0x0F));
    }
    uint32_t *out = (uint32_t *)user_buf;
    out[0] = h; out[1] = m; out[2] = s;
    return 0;
}

/* Программный vsync для userspace WM (Limine-путь): ждём начало vblank через
 * VGA Input Status 1 (порт 0x3DA, бит 3). На UEFI/GOP-железе регистр может
 * быть мёртв — оба цикла с таймаутом, при срабатывании сами отключаемся.
 * При virtio-gpu vsync не нужен (RESOURCE_FLUSH показывает кадр атомарно). */
static int user_vsync_enabled = 1;

static uint64_t sys_vsync(void) {
    if (virtio_gpu_active() || !user_vsync_enabled) return 0;
    uint8_t v; uint32_t guard;
    /* ОПТИМИЗАЦИЯ (FPS при drag): если мы УЖЕ в vblank — рисуем сразу.
     * Раньше ждали конец текущего vblank и потом начало следующего — это
     * до целого кадра (~16 мс) простоя на КАЖДЫЙ present. При перетаскивании
     * окна это резало частоту кадров примерно вдвое. */
    __asm__ volatile("inb $0x3DA, %0" : "=a"(v));
    if (v & 0x08) return 0;
    guard = 1000000;
    do {
        __asm__ volatile("inb $0x3DA, %0" : "=a"(v));
    } while (!(v & 0x08) && --guard);
    if (!guard) user_vsync_enabled = 0;
    return 0;
}

/* Present для virtio-gpu: после записи пикселей в backing нужно явно запушить
 * прямоугольник в хост (TRANSFER + FLUSH). На Limine-пути — no-op (запись в
 * linear framebuffer видна сразу). */
static uint64_t sys_fb_present(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (virtio_gpu_active())
        virtio_gpu_flush((int)(int64_t)x, (int)(int64_t)y,
                         (int)(int64_t)w, (int)(int64_t)h);
    return 0;
}

/* Структура для событий ввода */
typedef struct {
    uint8_t type;    /* 0=нет события, 1=мышь, 2=клавиатура */
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
    uint16_t key_code;
    uint8_t key_pressed;
} input_event_t;

static uint64_t sys_input_poll(uint64_t user_buf_addr) {
    /* TODO: реализовать очередь событий */
    /* Пока возвращаем "нет событий" */
    uint8_t *event_ptr = (uint8_t *)user_buf_addr;
    event_ptr[0] = 0; // type = 0 (no event)
    return 0;
}

/* -------------------------------------------------------------------------
 * Диспетчер — вызывается из syscall_entry.asm
 * Аргументы по System V AMD64 ABI для syscall:
 *   rax = номер,  rdi = arg1,  rsi = arg2,  rdx = arg3
 * ------------------------------------------------------------------------- */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (num) {
        case 0: return sys_write(a1, a2, a3);
        case 1: return sys_exit(a1);
        case 2: return sys_getpid();
        case 3: return sys_sleep(a1);
        case 4: return sys_fb_info(a1);
        case 5: return sys_fb_map();
        case 6: return sys_input_poll(a1);
        case 10: // SYS_WM_CREATE_WINDOW
            return wm_create_window((const char *)a1, (int32_t)a2, (int32_t)a3, 
                                   (int32_t)a4, (int32_t)a5, sys_getpid());
        case 11: // SYS_WM_DRAW_RECT
            wm_draw_rect(a1, (int32_t)a2, (int32_t)a3, (int32_t)a4, (int32_t)a5, (uint32_t)a6);
            return 0;
        case 12: // SYS_WM_DRAW_STRING
            wm_draw_string(a1, (int32_t)a2, (int32_t)a3, (const char *)a4, (uint32_t)a5);
            return 0;
        case 13: // SYS_WM_FLUSH
            wm_flush(a1);
            return 0;
        case 14: // SYS_WM_GET_EVENT
            return wm_get_event(a1, (void *)a2);
        case 15: // SYS_WM_WAIT_EVENT — блокирующее ожидание (block, don't poll)
            return wm_wait_event(a1, (void *)a2);
        case 16: // SYS_BLOCK — припарковать задачу навсегда (0% CPU)
            return sys_block();

        /* --- userspace window manager (feat/userspace-wm) --- */
        case 17: return ipc_sys_send(a1, a2);          // SYS_IPC_SEND(dst_pid, msg)
        case 18: return ipc_sys_recv(a1, a2);          // SYS_IPC_RECV(msg, timeout)
        case 19: return ipc_sys_shm_create(a1);        // SYS_SHM_CREATE(size) -> id
        case 20: return ipc_sys_shm_map(a1);           // SYS_SHM_MAP(id) -> vaddr
        case 21: return ipc_sys_input_grab();          // SYS_INPUT_GRAB
        case 22: return ipc_sys_svc_register(a1);      // SYS_SVC_REGISTER(svc)
        case 23: return ipc_sys_svc_lookup(a1);        // SYS_SVC_LOOKUP(svc) -> pid
        case 24: return sys_spawn(a1);                 // SYS_SPAWN(path) -> pid
        case 25: return pit_ticks();                   // SYS_UPTIME -> тики PIT (100 Hz)
        case 26: return sys_rtc(a1);                   // SYS_RTC(buf: 3 x uint32 h,m,s)
        case 27: return sys_vsync();                   // SYS_VSYNC — ждать vblank
        case 28: return sys_fb_present(a1,a2,a3,a4);   // SYS_FB_PRESENT(x,y,w,h)
        default: return (uint64_t)-1;
    }
}

void syscall_set_kernel_stack(uint64_t rsp0) {
    syscall_kernel_stack = rsp0;
}

void syscall_init(void) {
    /* Включаем SCE (System Call Extensions) в EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;
    wrmsr(MSR_EFER, efer);

    /*
     * STAR:
     *   bits 63:48 — CS для sysret  (ring3 code = 0x1B, data = 0x23)
     *   bits 47:32 — CS для syscall (ring0 code = 0x08, data = 0x10)
     * Формат: [sysret_cs | sysret_ss-8] [syscall_cs]
     */
    /*
     * STAR[63:48]: база для sysretq.
     *   sysretq ставит CS = STAR[63:48] + 16, SS = STAR[63:48] + 8
     *   Нам нужно CS=0x1B (user code) и SS=0x23 (user data).
     *   0x1B = 0x0B + 16  →  STAR[63:48] = 0x0B
     *   0x23 = 0x0B + 8   →  SS=0x13? Нет — 0x0B+8=0x13, не 0x23.
     *
     * GDT порядок: 0=null, 1=kcode(0x08), 2=kdata(0x10),
     *              3=ucode(0x18/0x1B), 4=udata(0x20/0x23)
     *   Нужно SS=STAR+8=0x23 → STAR=0x1B; CS=STAR+16=0x2B — нет.
     *
     * Правильный порядок для sysretq: SS должен быть ПЕРЕД CS в GDT.
     * Linux решает это так: STAR[63:48]=0x10 (user32 CS базовый):
     *   SS = 0x10+8 = 0x18, CS = 0x10+16 = 0x20 — тоже не то для нас.
     *
     * Единственное решение для нашего GDT:
     *   user_data(0x20) + 8 = 0x28 (нет), user_code(0x18) + 16 = 0x28 (нет).
     *
     * НУЖНО переставить GDT: null, kcode, kdata, udata, ucode, TSS
     * тогда: udata=0x18(0x1B), ucode=0x20(0x23)
     *   STAR[63:48]=0x13: SS=0x13+8=0x1B, CS=0x13+16=0x23 — нет.
     *
     * Правильно для Linux-совместимого GDT:
     *   null=0, kcode=0x08, kdata=0x10, ucode=0x18, udata=0x20
     *   STAR[63:48]=0x08: SS=0x08+8=0x10(kernel!), CS=0x08+16=0x18 — нет.
     *
     * ОКОНЧАТЕЛЬНО верное решение (как у Linux):
     *   GDT: null, kcode(0x08), kdata(0x10), udata(0x18), ucode(0x20)
     *   STAR[63:48]=0x10: SS=0x10+8=0x18(udata+RPL3=0x1B ✓),
     *                      CS=0x10+16=0x20(ucode+RPL3=0x23 ✓)
     * Но менять GDT сейчас опасно — сломает idt/tss.
     *
     * ПРОСТЕЙШЕЕ решение: оставить GDT как есть, но задать STAR[63:48]
     * так чтобы +16=user_code_selector_base:
     *   user_code в GDT = индекс 3 = selector 0x18.
     *   Нам нужно STAR[63:48]+16 = 0x18 → STAR[63:48] = 0x08.
     *   Тогда SS = 0x08+8 = 0x10 = kernel data — НЕВЕРНО для SS.
     *
     * Вывод: нужно поменять порядок user сегментов в GDT:
     *   3=udata(0x18), 4=ucode(0x20)
     *   Тогда STAR[63:48]=0x10:
     *     SS = 0x10+8 = 0x18|3 = 0x1B (udata RPL3) ✓
     *     CS = 0x10+16 = 0x20|3 = 0x23 (ucode RPL3) ✓
     *   И enter_usermode должен использовать CS=0x23, SS=0x1B.
     */
    /* STAR[63:48]=0x10 для sysretq: CS=0x20|3=0x23(ucode), SS=0x18|3=0x1B(udata)
     * STAR[47:32]=0x08 для syscall:  CS=0x08(kcode), SS=0x10(kdata) */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    /* LSTAR — адрес точки входа syscall */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK — маскируем IF (прерывания) при входе в syscall */
    wrmsr(MSR_SFMASK, (1 << 9)); /* IF = бит 9 */

    fb_puts("[OK] Syscall initialized\n");
}
