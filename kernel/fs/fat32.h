/* =============================================================================
 * VortexOS — kernel/fs/fat32.h
 * Упрощённый FAT32 драйвер для чтения/записи на ATA диск.
 * ============================================================================= */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

/* Boot Sector (512 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;          /* 0 для FAT32 */
    uint16_t total_sectors_16;      /* 0 для FAT32 */
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16;    /* 0 для FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    /* FAT32 Specific */
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved2;
    uint8_t  boot_signature;
    uint32_t serial_number;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t boot_sig;              /* 0xAA55 */
} fat32_boot_t;

/* Directory Entry (32 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  ctime_tenths;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} fat32_dirent_t;

/* Атрибуты файлов */
#define FAT32_ATTR_RO       0x01
#define FAT32_ATTR_HIDDEN   0x02
#define FAT32_ATTR_SYSTEM   0x04
#define FAT32_ATTR_VOLUME   0x08
#define FAT32_ATTR_DIR      0x10
#define FAT32_ATTR_ARCHIVE  0x20
#define FAT32_ATTR_LFN      0x0F

/* Специальные значения cluster */
#define FAT32_FREE          0x00000000
#define FAT32_EOC           0x0FFFFFF8  /* End of chain */
#define FAT32_BAD           0x0FFFFFF7

/* FAT32 volume */
typedef struct {
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t bytes_per_sector;
} fat32_volume_t;

/* Инициализация */
int  fat32_init(void);
void fat32_mount(void);

/* VFS callbacks */
int32_t fat32_read_vfs(void *node, uint32_t offset, uint32_t size, uint8_t *buf);
int32_t fat32_write_vfs(void *node, uint32_t offset, uint32_t size, const uint8_t *buf);
void*   fat32_open_vfs(void *node, uint32_t flags);
void    fat32_close_vfs(void *node);
void*   fat32_readdir_vfs(void *node, uint32_t index);
void*   fat32_finddir_vfs(void *node, const char *name);
int     fat32_mkdir_vfs(void *node, const char *name);
int     fat32_create_vfs(void *node, const char *name);

#endif
