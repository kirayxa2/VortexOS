/* =============================================================================
 * VortexOS — kernel/fs/fat32.c
 * Упрощённый FAT32 драйвер: чтение И запись через ATA.
 *
 * Реализовано:
 *   - mount как корень VFS
 *   - read  (чтение файла по цепочке кластеров)
 *   - write (запись + рост файла, выделение кластеров, обновление dirent)
 *   - finddir (поиск в ЛЮБОЙ директории, не только в root)
 *   - readdir (листинг директории — для `ls`)
 *   - create  (создание пустого файла)
 *   - mkdir   (создание директории c записями `.` и `..`)
 *   - unlink  (удаление файла/пустой директории + освобождение цепочки FAT)
 * ============================================================================= */

#include "fat32.h"
#include "vfs.h"
#include "ata.h"
#include "heap.h"
#include "fb.h"

static fat32_volume_t g_vol;
static uint8_t sector_buf[512];

/* Приватные данные FAT32-ноды: где лежит её dirent (чтобы обновлять на диске) */
typedef struct {
    uint32_t parent_cluster;  /* кластер директории-родителя             */
    uint32_t dirent_lba;      /* LBA сектора, где лежит 32-байтный dirent */
    uint32_t dirent_idx;      /* индекс dirent внутри сектора (0..15)     */
} fat32_priv_t;

static vfs_ops_t fat32_ops; /* единая таблица операций, заполняется в mount */

/* --- Низкоуровневый доступ к секторам ----------------------------------- */

static int read_sector(uint32_t lba, void *buf) {
    return ata_read_sector(0, lba, buf);
}

static int write_sector(uint32_t lba, const void *buf) {
    return ata_write(0, lba, 1, (const uint16_t *)buf);
}

/* --- FAT таблица --------------------------------------------------------- */

/* Получаем следующий кластер в цепочке */
static uint32_t fat_get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset   = cluster * 4;
    uint32_t fat_sector   = g_vol.fat_begin_lba + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    if (read_sector(fat_sector, sector_buf) != 0)
        return FAT32_EOC;

    uint32_t *entry = (uint32_t *)(sector_buf + entry_offset);
    return (*entry) & 0x0FFFFFFF;
}

/* Записываем значение в FAT-entry (во ВСЕ копии FAT) */
static int fat_set_next_cluster(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset   = cluster * 4;
    uint32_t entry_offset = fat_offset % 512;
    uint8_t  buf[512];

    for (uint32_t f = 0; f < g_vol.num_fats; f++) {
        uint32_t fat_sector = g_vol.fat_begin_lba
                            + f * g_vol.sectors_per_fat
                            + (fat_offset / 512);
        if (read_sector(fat_sector, buf) != 0) return -1;
        uint32_t *entry = (uint32_t *)(buf + entry_offset);
        /* Сохраняем верхние 4 бита (зарезервированы) */
        *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
        if (write_sector(fat_sector, buf) != 0) return -1;
    }
    return 0;
}

/* Ищем свободный кластер, помечаем как EOC, обнуляем его данные */
static uint32_t fat_alloc_cluster(void) {
    for (uint32_t c = 2; c < g_vol.total_clusters + 2; c++) {
        if (fat_get_next_cluster(c) == FAT32_FREE) {
            if (fat_set_next_cluster(c, FAT32_EOC) != 0) return 0;
            /* Обнуляем сектора кластера */
            uint8_t zero[512];
            for (int i = 0; i < 512; i++) zero[i] = 0;
            uint32_t lba = g_vol.cluster_begin_lba + (c - 2) * g_vol.sectors_per_cluster;
            for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++)
                write_sector(lba + s, zero);
            return c;
        }
    }
    return 0; /* диск полон */
}

/* Освобождаем всю цепочку кластеров начиная с first */
static void fat_free_chain(uint32_t first) {
    uint32_t c = first;
    while (c >= 2 && c < FAT32_EOC) {
        uint32_t next = fat_get_next_cluster(c);
        fat_set_next_cluster(c, FAT32_FREE);
        c = next;
    }
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return g_vol.cluster_begin_lba + (cluster - 2) * g_vol.sectors_per_cluster;
}

/* --- Имена 8.3 ----------------------------------------------------------- */

/* Сравнение имени с записью FAT (8.3, без регистра) */
static int fat_name_match(const char *name, const uint8_t *fat_name, const uint8_t *fat_ext) {
    char buf[13];
    int i, j = 0;
    for (i = 0; i < 8 && fat_name[i] != ' '; i++) buf[j++] = fat_name[i];
    if (fat_ext[0] != ' ') {
        buf[j++] = '.';
        for (i = 0; i < 3 && fat_ext[i] != ' '; i++) buf[j++] = fat_ext[i];
    }
    buf[j] = 0;

    for (i = 0; name[i] && buf[i]; i++) {
        char c1 = name[i], c2 = buf[i];
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return 0;
    }
    return name[i] == 0 && buf[i] == 0;
}

/* Преобразуем "name.ext" → 11-байтный 8.3 формат (uppercase, space-padded) */
static void fat_make_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    /* base */
    while (name[i] && name[i] != '.' && o < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[o++] = (uint8_t)c;
        i++;
    }
    /* пропускаем до точки */
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        o = 8;
        while (name[i] && o < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[o++] = (uint8_t)c;
            i++;
        }
    }
}

/* Извлекаем человекочитаемое имя из dirent в out (с точкой) */
static void fat_extract_name(const fat32_dirent_t *e, char *out) {
    int i, j = 0;
    for (i = 0; i < 8 && e->name[i] != ' '; i++) out[j++] = e->name[i];
    if (e->ext[0] != ' ') {
        out[j++] = '.';
        for (i = 0; i < 3 && e->ext[i] != ' '; i++) out[j++] = e->ext[i];
    }
    out[j] = 0;
}

/* --- Поиск в директории -------------------------------------------------- */

/* Ищем запись по имени в директории dir_cluster.
 * Возвращает 1 если найдено: копирует dirent в *out, отдаёт LBA+idx. */
static int fat_find_entry(uint32_t dir_cluster, const char *name,
                          fat32_dirent_t *out, uint32_t *out_lba, uint32_t *out_idx) {
    uint8_t  dir_buf[512];
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, dir_buf) != 0) return 0;
            fat32_dirent_t *entries = (fat32_dirent_t *)dir_buf;
            for (int i = 0; i < 512 / 32; i++) {
                fat32_dirent_t *e = &entries[i];
                if (e->name[0] == 0x00) return 0;            /* конец директории */
                if (e->name[0] == 0xE5) continue;            /* удалённый        */
                if (e->attr == FAT32_ATTR_LFN) continue;     /* LFN              */
                if (e->attr & FAT32_ATTR_VOLUME) continue;   /* метка тома       */
                if (fat_name_match(name, e->name, e->ext)) {
                    *out = *e;
                    if (out_lba) *out_lba = lba + s;
                    if (out_idx) *out_idx = (uint32_t)i;
                    return 1;
                }
            }
        }
        cluster = fat_get_next_cluster(cluster);
    }
    return 0;
}

/* Находим свободный слот dirent в директории; при нехватке — расширяем цепочку.
 * Возвращает 0 при успехе, отдаёт LBA+idx слота. */
static int fat_find_free_slot(uint32_t dir_cluster, uint32_t *out_lba, uint32_t *out_idx) {
    uint8_t  dir_buf[512];
    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        last_cluster = cluster;
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, dir_buf) != 0) return -1;
            fat32_dirent_t *entries = (fat32_dirent_t *)dir_buf;
            for (int i = 0; i < 512 / 32; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00 || first == 0xE5) {
                    *out_lba = lba + s;
                    *out_idx = (uint32_t)i;
                    return 0;
                }
            }
        }
        cluster = fat_get_next_cluster(cluster);
    }

    /* Свободных слотов нет — расширяем директорию новым кластером */
    uint32_t newc = fat_alloc_cluster();
    if (!newc) return -1;
    if (fat_set_next_cluster(last_cluster, newc) != 0) return -1;
    *out_lba = cluster_to_lba(newc); /* первый сектор уже обнулён */
    *out_idx = 0;
    return 0;
}

/* Записываем 32-байтный dirent в (lba, idx) */
static int fat_write_dirent(uint32_t lba, uint32_t idx, const fat32_dirent_t *e) {
    uint8_t buf[512];
    if (read_sector(lba, buf) != 0) return -1;
    fat32_dirent_t *entries = (fat32_dirent_t *)buf;
    entries[idx] = *e;
    return write_sector(lba, buf);
}

/* --- Создание VFS-ноды для записи диска --------------------------------- */

static vfs_node_t *fat_make_vnode(const fat32_dirent_t *e,
                                  uint32_t parent_cluster,
                                  uint32_t dirent_lba, uint32_t dirent_idx) {
    vfs_node_t *vnode = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!vnode) return 0;
    fat32_priv_t *priv = (fat32_priv_t *)kmalloc(sizeof(fat32_priv_t));
    if (!priv) { kfree(vnode); return 0; }

    uint32_t cluster = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
    vnode->inode   = cluster;
    vnode->size    = e->size;
    vnode->type    = (e->attr & FAT32_ATTR_DIR) ? VFS_DIR : VFS_FILE;
    vnode->flags   = 0;
    vnode->mode    = 0;  /* FAT32 без прав: 0 = «всем 0755» (см. vfs_access) */
    vnode->uid     = 0;
    vnode->gid     = 0;
    vnode->ops     = &fat32_ops;
    vnode->mount   = 0;
    priv->parent_cluster = parent_cluster;
    priv->dirent_lba     = dirent_lba;
    priv->dirent_idx     = dirent_idx;
    vnode->fs_data = priv;

    fat_extract_name(e, vnode->name);
    return vnode;
}

/* Обновляем поля dirent (cluster + size) на диске */
static int fat_update_dirent(fat32_priv_t *priv, uint32_t cluster, uint32_t size) {
    if (!priv || priv->dirent_lba == 0) return -1;
    uint8_t buf[512];
    if (read_sector(priv->dirent_lba, buf) != 0) return -1;
    fat32_dirent_t *e = &((fat32_dirent_t *)buf)[priv->dirent_idx];
    e->cluster_hi = (uint16_t)(cluster >> 16);
    e->cluster_lo = (uint16_t)(cluster & 0xFFFF);
    e->size       = size;
    return write_sector(priv->dirent_lba, buf);
}

/* ============================================================================
 * Инициализация / монтирование
 * ============================================================================ */

int fat32_init(void) {
    if (read_sector(0, sector_buf) != 0) {
        fb_puts("[FAT32] Failed to read boot sector\n");
        return -1;
    }
    fat32_boot_t *bs = (fat32_boot_t *)sector_buf;
    if (bs->boot_sig != 0xAA55) {
        fb_puts("[FAT32] Invalid boot signature\n");
        return -1;
    }

    g_vol.bytes_per_sector    = bs->bytes_per_sector;
    g_vol.sectors_per_cluster = bs->sectors_per_cluster;
    g_vol.root_cluster        = bs->root_cluster;
    g_vol.fat_begin_lba       = bs->reserved_sectors;
    g_vol.num_fats            = bs->num_fats;
    g_vol.sectors_per_fat     = bs->sectors_per_fat_32;
    g_vol.cluster_begin_lba   = bs->reserved_sectors + (bs->num_fats * bs->sectors_per_fat_32);

    uint32_t data_sectors = bs->total_sectors_32 - g_vol.cluster_begin_lba;
    g_vol.total_clusters  = bs->sectors_per_cluster ? (data_sectors / bs->sectors_per_cluster) : 0;

    fb_puts("[FAT32] Mounted (read/write)\n");
    return 0;
}

void fat32_mount(void) {
    /* Заполняем единую таблицу операций один раз */
    fat32_ops.open    = (int (*)(struct vfs_node *, uint32_t))fat32_open_vfs;
    fat32_ops.close   = (void (*)(struct vfs_node *))fat32_close_vfs;
    fat32_ops.read    = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, uint8_t *))fat32_read_vfs;
    fat32_ops.write   = (int32_t (*)(struct vfs_node *, uint32_t, uint32_t, const uint8_t *))fat32_write_vfs;
    fat32_ops.finddir = (struct vfs_node *(*)(struct vfs_node *, const char *))fat32_finddir_vfs;
    fat32_ops.mkdir   = (int (*)(struct vfs_node *, const char *))fat32_mkdir_vfs;
    fat32_ops.create  = (int (*)(struct vfs_node *, const char *))fat32_create_vfs;
    fat32_ops.unlink  = (int (*)(struct vfs_node *, const char *))fat32_unlink_vfs;
    fat32_ops.readdir = (const char *(*)(struct vfs_node *, uint32_t))fat32_readdir_vfs;

    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!root) { fb_puts("[FAT32] Failed to allocate root node\n"); return; }

    root->inode   = g_vol.root_cluster;
    root->size    = 0;
    root->type    = VFS_DIR;
    root->flags   = 0;
    root->mode    = 0;   /* без прав — дефолт */
    root->uid     = 0;
    root->gid     = 0;
    root->name[0] = '/';
    root->name[1] = 0;
    root->ops     = &fat32_ops;
    root->fs_data = 0;          /* у корня нет dirent на диске */
    root->mount   = 0;

    vfs_mount_root(root);
    fb_puts("[FAT32] Mounted as VFS root\n");
}

/* ============================================================================
 * VFS Callbacks
 * ============================================================================ */

int32_t fat32_read_vfs(void *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    if (!vnode || vnode->type != VFS_FILE) return -1;

    uint32_t cluster   = vnode->inode;
    uint32_t file_size = vnode->size;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint32_t bytes_read   = 0;
    uint32_t cluster_size = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;
    static uint8_t cluster_buf[4096];

    uint32_t skip_clusters     = offset / cluster_size;
    uint32_t offset_in_cluster = offset % cluster_size;
    for (uint32_t i = 0; i < skip_clusters && cluster < FAT32_EOC; i++)
        cluster = fat_get_next_cluster(cluster);

    while (size > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, cluster_buf + (s * 512)) != 0)
                return bytes_read;
        }
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

#define FAT_MAX_FILE_CLUSTERS 2048   /* до 8 МБ при кластере 4 КБ */

int32_t fat32_write_vfs(void *node, uint32_t offset, uint32_t size, const uint8_t *buf) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    if (!vnode || vnode->type != VFS_FILE || size == 0) return 0;
    fat32_priv_t *priv = (fat32_priv_t *)vnode->fs_data;

    uint32_t cluster_size = g_vol.sectors_per_cluster * g_vol.bytes_per_sector;
    uint32_t end          = offset + size;
    uint32_t needed        = (end + cluster_size - 1) / cluster_size;
    if (needed == 0 || needed > FAT_MAX_FILE_CLUSTERS) return -1;

    /* Обеспечиваем первый кластер */
    if (vnode->inode < 2 || vnode->inode >= FAT32_EOC) {
        uint32_t c = fat_alloc_cluster();
        if (!c) return -1;
        vnode->inode = c;
        fat_update_dirent(priv, c, vnode->size);
    }

    /* Строим/расширяем цепочку до needed кластеров */
    static uint32_t chain[FAT_MAX_FILE_CLUSTERS];
    uint32_t count = 0;
    uint32_t cl    = vnode->inode;
    while (cl >= 2 && cl < FAT32_EOC && count < needed) {
        chain[count++] = cl;
        uint32_t next = fat_get_next_cluster(cl);
        if ((next < 2 || next >= FAT32_EOC) && count < needed) {
            /* нужно ещё — выделяем и линкуем */
            uint32_t nc = fat_alloc_cluster();
            if (!nc) return -1;
            if (fat_set_next_cluster(cl, nc) != 0) return -1;
            next = nc;
        }
        cl = next;
    }
    if (count < needed) return -1;

    /* Пишем данные по кластерам (read-modify-write для частичных) */
    uint8_t  cbuf[4096];
    uint32_t written = 0;
    while (written < size) {
        uint32_t pos      = offset + written;
        uint32_t ci       = pos / cluster_size;
        uint32_t in_off   = pos % cluster_size;
        uint32_t to_copy  = cluster_size - in_off;
        if (to_copy > size - written) to_copy = size - written;

        uint32_t lba = cluster_to_lba(chain[ci]);
        /* RMW только если пишем не весь кластер целиком */
        if (in_off != 0 || to_copy != cluster_size) {
            for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++)
                read_sector(lba + s, cbuf + s * 512);
        }
        for (uint32_t i = 0; i < to_copy; i++)
            cbuf[in_off + i] = buf[written + i];
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++)
            if (write_sector(lba + s, cbuf + s * 512) != 0) return (int32_t)written;

        written += to_copy;
    }

    /* Обновляем размер */
    if (end > vnode->size) {
        vnode->size = end;
        fat_update_dirent(priv, vnode->inode, vnode->size);
    }
    return (int32_t)written;
}

void* fat32_open_vfs(void *node, uint32_t flags) {
    (void)flags;
    return node;
}

void fat32_close_vfs(void *node) {
    (void)node;
}

/* Листинг директории: index 0,1,2... → имя или 0 */
void* fat32_readdir_vfs(void *node, uint32_t index) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    if (!vnode || vnode->type != VFS_DIR) return 0;

    static char namebuf[13];
    uint32_t dir_cluster = vnode->inode ? vnode->inode : g_vol.root_cluster;
    uint32_t cluster = dir_cluster;
    uint32_t seen = 0;
    uint8_t  dir_buf[512];

    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, dir_buf) != 0) return 0;
            fat32_dirent_t *entries = (fat32_dirent_t *)dir_buf;
            for (int i = 0; i < 512 / 32; i++) {
                fat32_dirent_t *e = &entries[i];
                if (e->name[0] == 0x00) return 0;
                if (e->name[0] == 0xE5) continue;
                if (e->attr == FAT32_ATTR_LFN) continue;
                if (e->attr & FAT32_ATTR_VOLUME) continue;
                if (seen == index) {
                    fat_extract_name(e, namebuf);
                    return namebuf;
                }
                seen++;
            }
        }
        cluster = fat_get_next_cluster(cluster);
    }
    return 0;
}

/* Поиск в директории (любой, не только root) */
void* fat32_finddir_vfs(void *node, const char *name) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    uint32_t dir_cluster = (vnode && vnode->inode) ? vnode->inode : g_vol.root_cluster;

    fat32_dirent_t e;
    uint32_t lba, idx;
    if (!fat_find_entry(dir_cluster, name, &e, &lba, &idx)) return 0;
    return fat_make_vnode(&e, dir_cluster, lba, idx);
}

/* Общая часть create/mkdir: создаёт dirent в директории */
static int fat_make_entry(uint32_t dir_cluster, const char *name,
                          uint8_t attr, uint32_t first_cluster, uint32_t size) {
    /* Проверяем что нет дубликата */
    fat32_dirent_t tmp; uint32_t tl, ti;
    if (fat_find_entry(dir_cluster, name, &tmp, &tl, &ti)) return -1;

    uint32_t lba, idx;
    if (fat_find_free_slot(dir_cluster, &lba, &idx) != 0) return -1;

    fat32_dirent_t e;
    uint8_t *raw = (uint8_t *)&e;
    for (uint32_t i = 0; i < sizeof(e); i++) raw[i] = 0;
    uint8_t n83[11];
    fat_make_83(name, n83);
    for (int i = 0; i < 8; i++) e.name[i] = n83[i];
    for (int i = 0; i < 3; i++) e.ext[i]  = n83[8 + i];
    e.attr       = attr;
    e.cluster_hi = (uint16_t)(first_cluster >> 16);
    e.cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    e.size       = size;
    return fat_write_dirent(lba, idx, &e);
}

int fat32_create_vfs(void *node, const char *name) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    uint32_t dir_cluster = (vnode && vnode->inode) ? vnode->inode : g_vol.root_cluster;
    /* Пустой файл: первый кластер 0, размер 0 */
    return fat_make_entry(dir_cluster, name, FAT32_ATTR_ARCHIVE, 0, 0);
}

int fat32_mkdir_vfs(void *node, const char *name) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    uint32_t dir_cluster = (vnode && vnode->inode) ? vnode->inode : g_vol.root_cluster;

    /* Выделяем кластер под новую директорию */
    uint32_t newc = fat_alloc_cluster();
    if (!newc) return -1;

    /* Записи "." (на себя) и ".." (на родителя) в начале кластера */
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    fat32_dirent_t *de = (fat32_dirent_t *)buf;

    for (int i = 0; i < 8; i++) de[0].name[i] = ' ';
    for (int i = 0; i < 3; i++) de[0].ext[i]  = ' ';
    de[0].name[0] = '.';
    de[0].attr = FAT32_ATTR_DIR;
    de[0].cluster_hi = (uint16_t)(newc >> 16);
    de[0].cluster_lo = (uint16_t)(newc & 0xFFFF);

    for (int i = 0; i < 8; i++) de[1].name[i] = ' ';
    for (int i = 0; i < 3; i++) de[1].ext[i]  = ' ';
    de[1].name[0] = '.'; de[1].name[1] = '.';
    de[1].attr = FAT32_ATTR_DIR;
    uint32_t parent = (dir_cluster == g_vol.root_cluster) ? 0 : dir_cluster;
    de[1].cluster_hi = (uint16_t)(parent >> 16);
    de[1].cluster_lo = (uint16_t)(parent & 0xFFFF);

    if (write_sector(cluster_to_lba(newc), buf) != 0) {
        fat_free_chain(newc);
        return -1;
    }

    /* Записываем dirent самой директории в родителя */
    if (fat_make_entry(dir_cluster, name, FAT32_ATTR_DIR, newc, 0) != 0) {
        fat_free_chain(newc);
        return -1;
    }
    return 0;
}

/* Проверяем, пуста ли директория (кроме "." и "..") */
static int fat_dir_is_empty(uint32_t dir_cluster) {
    uint8_t  dir_buf[512];
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < g_vol.sectors_per_cluster; s++) {
            if (read_sector(lba + s, dir_buf) != 0) return 0;
            fat32_dirent_t *entries = (fat32_dirent_t *)dir_buf;
            for (int i = 0; i < 512 / 32; i++) {
                fat32_dirent_t *e = &entries[i];
                if (e->name[0] == 0x00) return 1;
                if (e->name[0] == 0xE5) continue;
                if (e->attr == FAT32_ATTR_LFN) continue;
                if (e->name[0] == '.') continue; /* "." и ".." */
                return 0;
            }
        }
        cluster = fat_get_next_cluster(cluster);
    }
    return 1;
}

int fat32_unlink_vfs(void *node, const char *name) {
    vfs_node_t *vnode = (vfs_node_t *)node;
    uint32_t dir_cluster = (vnode && vnode->inode) ? vnode->inode : g_vol.root_cluster;

    fat32_dirent_t e;
    uint32_t lba, idx;
    if (!fat_find_entry(dir_cluster, name, &e, &lba, &idx)) return -1;

    uint32_t first = ((uint32_t)e.cluster_hi << 16) | e.cluster_lo;

    /* Директорию удаляем только если пуста */
    if (e.attr & FAT32_ATTR_DIR) {
        if (first >= 2 && !fat_dir_is_empty(first)) return -1;
    }

    /* Помечаем dirent как удалённый */
    uint8_t buf[512];
    if (read_sector(lba, buf) != 0) return -1;
    ((fat32_dirent_t *)buf)[idx].name[0] = 0xE5;
    if (write_sector(lba, buf) != 0) return -1;

    /* Освобождаем цепочку кластеров */
    if (first >= 2) fat_free_chain(first);
    return 0;
}
