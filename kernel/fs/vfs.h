#ifndef VOS_VFS_H
#define VOS_VFS_H

#include "types.h"

/* Типы нод */
#define VFS_FILE    0x01
#define VFS_DIR     0x02
#define VFS_SYMLINK 0x03
#define VFS_MOUNT   0x04

#define VFS_MAX_NAME  128
#define VFS_MAX_PATH  512
#define VFS_MAX_FDS   64
#define VFS_MAX_MOUNTS 16

/* Флаги vfs_node_t.flags */
#define VFS_FL_CACHED 0x1   /* нода живёт в кэше своей ФС — НЕ kfree'ить
                             * (vortexfs кэширует ноды; см. fs_node_put) */

/* Маски прав для vfs_access (классический rwx) */
#define VFS_PERM_R 4
#define VFS_PERM_W 2
#define VFS_PERM_X 1

struct vfs_node;

/* Таблица операций — каждая ФС заполняет свою */
typedef struct vfs_ops {
    int      (*open)   (struct vfs_node *node, uint32_t flags);
    void     (*close)  (struct vfs_node *node);
    int32_t  (*read)   (struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buf);
    int32_t  (*write)  (struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buf);
    struct vfs_node *(*finddir)(struct vfs_node *node, const char *name);
    int      (*mkdir)  (struct vfs_node *node, const char *name);
    int      (*create) (struct vfs_node *node, const char *name);
    int      (*unlink) (struct vfs_node *node, const char *name);
    /* Итерация директории: index = 0,1,2... возвращает имя или NULL */
    const char *(*readdir)(struct vfs_node *node, uint32_t index);
    /* Права (опционально; ФС без прав оставляют NULL) */
    int      (*chmod)  (struct vfs_node *node, uint32_t mode);
    int      (*chown)  (struct vfs_node *node, uint32_t uid, uint32_t gid);
} vfs_ops_t;

/* VFS нода — универсальный объект (файл, директория, ...) */
typedef struct vfs_node {
    char        name[VFS_MAX_NAME];
    uint32_t    type;       /* VFS_FILE / VFS_DIR / ... */
    uint32_t    size;       /* размер в байтах */
    uint32_t    inode;      /* номер inode в конкретной ФС */
    uint32_t    flags;
    uint32_t    mode;       /* права rwxrwxrwx (0 = ФС без прав → 0755) */
    uint32_t    uid;        /* владелец */
    uint32_t    gid;        /* группа   */
    vfs_ops_t  *ops;        /* операции */
    void       *fs_data;    /* указатель на данные конкретной ФС */
    struct vfs_node *mount; /* если VFS_MOUNT — точка монтирования */
} vfs_node_t;

/* Дескриптор открытого файла */
typedef struct {
    vfs_node_t *node;
    uint32_t    offset;
    uint8_t     used;
} vfs_fd_t;

/* --- API VFS --- */

void         vfs_init(void);
void         vfs_mount_root(vfs_node_t *root);
int          vfs_mount(const char *path, vfs_node_t *root);

vfs_node_t  *vfs_open(const char *path, uint32_t flags);
void         vfs_close(vfs_node_t *node);
int32_t      vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buf);
int32_t      vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buf);
vfs_node_t  *vfs_finddir(vfs_node_t *node, const char *name);
int          vfs_mkdir(const char *path);
int          vfs_create(const char *path);
int          vfs_unlink(const char *path);
const char  *vfs_readdir(vfs_node_t *node, uint32_t index);

/* --- Права --- */

/* Креды текущей задачи (root=0 до старта планировщика и для kernel-тасков) */
uint32_t     vfs_cur_uid(void);
uint32_t     vfs_cur_gid(void);
/* Проверка доступа: mask = VFS_PERM_R|W|X. 0 = ок, -1 = отказ.
 * uid 0 (root) проходит всё, кроме X на файлах вообще без x-битов. */
int          vfs_access(vfs_node_t *node, uint32_t mask);
int          vfs_chmod(const char *path, uint32_t mode);  /* владелец/root */
int          vfs_chown(const char *path, uint32_t uid, uint32_t gid); /* root */

/* Глобальный корень */
extern vfs_node_t *vfs_root;

#endif
