#ifndef VOS_RAMFS_H
#define VOS_RAMFS_H

#include "vfs.h"

/* Инициализирует ramfs и возвращает корневую ноду */
vfs_node_t *ramfs_create_root(void);

#endif
