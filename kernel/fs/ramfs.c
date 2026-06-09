/* =============================================================================
 * VortexOS — kernel/fs/ramfs.c
 * RAM-based файловая система. Всё хранится в куче ядра (kmalloc).
 * Используется как корневая ФС до монтирования VortexFS с диска.
 *
 * Структура:
 *   ramfs_node_t — внутренний узел (файл или директория)
 *   Директория хранит массив дочерних узлов (children[])
 *   Файл хранит данные в data[] (динамически расширяется через kmalloc)
 * ============================================================================= */

#include "ramfs.h"
#include "vfs.h"
#include "heap.h"
#include "fb.h"

#define RAMFS_MAX_CHILDREN 64
#define RAMFS_MAX_FILESIZE (4 * 1024 * 1024) /* 4 МБ */

/* Внутренний узел ramfs */
typedef struct ramfs_node {
    char               name[VFS_MAX_NAME];
    uint32_t           type;    /* VFS_FILE / VFS_DIR */
    uint32_t           size;    /* для файлов — размер данных */
    uint8_t           *data;    /* для файлов — содержимое */
    uint32_t           data_cap;/* выделенная ёмкость */

    /* Дочерние узлы (только для директорий) */
    struct ramfs_node *children[RAMFS_MAX_CHILDREN];
    uint32_t           child_count;

    /* VFS нода, связанная с этим узлом */
    vfs_node_t         vnode;
} ramfs_node_t;

/* --- Вспомогательные функции -------------------------------------------- */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Создаём новый внутренний узел */
static ramfs_node_t *ramfs_alloc_node(const char *name, uint32_t type) {
    ramfs_node_t *n = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (!n) return 0;

    /* Обнуляем вручную */
    uint8_t *p = (uint8_t *)n;
    for (uint32_t i = 0; i < sizeof(ramfs_node_t); i++) p[i] = 0;

    str_copy(n->name, name, VFS_MAX_NAME);
    n->type = type;
    return n;
}

/* --- Операции VFS -------------------------------------------------------- */

static vfs_ops_t ramfs_ops; /* объявлена ниже, инициализируется в ramfs_create_root */

/* Получаем ramfs_node_t из vfs_node_t через fs_data */
static ramfs_node_t *to_ramfs(vfs_node_t *vnode) {
    return (ramfs_node_t *)vnode->fs_data;
}

static int ramfs_open(vfs_node_t *node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;
}

static void ramfs_close(vfs_node_t *node) {
    (void)node;
}

static int32_t ramfs_read(vfs_node_t *vnode, uint32_t offset, uint32_t size, uint8_t *buf) {
    ramfs_node_t *n = to_ramfs(vnode);
    if (!n || n->type != VFS_FILE) return -1;
    if (offset >= n->size) return 0;
    if (offset + size > n->size) size = n->size - offset;
    for (uint32_t i = 0; i < size; i++) buf[i] = n->data[offset + i];
    return (int32_t)size;
}

static int32_t ramfs_write(vfs_node_t *vnode, uint32_t offset, uint32_t size, const uint8_t *buf) {
    ramfs_node_t *n = to_ramfs(vnode);
    if (!n || n->type != VFS_FILE) return -1;

    uint32_t end = offset + size;
    if (end > RAMFS_MAX_FILESIZE) return -1;

    /* Расширяем буфер если нужно */
    if (end > n->data_cap) {
        uint32_t new_cap = end + 512; /* с запасом */
        uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
        if (!new_data) return -1;
        for (uint32_t i = 0; i < n->data_cap; i++) new_data[i] = n->data ? n->data[i] : 0;
        for (uint32_t i = n->data_cap; i < new_cap; i++) new_data[i] = 0;
        if (n->data) kfree(n->data);
        n->data = new_data;
        n->data_cap = new_cap;
    }

    for (uint32_t i = 0; i < size; i++) n->data[offset + i] = buf[i];
    if (end > n->size) n->size = end;
    vnode->size = n->size;
    return (int32_t)size;
}

static vfs_node_t *ramfs_finddir(vfs_node_t *vnode, const char *name) {
    ramfs_node_t *n = to_ramfs(vnode);
    if (!n || n->type != VFS_DIR) return 0;
    for (uint32_t i = 0; i < n->child_count; i++) {
        if (str_eq(n->children[i]->name, name))
            return &n->children[i]->vnode;
    }
    return 0;
}

static const char *ramfs_readdir(vfs_node_t *vnode, uint32_t index) {
    ramfs_node_t *n = to_ramfs(vnode);
    if (!n || n->type != VFS_DIR) return 0;
    if (index >= n->child_count) return 0;
    return n->children[index]->name;
}

static int ramfs_mkdir(vfs_node_t *vnode, const char *name) {
    ramfs_node_t *parent = to_ramfs(vnode);
    if (!parent) { fb_puts("[ramfs] mkdir: parent null\n"); return -1; }
    if (parent->type != VFS_DIR) { fb_puts("[ramfs] mkdir: not dir\n"); return -1; }
    if (parent->child_count >= RAMFS_MAX_CHILDREN) return -1;

    /* Проверяем что нет дубликата */
    for (uint32_t i = 0; i < parent->child_count; i++)
        if (str_eq(parent->children[i]->name, name)) return -1;

    ramfs_node_t *child = ramfs_alloc_node(name, VFS_DIR);
    if (!child) return -1;

    child->vnode.type   = VFS_DIR;
    child->vnode.ops    = &ramfs_ops;
    child->vnode.fs_data = child;
    str_copy(child->vnode.name, name, VFS_MAX_NAME);

    parent->children[parent->child_count++] = child;
    return 0;
}

static int ramfs_create(vfs_node_t *vnode, const char *name) {
    ramfs_node_t *parent = to_ramfs(vnode);
    if (!parent || parent->type != VFS_DIR) return -1;
    if (parent->child_count >= RAMFS_MAX_CHILDREN) return -1;

    for (uint32_t i = 0; i < parent->child_count; i++)
        if (str_eq(parent->children[i]->name, name)) return -1;

    ramfs_node_t *child = ramfs_alloc_node(name, VFS_FILE);
    if (!child) return -1;

    child->vnode.type    = VFS_FILE;
    child->vnode.ops     = &ramfs_ops;
    child->vnode.fs_data = child;
    str_copy(child->vnode.name, name, VFS_MAX_NAME);

    parent->children[parent->child_count++] = child;
    return 0;
}

static int ramfs_unlink(vfs_node_t *vnode, const char *name) {
    ramfs_node_t *parent = to_ramfs(vnode);
    if (!parent || parent->type != VFS_DIR) return -1;

    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (str_eq(parent->children[i]->name, name)) {
            ramfs_node_t *target = parent->children[i];
            if (target->data) kfree(target->data);
            kfree(target);
            /* Сдвигаем массив */
            for (uint32_t j = i; j < parent->child_count - 1; j++)
                parent->children[j] = parent->children[j + 1];
            parent->child_count--;
            return 0;
        }
    }
    return -1; /* не найдено */
}

/* Таблица операций */
static vfs_ops_t ramfs_ops = {
    .open    = ramfs_open,
    .close   = ramfs_close,
    .read    = ramfs_read,
    .write   = ramfs_write,
    .finddir = ramfs_finddir,
    .readdir = ramfs_readdir,
    .mkdir   = ramfs_mkdir,
    .create  = ramfs_create,
    .unlink  = ramfs_unlink,
};

/* --- Публичный API ------------------------------------------------------- */

vfs_node_t *ramfs_create_root(void) {
    ramfs_node_t *root = ramfs_alloc_node("/", VFS_DIR);
    if (!root) return 0;

    root->vnode.type    = VFS_DIR;
    root->vnode.ops     = &ramfs_ops;
    root->vnode.fs_data = root;
    root->vnode.name[0] = '/';
    root->vnode.name[1] = 0;

    return &root->vnode;
}
