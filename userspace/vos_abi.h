/* =============================================================================
 * VortexOS — userspace/vos_abi.h
 * ABI userspace window manager'а (feat/userspace-wm):
 *   - номера новых syscall'ов (IPC / shm / input grab / spawn / время / vsync)
 *   - протокол сообщений клиент <-> WM (поверх 64-байтных IPC-сообщений)
 *   - клиентские хелперы (создать окно, закоммитить кадр, разобрать события)
 *
 * Архитектура «по-взрослому» (как Wayland, в миниатюре):
 *   - ядро НЕ рисует окна. Оно только: доставляет сообщения (mailbox на
 *     процесс), отдаёт сырой ввод тому, кто сделал input grab (это WM),
 *     и шарит память (shm) для пиксельных буферов.
 *   - /vwm — обычный ring3-процесс: композитор + декорации + панель + dock.
 *   - клиент рисует ТОЛЬКО содержимое окна в свой shm-буфер и шлёт COMMIT.
 *     Копирования пикселей через ядро нет вообще: WM видит тот же буфер.
 * ============================================================================= */
#ifndef VOS_ABI_H
#define VOS_ABI_H

#include "syscalls.h"

/* syscalls.h не объявляет int64_t — дообъявляем (хост x86_64: long = 64 бита) */
typedef long int64_t;

/* --- Новые syscall'ы --- */
#define SYS_IPC_SEND      17   /* (dst_pid, msg*)         -> 0 / -1            */
#define SYS_IPC_RECV      18   /* (msg*, timeout_ticks)   -> 1 = есть, 0 = нет */
#define SYS_SHM_CREATE    19   /* (size)                  -> shm_id / -1       */
#define SYS_SHM_MAP       20   /* (shm_id)                -> vaddr / 0         */
#define SYS_INPUT_GRAB    21   /* ()                      -> 0 / -1 (для WM)   */
#define SYS_SVC_REGISTER  22   /* (svc_id)                -> 0 / -1            */
#define SYS_SVC_LOOKUP    23   /* (svc_id)                -> pid / 0           */
#define SYS_SPAWN         24   /* (path*)                 -> pid / -1          */
#define SYS_UPTIME        25   /* ()                      -> тики PIT (100 Hz) */
#define SYS_RTC           26   /* (uint32[3] h,m,s)       -> 0                 */
#define SYS_VSYNC         27   /* ()  ждать vblank (no-op на virtio)           */
#define SYS_FB_PRESENT    28   /* (x,y,w,h)  present для virtio (иначе no-op)  */
#define SYS_SHM_RELEASE   29   /* (shm_id) отпустить ссылку + unmap -> 0 / -1.
                                * Последний держатель освобождает страницы.
                                * После release буфер трогать НЕЛЬЗЯ.          */

#define VOS_SVC_WM        0    /* service id window manager'а */

/* recv: ждать вечно / не ждать */
#define VOS_IPC_FOREVER   ((uint64_t)-1)
#define VOS_IPC_NOWAIT    0

/* --- IPC-сообщение: 8 x uint64. w[7] ядро затирает pid'ом отправителя. --- */
typedef struct { uint64_t w[8]; } vos_msg_t;

/* --- Ввод ядро -> WM (после SYS_INPUT_GRAB) --- */
#define VIN_MOUSE         100  /* w1=dx(int64) w2=dy(int64) w3=buttons w4=btn_changed */
#define VIN_KEY           101  /* w1=ascii w2=pressed */

/* --- Клиент -> WM --- */
#define VWM_CREATE        1    /* w1=(width<<32)|height, w2=shm_id, w3..w6=title (31 байт + 0) */
#define VWM_DESTROY       2    /* w1=win_id */
#define VWM_COMMIT        3    /* w1=win_id, w2=x, w3=y, w4=w, w5=h (damage в коорд. окна) */

/* --- WM -> клиент --- */
#define VWM_CREATED       10   /* w1=win_id (0 = отказ) */
#define VWM_EV_KEY        11   /* w1=win_id, w2=ascii, w3=pressed */
#define VWM_EV_RESIZE     13   /* w1=win_id, w2=new_w, w3=new_h */
#define VWM_EV_CLOSE      14   /* w1=win_id — клиент должен выйти */

/* Максимум пикселей содержимого окна (как MAX_WINDOW_PIXELS в ядре).
 * Буфер окна всегда выделяется под максимум — resize не требует переалокации,
 * меняется только логический stride (= текущая ширина окна). */
#define VWM_MAX_PIXELS    (1024 * 768)
#define VWM_SURFACE_BYTES (VWM_MAX_PIXELS * 4)

/* ------------------------- обёртки syscall'ов ---------------------------- */
static inline uint64_t vos_ipc_send(uint64_t dst_pid, vos_msg_t *m) {
    return syscall6(SYS_IPC_SEND, dst_pid, (uint64_t)m, 0, 0, 0, 0);
}
static inline uint64_t vos_ipc_recv(vos_msg_t *m, uint64_t timeout_ticks) {
    return syscall6(SYS_IPC_RECV, (uint64_t)m, timeout_ticks, 0, 0, 0, 0);
}
static inline uint64_t vos_shm_create(uint64_t size) {
    return syscall1(SYS_SHM_CREATE, size);
}
static inline void *vos_shm_map(uint64_t shm_id) {
    return (void *)syscall1(SYS_SHM_MAP, shm_id);
}
static inline uint64_t vos_shm_release(uint64_t shm_id) {
    return syscall1(SYS_SHM_RELEASE, shm_id);
}
static inline uint64_t vos_input_grab(void) { return syscall0(SYS_INPUT_GRAB); }
static inline uint64_t vos_svc_register(uint64_t svc) { return syscall1(SYS_SVC_REGISTER, svc); }
static inline uint64_t vos_svc_lookup(uint64_t svc)   { return syscall1(SYS_SVC_LOOKUP, svc); }
static inline uint64_t vos_spawn(const char *path)    { return syscall1(SYS_SPAWN, (uint64_t)path); }
static inline uint64_t vos_uptime(void)               { return syscall0(SYS_UPTIME); }
static inline void     vos_rtc(uint32_t hms[3])       { syscall1(SYS_RTC, (uint64_t)hms); }
static inline void     vos_vsync(void)                { syscall0(SYS_VSYNC); }
static inline void     vos_fb_present(int x, int y, int w, int h) {
    syscall6(SYS_FB_PRESENT, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
             (uint64_t)(int64_t)w, (uint64_t)(int64_t)h, 0, 0);
}

/* ------------------------- клиентские хелперы ---------------------------- */

/* Найти pid window manager'а; ждём, пока /vwm зарегистрируется (он стартует
 * параллельно с приложениями). */
static inline uint64_t vwm_wait_for_wm(void) {
    for (;;) {
        uint64_t pid = vos_svc_lookup(VOS_SVC_WM);
        if (pid) return pid;
        syscall1(SYS_SLEEP, 10);   /* 100 мс */
    }
}

/* Создать окно: выделяет shm-поверхность, шлёт CREATE, ждёт CREATED.
 * Возвращает win_id (0 = ошибка); *out_pixels — буфер содержимого окна
 * (stride = текущая ширина окна). */
static inline uint64_t vwm_create_window(uint64_t wm_pid, const char *title,
                                         int w, int h, uint32_t **out_pixels) {
    uint64_t shm_id = vos_shm_create(VWM_SURFACE_BYTES);
    if (shm_id == (uint64_t)-1) return 0;
    uint32_t *pixels = (uint32_t *)vos_shm_map(shm_id);
    if (!pixels) return 0;

    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_CREATE;
    m.w[1] = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    m.w[2] = shm_id;
    /* title: до 31 байта + 0 в w3..w6 */
    {
        char *dst = (char *)&m.w[3];
        int i = 0;
        while (title[i] && i < 31) { dst[i] = title[i]; i++; }
        dst[i] = 0;
    }
    if (vos_ipc_send(wm_pid, &m)) return 0;

    /* Ждём CREATED (другие сообщения до него игнорируем — окно ещё не живо). */
    for (;;) {
        if (!vos_ipc_recv(&m, VOS_IPC_FOREVER)) continue;
        if (m.w[0] == VWM_CREATED) {
            if (!m.w[1]) return 0;
            *out_pixels = pixels;
            return m.w[1];
        }
    }
}

/* Сообщить WM, что прямоугольник содержимого окна перерисован. */
static inline void vwm_commit(uint64_t wm_pid, uint64_t win_id,
                              int x, int y, int w, int h) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_COMMIT;
    m.w[1] = win_id;
    m.w[2] = (uint64_t)(int64_t)x;
    m.w[3] = (uint64_t)(int64_t)y;
    m.w[4] = (uint64_t)(int64_t)w;
    m.w[5] = (uint64_t)(int64_t)h;
    vos_ipc_send(wm_pid, &m);
}

static inline void vwm_destroy(uint64_t wm_pid, uint64_t win_id) {
    vos_msg_t m;
    for (int i = 0; i < 8; i++) m.w[i] = 0;
    m.w[0] = VWM_DESTROY;
    m.w[1] = win_id;
    vos_ipc_send(wm_pid, &m);
}

#endif /* VOS_ABI_H */
