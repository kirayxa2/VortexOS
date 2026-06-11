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
#define SYS_FS_READDIR    30   /* (path*, index, vos_dirent_t*) -> 0 / -1.
                                * Запись каталога №index (для /bin/vfiles).    */
#define SYS_FB_CAPS       40   /* () -> битовая маска возможностей экрана      */
#define VOS_FB_CAP_OFFSCREEN 1 /* present-модель (virtio): экран меняется
                                * ТОЛЬКО по SYS_FB_PRESENT => можно безопасно
                                * компоновать прямо в fb, без back buffer      */

/* --- ФС + процессы для утилит /bin и шелла (пути ТОЛЬКО абсолютные) --- */
#define SYS_FS_READ       31   /* (path*, off, buf*, len) -> прочитано / -1    */
#define SYS_FS_WRITE      32   /* (path*, off, buf*, len) -> записано / -1     */
#define SYS_FS_CREATE     33   /* (path*, is_dir)         -> 0 / -1            */
#define SYS_FS_UNLINK     34   /* (path*) файл или ПУСТОЙ каталог -> 0 / -1    */
#define SYS_FS_STAT       35   /* (path*, vos_stat_t*)    -> 0 / -1            */
#define SYS_SPAWN_EX      36   /* (cmdline*, flags) -> pid / -1. bit0 = пайп:
                                * stdout child'а приходит вызвавшему сообщениями
                                * VOS_MSG_STDOUT, на выходе VOS_MSG_CHILD_EXIT.
                                * Бинарь ищется как в SYS_SPAWN (в т.ч. /bin). */
#define SYS_GETARGS       37   /* (buf*, max) -> len. Командная строка процесса */
#define SYS_CHDIR         38   /* (path*) только каталог  -> 0 / -1            */
#define SYS_GETCWD        39   /* (buf*, max) -> len                           */

#define VOS_SVC_WM        0    /* service id window manager'а */

/* recv: ждать вечно / не ждать */
#define VOS_IPC_FOREVER   ((uint64_t)-1)
#define VOS_IPC_NOWAIT    0

/* --- IPC-сообщение: 8 x uint64. w[7] ядро затирает pid'ом отправителя. --- */
typedef struct { uint64_t w[8]; } vos_msg_t;

/* --- Запись каталога для SYS_FS_READDIR (должна совпадать с ядром) --- */
#define VOS_DT_FILE 0
#define VOS_DT_DIR  1
typedef struct {
    char     name[32];
    uint32_t type;    /* VOS_DT_FILE / VOS_DT_DIR */
    uint32_t size;    /* байты (для каталогов 0) */
} vos_dirent_t;

/* --- SYS_FS_STAT (должна совпадать с sys_stat_t в ядре) --- */
typedef struct {
    uint32_t type;    /* VOS_DT_FILE / VOS_DT_DIR */
    uint32_t size;    /* байты (для каталогов 0) */
} vos_stat_t;

/* --- Сообщения ядро -> шелл (stdout-пайп SYS_SPAWN_EX, см. ipc.h) --- */
#define VOS_MSG_STDOUT     200  /* w1=len (<=40), байты в w2..w6, w7=pid child'а */
#define VOS_MSG_CHILD_EXIT 201  /* w1=exit_code, w7=pid child'а */
#define VOS_STDOUT_CHUNK   40

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
#define VWM_EV_MOUSE      15   /* w1=win_id, w2=x, w3=y, w4=buttons.
                                * Координаты в СОДЕРЖИМОМ окна (без титлбара).
                                * Шлётся на нажатие ЛКМ в содержимое и на
                                * отпускание (buttons=0) тому же окну. */

/* --- Протокол панели (/bin/vpanel <-> vwm) ------------------------------
 * Панель — НЕ окно: отдельная shm-поверхность во всю ширину экрана высотой
 * VWM_PANEL_H, vwm блендит её поверх обоев per-pixel alpha. Без титлбара,
 * фокуса, чипов и z-порядка. */
#define VWM_PANEL_ATTACH   40  /* клиент->wm: w2=shm_id (fbw*VWM_PANEL_H*4) */
#define VWM_PANEL_OK       41  /* wm->клиент: w1=(width<<32)|height; 0 = отказ */
#define VWM_PANEL_COMMIT   42  /* клиент->wm: w1=x w2=y w3=w w4=h (damage) */
#define VWM_PANEL_WINS     43  /* wm->клиент: список окон, по msg на окно:
                                * w1=win_id, w2=(idx<<32)|count,
                                * w3=flags (bit0=minimized, bit1=focused),
                                * w4..w6=title (23 байта + 0).
                                * count=0 -> одно msg с w1=0. */
#define VWM_PANEL_CLICK    44  /* wm->клиент: w1=x, w2=y, w3=buttons */
#define VWM_PANEL_ACTIVATE 45  /* клиент->wm: w1=win_id (restore+focus+raise) */
#define VWM_PANEL_H        28  /* высота панели (= PANEL_H в vwm) */

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
static inline int64_t  vos_fs_readdir(const char *path, uint64_t index, vos_dirent_t *out) {
    return (int64_t)syscall3(SYS_FS_READDIR, (uint64_t)path, index, (uint64_t)out);
}
static inline uint64_t vos_uptime(void)               { return syscall0(SYS_UPTIME); }
static inline void     vos_rtc(uint32_t hms[3])       { syscall1(SYS_RTC, (uint64_t)hms); }
static inline void     vos_vsync(void)                { syscall0(SYS_VSYNC); }
static inline uint64_t vos_fb_caps(void)              { return syscall0(SYS_FB_CAPS); }
static inline void     vos_fb_present(int x, int y, int w, int h) {
    syscall6(SYS_FB_PRESENT, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
             (uint64_t)(int64_t)w, (uint64_t)(int64_t)h, 0, 0);
}

/* --- ФС + процессы (утилиты /bin, шелл) --- */
static inline int64_t vos_fs_read(const char *path, uint64_t off,
                                  void *buf, uint64_t len) {
    return (int64_t)syscall6(SYS_FS_READ, (uint64_t)path, off,
                             (uint64_t)buf, len, 0, 0);
}
static inline int64_t vos_fs_write(const char *path, uint64_t off,
                                   const void *buf, uint64_t len) {
    return (int64_t)syscall6(SYS_FS_WRITE, (uint64_t)path, off,
                             (uint64_t)buf, len, 0, 0);
}
static inline int64_t vos_fs_create(const char *path, int is_dir) {
    return (int64_t)syscall6(SYS_FS_CREATE, (uint64_t)path,
                             (uint64_t)is_dir, 0, 0, 0, 0);
}
static inline int64_t vos_fs_unlink(const char *path) {
    return (int64_t)syscall1(SYS_FS_UNLINK, (uint64_t)path);
}
static inline int64_t vos_fs_stat(const char *path, vos_stat_t *out) {
    return (int64_t)syscall3(SYS_FS_STAT, (uint64_t)path, (uint64_t)out, 0);
}
static inline int64_t vos_spawn_ex(const char *cmdline, uint64_t flags) {
    return (int64_t)syscall3(SYS_SPAWN_EX, (uint64_t)cmdline, flags, 0);
}
static inline int64_t vos_getargs(char *buf, uint64_t max) {
    return (int64_t)syscall3(SYS_GETARGS, (uint64_t)buf, max, 0);
}
static inline int64_t vos_chdir(const char *path) {
    return (int64_t)syscall1(SYS_CHDIR, (uint64_t)path);
}
static inline int64_t vos_getcwd(char *buf, uint64_t max) {
    return (int64_t)syscall3(SYS_GETCWD, (uint64_t)buf, max, 0);
}

/* Развернуть путь относительно cwd + нормализовать "." и "..".
 * out_max >= 2. Возвращает длину результата (всегда абсолютный путь).
 * Общий для шелла (vterm) и утилит /bin — поэтому живёт здесь. */
static inline int vos_abspath(const char *cwd, const char *path,
                              char *out, int out_max) {
    char tmp[512];
    int t = 0;
    if (path[0] == '/') {
        while (path[t] && t < (int)sizeof(tmp) - 1) { tmp[t] = path[t]; t++; }
    } else {
        int i = 0;
        while (cwd[i] && t < (int)sizeof(tmp) - 1) tmp[t++] = cwd[i++];
        if (t == 0 || tmp[t - 1] != '/') {
            if (t < (int)sizeof(tmp) - 1) tmp[t++] = '/';
        }
        i = 0;
        while (path[i] && t < (int)sizeof(tmp) - 1) tmp[t++] = path[i++];
    }
    tmp[t] = 0;

    /* пройти по компонентам, схлопывая "." / ".." / "//" */
    int o = 0;
    out[o++] = '/';
    int i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        char comp[64];
        int c = 0;
        while (tmp[i] && tmp[i] != '/' && c < 63) comp[c++] = tmp[i++];
        while (tmp[i] && tmp[i] != '/') i++;   /* хвост длинного компонента */
        comp[c] = 0;
        if (c == 1 && comp[0] == '.') continue;
        if (c == 2 && comp[0] == '.' && comp[1] == '.') {
            while (o > 1 && out[o - 1] == '/') o--;
            while (o > 1 && out[o - 1] != '/') o--;   /* срезать компонент */
            while (o > 1 && out[o - 1] == '/') o--;   /* и его '/' */
            continue;
        }
        if (o > 1 && o < out_max - 1) out[o++] = '/';
        for (int k = 0; k < c && o < out_max - 1; k++) out[o++] = comp[k];
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return o;
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
