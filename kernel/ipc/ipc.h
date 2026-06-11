#ifndef VOS_IPC_H
#define VOS_IPC_H

/* =============================================================================
 * VortexOS — kernel/ipc/ipc.h
 * Микроядерные примитивы для userspace window manager'а («по-взрослому»):
 *
 *   1) IPC-сообщения: у каждого процесса есть mailbox (очередь сообщений по
 *      64 байта = 8 x uint64). sys_ipc_send кладёт сообщение в mailbox
 *      получателя, sys_ipc_recv блокирующе ждёт (block, don't poll).
 *      Ядро само вписывает pid отправителя в слово [7] — подделать нельзя.
 *
 *   2) Service registry: процесс может зарегистрировать себя как сервис
 *      (SVC_WM = 0), другие находят его pid через lookup. Так клиенты находят
 *      window manager без хардкода pid.
 *
 *   3) Input grab: userspace WM забирает себе ВЕСЬ ввод. После grab'а IRQ
 *      клавиатуры/мыши шлют сырые события сообщениями в mailbox WM, а
 *      встроенный kernel-WM (simple_wm) ввод больше не получает.
 *
 *   4) Shared memory: окна передают пиксели без копирования через ядро.
 *      shm-сегмент маппится во все процессы на ОДИН и тот же user-адрес
 *      (SHM_USER_BASE + id*slot), поэтому указатели валидны на обеих сторонах.
 * ============================================================================= */

#include "types.h"

#define IPC_MSG_WORDS   8          /* сообщение = 8 x uint64 = 64 байта */
#define IPC_QUEUE_LEN   64         /* глубина mailbox'а (сообщений)     */

/* Сервисы (service registry) */
#define IPC_SVC_WM      0
#define IPC_SVC_MAX     4

/* Типы сообщений ввода ядро -> input grabber (userspace WM) */
#define IPC_MSG_INPUT_MOUSE  100   /* w1=dx(int64) w2=dy(int64) w3=buttons w4=btn_changed */
#define IPC_MSG_INPUT_KEY    101   /* w1=ascii w2=pressed */

/* Shared memory: фиксированная схема адресов.
 * Каждому сегменту id выделяется слот 16MB:
 *   - в ядре:      SHM_KERNEL_BASE + id*SHM_SLOT_SIZE
 *   - в userspace: SHM_USER_BASE   + id*SHM_SLOT_SIZE (одинаково во всех процессах)
 * 16MB хватает на back buffer 1920x1080x4 (~8MB) с запасом. */
#define SHM_MAX_SEGS    32   /* 1 на back buffer vwm + по 1 на окно (+ запас) */
#define SHM_REF_MAX     8    /* держателей на сегмент (создатель + map'нувшие) */
#define SHM_SLOT_SIZE   (16ULL * 1024 * 1024)
#define SHM_KERNEL_BASE 0xFFFFFFFF94000000ULL
#define SHM_USER_BASE   0xA0000000ULL

void     ipc_init(void);

/* --- syscall-обработчики (зовутся из syscall.c, контекст задачи) --- */
uint64_t ipc_sys_send(uint64_t dst_pid, uint64_t user_msg);
uint64_t ipc_sys_recv(uint64_t user_msg, uint64_t timeout_ticks);
uint64_t ipc_sys_svc_register(uint64_t svc_id);
uint64_t ipc_sys_svc_lookup(uint64_t svc_id);
uint64_t ipc_sys_input_grab(void);
uint64_t ipc_sys_shm_create(uint64_t size);
uint64_t ipc_sys_shm_map(uint64_t shm_id);
uint64_t ipc_sys_shm_release(uint64_t shm_id);

/* Отправка из ядра от имени sender_pid (stdout-пайп шелла, child-exit).
 * Сообщение в kernel-памяти; IF сохраняется. 0 = доставлено. */
uint64_t ipc_kernel_send(uint32_t dst_pid, const uint64_t *msg8, uint32_t sender_pid);

/* Типы сообщений ядро -> шелл (vsh/vterm): stdout-пайп и завершение child'а.
 * Должны совпадать с VOS_MSG_* в userspace/vos_abi.h. */
#define IPC_MSG_STDOUT      200  /* w1=len(<=40), байты в w2..w6 */
#define IPC_MSG_CHILD_EXIT  201  /* w1=exit_code */

/* --- хуки для драйверов (зовутся ИЗ IRQ, прерывания уже выключены) --- */
int  ipc_input_grabbed(void);                       /* 1 = ввод забрал userspace WM */
void ipc_input_push_key(char ascii, int pressed);
void ipc_input_push_mouse(int dx, int dy, uint8_t buttons, int btn_changed);

/* Зовётся из PIT IRQ0: будит ipc_sys_recv, у которых истёк timeout. */
void ipc_tick(void);

/* SYS_KILL: разбудить задачу, спящую в ipc_sys_recv (см. pending_kill). */
void ipc_force_wake(uint32_t pid);

/* Зовётся из task_exit (под cli): освобождает mailbox/сервисы/grab задачи. */
void ipc_on_task_exit(uint32_t pid);

#endif /* VOS_IPC_H */
