/* =============================================================================
 * VortexOS — kernel/fs/vfs.c
 * VFS слой абстракции. Маршрутизирует вызовы к конкретной ФС.
 * ============================================================================= */

#include "vfs.h"
#include "fb.h"

/* Глобальный корень файловой системы */
vfs_node_t *vfs_root = 0;

/* Таблица точек монтирования */
typedef struct {
    char        path[VFS_MAX_PATH];
    vfs_node_t *root;
    uint8_t     used;
} mount_point_t;

static mount_point_t mounts[VFS_MAX_MOUNTS];

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        mounts[i].used = 0;
    vfs_root = 0;
}

void vfs_mount_root(vfs_node_t *root) {
    vfs_root = root;
}

int vfs_mount(const char *path, vfs_node_t *root) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            /* копируем строку вручную */
            int j = 0;
            while (path[j] && j < VFS_MAX_PATH - 1) {
                mounts[i].path[j] = path[j]; j++;
            }
            mounts[i].path[j] = 0;
            mounts[i].root = root;
            mounts[i].used = 1;
            return 0;
        }
    }
    return -1; /* нет места */
}

/* Разбиваем путь и ищем ноду начиная от корня */
vfs_node_t *vfs_open(const char *path, uint32_t flags) {
    if (!vfs_root || !path || path[0] != '/') return 0;

    /* Корень */
    if (path[1] == 0) return vfs_root;

    vfs_node_t *cur = vfs_root;
    /* Работаем с копией пути */
    char buf[VFS_MAX_PATH];
    int i = 0;
    while (path[i] && i < VFS_MAX_PATH - 1) { buf[i] = path[i]; i++; }
    buf[i] = 0;

    /* Идём по компонентам: /a/b/c → ["a","b","c"] */
    char *p = buf + 1; /* пропускаем первый '/' */
    while (p && *p) {
        /* Находим следующий '/' */
        char *slash = p;
        while (*slash && *slash != '/') slash++;
        uint8_t more = (*slash == '/');
        *slash = 0;

        if (!cur->ops || !cur->ops->finddir) return 0;
        vfs_node_t *next = cur->ops->finddir(cur, p);
        if (!next) return 0;

        cur = next;
        p = more ? slash + 1 : 0;
    }

    /* Вызываем open если есть */
    if (cur->ops && cur->ops->open)
        cur->ops->open(cur, flags);

    return cur;
}

void vfs_close(vfs_node_t *node) {
    if (!node) return;
    if (node->ops && node->ops->close)
        node->ops->close(node);
}

int32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, size, buf);
}

int32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, size, buf);
}

vfs_node_t *vfs_finddir(vfs_node_t *node, const char *name) {
    if (!node || !node->ops || !node->ops->finddir) return 0;
    return node->ops->finddir(node, name);
}

const char *vfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !node->ops || !node->ops->readdir) return 0;
    return node->ops->readdir(node, index);
}

int vfs_mkdir(const char *path) {
    if (!path || path[0] != '/') return -1;

    char buf[VFS_MAX_PATH];
    char name[VFS_MAX_NAME];
    int i = 0;
    while (path[i] && i < VFS_MAX_PATH - 1) { buf[i] = path[i]; i++; }
    buf[i] = 0;

    /* Находим последний '/' */
    int last = i - 1;
    while (last > 0 && buf[last] != '/') last--;

    /* Копируем имя до обрезания */
    int j = 0;
    for (int k = last + 1; buf[k] && j < VFS_MAX_NAME - 1; k++) name[j++] = buf[k];
    name[j] = 0;
    if (j == 0) return -1;

    /* Обрезаем родительский путь */
    if (last == 0) buf[1] = 0; else buf[last] = 0;

    vfs_node_t *parent = vfs_open(buf, 0);
    if (!parent || !parent->ops || !parent->ops->mkdir) return -1;
    return parent->ops->mkdir(parent, name);
}

int vfs_create(const char *path) {
    if (!path || path[0] != '/') return -1;

    char buf[VFS_MAX_PATH];
    char name[VFS_MAX_NAME];
    int i = 0;
    while (path[i] && i < VFS_MAX_PATH - 1) { buf[i] = path[i]; i++; }
    buf[i] = 0;

    int last = i - 1;
    while (last > 0 && buf[last] != '/') last--;

    int j = 0;
    for (int k = last + 1; buf[k] && j < VFS_MAX_NAME - 1; k++) name[j++] = buf[k];
    name[j] = 0;
    if (j == 0) return -1;

    if (last == 0) buf[1] = 0; else buf[last] = 0;

    vfs_node_t *parent = vfs_open(buf, 0);
    if (!parent || !parent->ops || !parent->ops->create) return -1;
    return parent->ops->create(parent, name);
}

int vfs_unlink(const char *path) {
    if (!path || path[0] != '/') return -1;

    char buf[VFS_MAX_PATH];
    int i = 0;
    while (path[i] && i < VFS_MAX_PATH - 1) { buf[i] = path[i]; i++; }
    buf[i] = 0;

    int last = i - 1;
    while (last > 0 && buf[last] != '/') last--;

    char *name = buf + last + 1;
    if (last == 0) buf[1] = 0; else buf[last] = 0;

    vfs_node_t *parent = vfs_open(last == 0 ? "/" : buf, 0);
    if (!parent || !parent->ops || !parent->ops->unlink) return -1;
    return parent->ops->unlink(parent, name);
}
