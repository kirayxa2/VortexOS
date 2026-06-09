#ifndef VOS_VIRTIO_GPU_H
#define VOS_VIRTIO_GPU_H

#include "types.h"

/* =============================================================================
 * VortexOS — virtio-gpu (2D) драйвер.
 *
 * Идея: даём «настоящий» аппаратный present. Вместо того чтобы копировать back
 * buffer прямо в линейный scanout-буфер Limine (что тирит, потому что луч
 * развёртки QEMU читает буфер в произвольный момент), мы:
 *   1) создаём в GPU 2D-ресурс размера экрана (RESOURCE_CREATE_2D);
 *   2) привязываем к нему наш framebuffer как backing (RESOURCE_ATTACH_BACKING);
 *   3) делаем его scanout 0 (SET_SCANOUT);
 *   4) на каждый кадр пушим только damage-прямоугольник: TRANSFER_TO_HOST_2D +
 *      RESOURCE_FLUSH. RESOURCE_FLUSH — это и есть аппаратный present: QEMU
 *      атомарно показывает кадр, без разрывов.
 *
 * БЕЗОПАСНОСТЬ: virtio_gpu_init() работает по принципу «всё или ничего». Любой
 * сбой (нет устройства / не та ревизия / таймаут очереди) → возвращает 0 и
 * НИЧЕГО не ломает: kmain просто продолжает на Limine-framebuffer как раньше.
 * Все циклы опроса — с таймаутом, MMIO мапится явно. Чёрного экрана быть не
 * должно: если GPU не поднялся, scanout остаётся прежним.
 * ============================================================================= */

/* Поднять virtio-gpu. Возвращает 1 при успехе (scanout переключён на наш
 * ресурс), 0 — если устройства нет или инициализация не удалась. */
int virtio_gpu_init(void);

/* Активен ли драйвер (инициализация прошла успешно). */
int virtio_gpu_active(void);

/* Линейный (contiguous в виртуальной памяти) framebuffer-буфер, в который надо
 * писать пиксели ARGB. pitch = width*4. 0 если драйвер не активен. */
uint32_t *virtio_gpu_framebuffer(void);
uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
uint32_t  virtio_gpu_pitch(void);   /* в байтах */

/* Запушить прямоугольник из backing в GPU и показать (present).
 * Координаты клипуются по размеру экрана. Без-ничего если драйвер не активен. */
void virtio_gpu_flush(int x, int y, int w, int h);

#endif /* VOS_VIRTIO_GPU_H */
