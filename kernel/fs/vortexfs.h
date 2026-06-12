/* =============================================================================
 * VortexOS — kernel/fs/vortexfs.h
 * VortexFS — нативная файловая система VortexOS.
 *
 * On-disk layout (512-байтные секторы, от начала раздела):
 *   Сектор  0:              Суперблок
 *   Секторы 1..1:           Bitmap инодов     (4096 инодов макс.)
 *   Секторы 2..9:           Bitmap блоков данных (до 32768 блоков)
 *   Секторы 10..521:        Таблица инодов    (4096 × 64 байта)
 *   Секторы 522+:           Блоки данных
 *
 * Блок 0 зарезервирован (sentinel). Валидные блоки начинаются с 1.
 * ============================================================================= */

#ifndef VOS_VORTEXFS_H
#define VOS_VORTEXFS_H

#include "vfs.h"

#define VTXFS_MAGIC             0x56545846  /* "VTXF" little-endian */
#define VTXFS_VERSION           2   /* v2 = + журнал; v1 монтируется без него */
#define VTXFS_BLOCK_SIZE        512

#define VTXFS_MAX_INODES        4096
#define VTXFS_MAX_DIRECT        10
#define VTXFS_INODE_SIZE        64
#define VTXFS_DIRENT_SIZE       64
#define VTXFS_NAME_MAX          59
#define VTXFS_DIRENTS_PER_BLOCK (VTXFS_BLOCK_SIZE / VTXFS_DIRENT_SIZE) /* 8 */

/* === On-disk суперблок (512 байт) ======================================== */
typedef struct __attribute__((packed)) {
    uint32_t magic;                 /* VTXFS_MAGIC                  */
    uint32_t version;               /* VTXFS_VERSION                */
    uint32_t total_blocks;          /* всего блоков данных           */
    uint32_t total_inodes;          /* всего инодов                  */
    uint32_t free_blocks;           /* свободных блоков              */
    uint32_t free_inodes;           /* свободных инодов              */
    uint32_t inode_bitmap_start;    /* LBA bitmap инодов (отн.)      */
    uint32_t block_bitmap_start;    /* LBA bitmap блоков (отн.)      */
    uint32_t inode_table_start;     /* LBA таблицы инодов (отн.)     */
    uint32_t data_start;            /* LBA первого блока данных (отн.) */
    uint32_t root_inode;            /* номер root inode (= 0)        */
    /* v2: журнал (в v1 здесь нули → журнал выключен) */
    uint32_t journal_start;         /* LBA журнала (отн.), 0 = нет   */
    uint32_t journal_sectors;       /* размер журнала в секторах     */
    uint8_t  _reserved[460];        /* до 512 байт                   */
} vtxfs_super_t;

/* === Журнал (metadata WAL, v2) ===========================================
 * Область из VTXFS_JOURNAL_SECTORS секторов между inode table и данными.
 * Транзакция: [hdr][payload × count][commit]. Заголовок хранит список LBA;
 * commit-маркер пишется ПОСЛЕ payload — есть commit → txn полная, можно
 * реиграть (идемпотентно); нет — отбрасываем. После применения заголовок
 * обнуляется. Журналируются только метаданные (bitmaps, суперблок, иноды,
 * каталоги); содержимое файлов пишется напрямую. */
#define VTXFS_JOURNAL_SECTORS   64
#define VTXFS_JRN_MAX_CAP       30          /* payload-секторов на txn      */
#define VTXFS_JRN_HDR_MAGIC     0x4E524A56  /* "VJRN" little-endian         */
#define VTXFS_JRN_CMT_MAGIC     0x544D4356  /* "VCMT" little-endian         */

typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* VTXFS_JRN_HDR_MAGIC              */
    uint32_t seq;                       /* номер транзакции                 */
    uint32_t count;                     /* секторов в payload               */
    uint32_t checksum;                  /* seq + count + сумма lba[]        */
    uint32_t lba[VTXFS_JRN_MAX_CAP];    /* целевые сектора (отн. раздела)   */
} vtxfs_jrn_hdr_t;

/* === On-disk инод (64 байта) ============================================= */
typedef struct __attribute__((packed)) {
    uint32_t type;                  /* VFS_FILE / VFS_DIR / 0=free   */
    uint32_t size;                  /* размер файла в байтах         */
    uint32_t blocks;                /* кол-во выделенных блоков      */
    uint32_t direct[VTXFS_MAX_DIRECT]; /* прямые указатели (0 = нет) */
    uint32_t indirect;              /* один уровень косвенности      */
    uint32_t dbl_indirect;          /* двойная косвенность           */
    /* Права (бывший _pad — старые образы читаются: mode==0 → дефолт) */
    uint16_t mode;                  /* unix-права rwxrwxrwx (07777)  */
    uint8_t  uid;                   /* владелец                      */
    uint8_t  gid;                   /* группа                        */
} vtxfs_inode_t;

/* Дефолтные права (и интерпретация mode==0 в старых образах) */
#define VTXFS_DEF_DIR_MODE  0755
#define VTXFS_DEF_FILE_MODE 0644

/* === On-disk запись каталога (64 байта) =================================== */
typedef struct __attribute__((packed)) {
    char     name[VTXFS_NAME_MAX + 1]; /* 60 байт, с нулём на конце */
    uint32_t inode;                     /* номер инода               */
} vtxfs_dirent_t;

/* === Публичный API ======================================================= */

/*  Форматирование раздела как VortexFS.
 *  ata_slave   — 0 (master) или 1 (slave)
 *  start_lba   — начало раздела на диске (абсолютный LBA)
 *  total_sectors — размер раздела в секторах                                */
int          vortexfs_mkfs(uint8_t ata_slave, uint32_t start_lba,
                           uint32_t total_sectors);

/*  Инициализация — читает суперблок, монтирует.
 *  Возвращает 0 при успехе.                                                 */
int          vortexfs_init(uint8_t ata_slave, uint32_t start_lba);

/*  Получить корневую VFS ноду (после init).                                 */
vfs_node_t  *vortexfs_get_root(void);

#endif /* VOS_VORTEXFS_H */
