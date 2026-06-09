/* =============================================================================
 * VortexOS — kernel/fs/fat32.c
 * Упрощённый FAT32 драйвер. Читает/пишет через ATA.
 * ============================================================================= */

#include "fat32.h"
#include "vfs.h"
#include "ata.h"
#include "heap.h"
#include "fb.h"

static fat32_volume_t g_vol;
static uint8_t sector_buf[512];

/* Читаем сектор через ATA */
static int read_sector(uint32_t lba, void *buf) {
    return ata_read_sector(0, lba, buf);
}

/* Читаем FAT entry для cluster */
static uint32_t fat_get_next_cluster(uint32_t cluster) __attribute__((unused));
static uint32_t fat_get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_vol.fat_begin_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;
    
    if (read_sector(fat_sector, sector_buf) != 0)
        return FAT32_EOC;
    
    uint32_t *entry = (uint32_t *)(sector_buf + entry_offset);
    return (*entry) & 0x0FFFFFFF;
}

/* Получаем LBA первого сектора cluster */
static uint32_t cluster_to_lba(uint32_t cluster) __attribute__((unused));
static uint32_t cluster_to_lba(uint32_t cluster) {
    return g_vol.cluster_begin_lba + (cluster - 2) * g_vol.sectors_per_cluster;
}

/* Простая утилита — сравнение имён FAT (8.3 формат без учёта регистра) */
static int fat_name_match(const char *name, const uint8_t *fat_name, const uint8_t *fat_ext) {
    char buf[12];
    int i, j = 0;
    
    /* Копируем имя из FAT entry, убираем trailing spaces */
    for (i = 0; i < 8 && fat_name[i] != ' '; i++)
        buf[j++] = fat_name[i];
    
    /* Добавляем точку и расширение если есть */
    if (fat_ext[0] != ' ') {
        buf[j++] = '.';
        for (i = 0; i < 3 && fat_ext[i] != ' '; i++)
            buf[j++] = fat_ext[i];
    }
    buf[j] = 0;
    
    /* Сравниваем без учёта регистра */
    for (i = 0; name[i] && buf[i]; i++) {
        char c1 = name[i], c2 = buf[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return name[i] == 0 && buf[i] == 0;
}

/* Ищем файл в директории (указан cluster директории) */
static fat32_dirent_t* fat_find_in_dir(uint32_t dir_cluster, const char *name) {
    static uint8_t dir_buf[512];
    uint32_t cluster = dir_cluster;
    
    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        
        /* Читаем все секторы cluster */
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, dir_buf) != 0)
                return 0;
            
            fat32_dirent_t *entries = (fat32_dirent_t *)dir_buf;
            for (int i = 0; i < 512 / 32; i++) {
                fat32_dirent_t *e = &entries[i];
                
                /* Конец директории */
                if (e->name[0] == 0x00) return 0;
                
                /* Удалённый entry или LFN — пропускаем */
                if (e->name[0] == 0xE5 || e->attr == FAT32_ATTR_LFN)
                    continue;
                
                /* Сравниваем имя */
                if (fat_name_match(name, e->name, e->ext))
                    return e;
            }
        }
        
        /* Следующий cluster директории */
        cluster = fat_get_next_cluster(cluster);
    }
    
    return 0;
}

int fat32_init(void) {
    /* Читаем boot sector (LBA 0) */
    if (read_sector(0, sector_buf) != 0) {
        fb_puts("[FAT32] Failed to read boot sector\n");
        return -1;
    }
    
    fat32_boot_t *bs = (fat32_boot_t *)sector_buf;
    
    /* Проверяем сигнатуру */
    if (bs->boot_sig != 0xAA55) {
        fb_puts("[FAT32] Invalid boot signature\n");
        return -1;
    }
    
    /* Заполняем volume info */
    g_vol.bytes_per_sector = bs->bytes_per_sector;
    g_vol.sectors_per_cluster = bs->sectors_per_cluster;
    g_vol.root_cluster = bs->root_cluster;
    g_vol.fat_begin_lba = bs->reserved_sectors;
    g_vol.cluster_begin_lba = bs->reserved_sectors + (bs->num_fats * bs->sectors_per_fat_32);
    
    fb_puts("[FAT32] Mounted: root_cluster=");
    fb_puts("0x");
    /* TODO: print hex */
    fb_putchar('\n');
    
    return 0;
}

void fat32_mount(void) {
    /* Создаём VFS root node для FAT32 */
    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!root) {
        fb_puts("[FAT32] Failed to allocate root node\n");
        return;
    }
    
    vfs_ops_t *ops = (vfs_ops_t *)kmalloc(sizeof(vfs_ops_t));
    if (!ops) {
        kfree(root);
        fb_puts("[FAT32] Failed to allocate ops\n");
        return;
    }
    
    /* Заполняем операции */
    ops->open    = (int (*)(struct vfs_node *, uint32_t))fat32_open_vfs;
    ops->close   = (void (*)(struct vfs_node *))fat32_close_vfs;
    ops->read    = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, uint8_t *))fat32_read_vfs;
    ops->write   = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, const uint8_t *))fat32_write_vfs;
    ops->finddir = (struct vfs_node *(*)(struct vfs_node *, const char *))fat32_finddir_vfs;
    ops->mkdir   = (int (*)(struct vfs_node *, const char *))fat32_mkdir_vfs;
    ops->create  = (int (*)(struct vfs_node *, const char *))fat32_create_vfs;
    ops->unlink  = 0;
    ops->readdir = (const char *(*)(struct vfs_node *, uint32_t))fat32_readdir_vfs;
    
    /* Заполняем root node */
    root->inode   = g_vol.root_cluster;
    root->size    = 0;
    root->type    = VFS_DIR;
    root->flags   = 0;
    root->name[0] = '/';
    root->name[1] = 0;
    root->ops     = ops;
    root->fs_data = 0;
    root->mount   = 0;
    
    /* Монтируем как корень VFS */
    vfs_mount_root(root);
    fb_puts("[FAT32] Mounted as VFS root\n");
}

/* ============================================================================
 * VFS Callbacks
 * ============================================================================ */

int32_t fat32_read_vfs(void *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    if (!vnode || vnode->type != VFS_FILE) return -1;
    
    uint32_t cluster = vnode->inode;
    uint32_t file_size = vnode->size;
    
    /* Ограничиваем чтение размером файла */
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;
    
    uint32_t bytes_read = 0;
    uint32_t cluster_size = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;
    static uint8_t cluster_buf[4096];
    
    /* Пропускаем offset кластеров */
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t offset_in_cluster = offset % cluster_size;
    
    for (uint32_t i = 0; i < skip_clusters && cluster < FAT32_EOC; i++)
        cluster = fat_get_next_cluster(cluster);
    
    /* Читаем данные */
    while (size > 0 && cluster < FAT32_EOC) {
        /* Читаем весь cluster */
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, cluster_buf + (s * 512)) != 0)
                return bytes_read;
        }
        
        /* Копируем нужные байты */
        uint32_t to_copy = cluster_size - offset_in_cluster;
        if (to_copy > size) to_copy = size;
        
        for (uint32_t i = 0; i < to_copy; i++)
            buf[bytes_read++] = cluster_buf[offset_in_cluster + i];
        
        size -= to_copy;
        offset_in_cluster = 0;
        
        cluster = fat_get_next_cluster(cluster);
    }
    
    return bytes_read;
}


int32_t fat32_write_vfs(void *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return 0;
}

void* fat32_open_vfs(void *node, uint32_t flags) {
    (void)node; (void)flags;
    return node;
}

void fat32_close_vfs(void *node) {
    (void)node;
}

void* fat32_readdir_vfs(void *node, uint32_t index) {
    (void)node; (void)index;
    /* TODO: листинг директории */
    return 0;
}

/* Ищем файл/директорию по имени в текущей ноде (директории) */
void* fat32_finddir_vfs(void *node, const char *name) {
    (void)node; /* пока ищем только в root */
    
    fat32_dirent_t *e = fat_find_in_dir(g_vol.root_cluster, name);
    if (!e) return 0;
    
    /* Создаём vfs_node для найденного файла/директории */
    vfs_node_t *vnode = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!vnode) return 0;
    
    vfs_ops_t *ops = (vfs_ops_t *)kmalloc(sizeof(vfs_ops_t));
    if (!ops) {
        kfree(vnode);
        return 0;
    }
    
    /* Копируем операции */
    ops->open    = (int (*)(struct vfs_node *, uint32_t))fat32_open_vfs;
    ops->close   = (void (*)(struct vfs_node *))fat32_close_vfs;
    ops->read    = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, uint8_t *))fat32_read_vfs;
    ops->write   = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, const uint8_t *))fat32_write_vfs;
    ops->finddir = (struct vfs_node *(*)(struct vfs_node *, const char *))fat32_finddir_vfs;
    ops->mkdir   = (int (*)(struct vfs_node *, const char *))fat32_mkdir_vfs;
    ops->create  = (int (*)(struct vfs_node *, const char *))fat32_create_vfs;
    ops->unlink  = 0;
    ops->readdir = (const char *(*)(struct vfs_node *, uint32_t))fat32_readdir_vfs;
    
    /* Заполняем поля */
    uint32_t cluster = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
    vnode->inode = cluster;
    vnode->size  = e->size;
    vnode->type  = (e->attr & FAT32_ATTR_DIR) ? VFS_DIR : VFS_FILE;
    vnode->flags = 0;
    vnode->ops   = ops;
    vnode->fs_data = 0;
    vnode->mount = 0;
    
    /* Копируем имя */
    int i, j = 0;
    for (i = 0; i < 8 && e->name[i] != ' '; i++)
        vnode->name[j++] = e->name[i];
    if (e->ext[0] != ' ') {
        vnode->name[j++] = '.';
        for (i = 0; i < 3 && e->ext[i] != ' '; i++)
            vnode->name[j++] = e->ext[i];
    }
    vnode->name[j] = 0;
    
    return vnode;
}

int fat32_mkdir_vfs(void *node, const char *name) {
    (void)node; (void)name;
    return -1;
}

int fat32_create_vfs(void *node, const char *name) {
    (void)node; (void)name;
    return -1;
}
