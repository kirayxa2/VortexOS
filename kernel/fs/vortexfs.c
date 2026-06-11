/* =============================================================================
 * VortexOS — kernel/fs/vortexfs.c
 * VortexFS — нативная файловая система VortexOS.
 *
 * Работает поверх ATA PIO. Поддерживает файлы, каталоги, mkdir, create,
 * unlink, read, write, finddir, readdir.
 *
 * Блок 0 зарезервирован как sentinel — в direct[]/indirect значение 0
 * означает «блок не выделен».
 * ============================================================================= */

#include "vortexfs.h"
#include "ata.h"
#include "heap.h"
#include "fb.h"

/* ========================================================================= *
 *  Глобальное состояние FS                                                  *
 * ========================================================================= */

static uint8_t       g_slave;          /* ATA master(0) / slave(1)         */
static uint32_t      g_start_lba;      /* LBA начала раздела               */
static vtxfs_super_t g_sb;             /* кэшированный суперблок           */
static uint8_t       g_mounted;        /* флаг: смонтирована?              */

static vfs_ops_t     vtxfs_ops;        /* единая таблица VFS-операций      */
static vfs_node_t   *g_root;           /* корневая VFS нода                */

/* Статические буферы (512 байт) — не на стеке, экономим kernel stack */
static uint8_t sec_tmp[512];
static uint8_t sec_tmp2[512];

/* Простой кэш VFS-нод (чтобы finddir не плодил дубликаты) */
#define NODE_CACHE_MAX 256
static vfs_node_t *node_cache[NODE_CACHE_MAX];
static uint32_t    node_cache_n;

/* ========================================================================= *
 *  Доступ к секторам (со смещением раздела)                                 *
 * ========================================================================= */

static int rd_sector(uint32_t rel, void *buf) {
    return ata_read_sector(g_slave, g_start_lba + rel, buf);
}
static int wr_sector(uint32_t rel, const void *buf) {
    return ata_write(g_slave, g_start_lba + rel, 1, (const uint16_t *)buf);
}

/* ========================================================================= *
 *  Суперблок                                                                *
 * ========================================================================= */

static int sb_read(void) {
    if (rd_sector(0, &g_sb) != 0) return -1;
    return (g_sb.magic == VTXFS_MAGIC && g_sb.version == VTXFS_VERSION) ? 0 : -1;
}
static int sb_write(void) { return wr_sector(0, &g_sb); }

/* ========================================================================= *
 *  Bitmap-операции                                                          *
 * ========================================================================= */

static int bm_get(uint32_t bm_start, uint32_t idx) {
    uint32_t sec = bm_start + (idx / 8) / 512;
    uint32_t off = (idx / 8) % 512;
    uint32_t bit = idx % 8;
    if (rd_sector(sec, sec_tmp) != 0) return -1;
    return (sec_tmp[off] >> bit) & 1;
}

static int bm_set(uint32_t bm_start, uint32_t idx, int val) {
    uint32_t sec = bm_start + (idx / 8) / 512;
    uint32_t off = (idx / 8) % 512;
    uint32_t bit = idx % 8;
    if (rd_sector(sec, sec_tmp) != 0) return -1;
    if (val) sec_tmp[off] |=  (1u << bit);
    else     sec_tmp[off] &= ~(1u << bit);
    return wr_sector(sec, sec_tmp);
}

/* Ищет первый свободный бит, занимает, возвращает индекс или -1 */
static int bm_alloc(uint32_t bm_start, uint32_t max) {
    /* Пробегаем по секторам bitmap целиком — быстрее, чем bit-by-bit */
    uint32_t bm_sectors = (max / 8 + 511) / 512;
    uint32_t global_bit = 0;

    for (uint32_t s = 0; s < bm_sectors && global_bit < max; s++) {
        if (rd_sector(bm_start + s, sec_tmp) != 0) return -1;
        for (uint32_t b = 0; b < 512 && global_bit < max; b++) {
            if (sec_tmp[b] == 0xFF) { global_bit += 8; continue; }
            for (uint32_t bit = 0; bit < 8 && global_bit < max; bit++, global_bit++) {
                if (!(sec_tmp[b] & (1u << bit))) {
                    sec_tmp[b] |= (1u << bit);
                    wr_sector(bm_start + s, sec_tmp);
                    return (int)global_bit;
                }
            }
        }
    }
    return -1;
}

/* ========================================================================= *
 *  Inode I/O  (8 инодов на сектор: 512 / 64 = 8)                           *
 * ========================================================================= */

static int ino_read(uint32_t ino, vtxfs_inode_t *out) {
    uint32_t sec = g_sb.inode_table_start + ino / 8;
    uint32_t off = (ino % 8) * VTXFS_INODE_SIZE;
    if (rd_sector(sec, sec_tmp) != 0) return -1;
    const uint8_t *s = sec_tmp + off;
    uint8_t *d = (uint8_t *)out;
    for (int i = 0; i < VTXFS_INODE_SIZE; i++) d[i] = s[i];
    return 0;
}

static int ino_write(uint32_t ino, const vtxfs_inode_t *in) {
    uint32_t sec = g_sb.inode_table_start + ino / 8;
    uint32_t off = (ino % 8) * VTXFS_INODE_SIZE;
    if (rd_sector(sec, sec_tmp) != 0) return -1;
    uint8_t *d = sec_tmp + off;
    const uint8_t *s = (const uint8_t *)in;
    for (int i = 0; i < VTXFS_INODE_SIZE; i++) d[i] = s[i];
    return wr_sector(sec, sec_tmp);
}

static int ino_alloc(void) {
    int n = bm_alloc(g_sb.inode_bitmap_start, g_sb.total_inodes);
    if (n >= 0) { g_sb.free_inodes--; sb_write(); }
    return n;
}

static void ino_free(uint32_t ino) {
    bm_set(g_sb.inode_bitmap_start, ino, 0);
    g_sb.free_inodes++;
    sb_write();
}

/* ========================================================================= *
 *  Блоки данных                                                             *
 * ========================================================================= */

static int blk_alloc(void) {
    int n = bm_alloc(g_sb.block_bitmap_start, g_sb.total_blocks);
    if (n >= 0) {
        g_sb.free_blocks--;
        sb_write();
        /* Обнуляем свежий блок */
        for (int i = 0; i < 512; i++) sec_tmp2[i] = 0;
        wr_sector(g_sb.data_start + (uint32_t)n, sec_tmp2);
    }
    return n;
}

static void blk_free(uint32_t blk) {
    bm_set(g_sb.block_bitmap_start, blk, 0);
    g_sb.free_blocks++;
    sb_write();
}

static int blk_read(uint32_t blk, void *buf) {
    return rd_sector(g_sb.data_start + blk, buf);
}
static int blk_write(uint32_t blk, const void *buf) {
    return wr_sector(g_sb.data_start + blk, buf);
}

/* Получить номер блока по логическому индексу внутри файла.
 * 0 = «не выделен» (sentinel).
 *
 * Уровни:
 *   [0 .. MAX_DIRECT-1]               — direct
 *   [MAX_DIRECT .. MAX_DIRECT+127]    — indirect  (128 ptr)
 *   [MAX_DIRECT+128 .. MAX_DIRECT+128+128*128-1] — double-indirect */
static uint32_t inode_get_blk(vtxfs_inode_t *di, uint32_t idx) {
    if (idx < VTXFS_MAX_DIRECT)
        return di->direct[idx];

    uint32_t off = idx - VTXFS_MAX_DIRECT;

    /* Single indirect */
    if (off < 128) {
        if (di->indirect == 0) return 0;
        uint32_t ibuf[128];
        if (blk_read(di->indirect, ibuf) != 0) return 0;
        return ibuf[off];
    }

    /* Double indirect */
    off -= 128;
    if (di->dbl_indirect == 0 || off >= 128 * 128) return 0;
    uint32_t d1[128];
    if (blk_read(di->dbl_indirect, d1) != 0) return 0;
    uint32_t slot = off / 128;
    if (d1[slot] == 0) return 0;
    uint32_t d2[128];
    if (blk_read(d1[slot], d2) != 0) return 0;
    return d2[off % 128];
}

/* Установить блок по логическому индексу, при необходимости выделяя
 * indirect / double-indirect блоки. */
static int inode_set_blk(vtxfs_inode_t *di, uint32_t idx, uint32_t blk) {
    if (idx < VTXFS_MAX_DIRECT) {
        di->direct[idx] = blk;
        return 0;
    }

    uint32_t off = idx - VTXFS_MAX_DIRECT;

    /* Single indirect */
    if (off < 128) {
        if (di->indirect == 0) {
            int ib = blk_alloc();
            if (ib < 0) return -1;
            di->indirect = (uint32_t)ib;
        }
        uint32_t ibuf[128];
        if (blk_read(di->indirect, ibuf) != 0) return -1;
        ibuf[off] = blk;
        return blk_write(di->indirect, ibuf);
    }

    /* Double indirect */
    off -= 128;
    if (off >= 128 * 128) return -1;

    /* Allocate top-level double-indirect block if needed */
    if (di->dbl_indirect == 0) {
        int db = blk_alloc();
        if (db < 0) return -1;
        di->dbl_indirect = (uint32_t)db;
    }
    uint32_t d1[128];
    if (blk_read(di->dbl_indirect, d1) != 0) return -1;

    uint32_t slot = off / 128;
    /* Allocate second-level indirect block if needed */
    if (d1[slot] == 0) {
        int sb = blk_alloc();
        if (sb < 0) return -1;
        d1[slot] = (uint32_t)sb;
        if (blk_write(di->dbl_indirect, d1) != 0) return -1;
    }
    uint32_t d2[128];
    if (blk_read(d1[slot], d2) != 0) return -1;
    d2[off % 128] = blk;
    return blk_write(d1[slot], d2);
}

/* ========================================================================= *
 *  VFS-ноды (кэш + создание)                                               *
 * ========================================================================= */

static vfs_node_t *node_find_cached(uint32_t ino) {
    for (uint32_t i = 0; i < node_cache_n; i++)
        if (node_cache[i] && node_cache[i]->inode == ino)
            return node_cache[i];
    return 0;
}

static vfs_node_t *node_make(uint32_t ino) {
    vfs_node_t *c = node_find_cached(ino);
    if (c) return c;

    vtxfs_inode_t di;
    if (ino_read(ino, &di) != 0 || di.type == 0) return 0;

    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return 0;

    /* Обнуляем */
    uint8_t *p = (uint8_t *)n;
    for (uint32_t i = 0; i < sizeof(vfs_node_t); i++) p[i] = 0;

    n->inode   = ino;
    n->type    = di.type;
    n->size    = di.size;
    n->ops     = &vtxfs_ops;
    n->fs_data = 0;

    if (node_cache_n < NODE_CACHE_MAX)
        node_cache[node_cache_n++] = n;

    return n;
}

static void node_uncache(vfs_node_t *n) {
    for (uint32_t i = 0; i < node_cache_n; i++) {
        if (node_cache[i] == n) {
            node_cache[i] = node_cache[--node_cache_n];
            return;
        }
    }
}

/* ========================================================================= *
 *  Вспомогательные: строки                                                  *
 * ========================================================================= */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ========================================================================= *
 *  Директория: добавить / удалить / найти запись                            *
 * ========================================================================= */

/* Добавляет запись (name → child_ino) в каталог dir_ino */
static int dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino) {
    vtxfs_inode_t di;
    if (ino_read(dir_ino, &di) != 0) return -1;

    /* Ищем свободный слот в существующих блоках */
    for (uint32_t bi = 0; bi < di.blocks; bi++) {
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) continue;

        uint8_t buf[512];
        if (blk_read(b, buf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)buf;
        for (int e = 0; e < VTXFS_DIRENTS_PER_BLOCK; e++) {
            if (ent[e].name[0] == 0) {
                str_copy(ent[e].name, name, VTXFS_NAME_MAX + 1);
                ent[e].inode = child_ino;
                blk_write(b, buf);
                di.size += VTXFS_DIRENT_SIZE;
                ino_write(dir_ino, &di);
                return 0;
            }
        }
    }

    /* Нет места — выделяем новый блок */
    int nb = blk_alloc();
    if (nb < 0) return -1;
    if (inode_set_blk(&di, di.blocks, (uint32_t)nb) != 0) return -1;
    di.blocks++;

    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;

    vtxfs_dirent_t *ent = (vtxfs_dirent_t *)buf;
    str_copy(ent[0].name, name, VTXFS_NAME_MAX + 1);
    ent[0].inode = child_ino;
    blk_write((uint32_t)nb, buf);

    di.size += VTXFS_DIRENT_SIZE;
    ino_write(dir_ino, &di);
    return 0;
}

/* Удаляет запись по имени из каталога */
static int dir_remove(uint32_t dir_ino, const char *name) {
    vtxfs_inode_t di;
    if (ino_read(dir_ino, &di) != 0) return -1;

    for (uint32_t bi = 0; bi < di.blocks; bi++) {
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) continue;

        uint8_t buf[512];
        if (blk_read(b, buf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)buf;
        for (int e = 0; e < VTXFS_DIRENTS_PER_BLOCK; e++) {
            if (ent[e].name[0] && str_eq(ent[e].name, name)) {
                ent[e].name[0] = 0;
                ent[e].inode   = 0;
                blk_write(b, buf);
                di.size -= VTXFS_DIRENT_SIZE;
                ino_write(dir_ino, &di);
                return 0;
            }
        }
    }
    return -1;
}

/* ========================================================================= *
 *  VFS колбэки                                                             *
 * ========================================================================= */

static int vtxfs_open_cb(vfs_node_t *n, uint32_t f) {
    (void)n; (void)f; return 0;
}
static void vtxfs_close_cb(vfs_node_t *n) {
    (void)n;
}

/* --- read --------------------------------------------------------------- */
static int32_t vtxfs_read_cb(vfs_node_t *vn, uint32_t off, uint32_t sz,
                              uint8_t *buf)
{
    if (!vn || vn->type != VFS_FILE) return -1;
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return -1;

    if (off >= di.size) return 0;
    if (off + sz > di.size) sz = di.size - off;

    uint32_t done = 0;
    while (done < sz) {
        uint32_t cur   = off + done;
        uint32_t bi    = cur / VTXFS_BLOCK_SIZE;
        uint32_t boff  = cur % VTXFS_BLOCK_SIZE;
        uint32_t chunk = VTXFS_BLOCK_SIZE - boff;
        if (chunk > sz - done) chunk = sz - done;

        if (bi >= di.blocks) break;
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) break;

        uint8_t bb[512];
        if (blk_read(b, bb) != 0) break;
        for (uint32_t i = 0; i < chunk; i++) buf[done + i] = bb[boff + i];
        done += chunk;
    }
    return (int32_t)done;
}

/* --- write -------------------------------------------------------------- */
static int32_t vtxfs_write_cb(vfs_node_t *vn, uint32_t off, uint32_t sz,
                               const uint8_t *buf)
{
    if (!vn || vn->type != VFS_FILE) return -1;
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return -1;

    uint32_t done = 0;
    while (done < sz) {
        uint32_t cur   = off + done;
        uint32_t bi    = cur / VTXFS_BLOCK_SIZE;
        uint32_t boff  = cur % VTXFS_BLOCK_SIZE;
        uint32_t chunk = VTXFS_BLOCK_SIZE - boff;
        if (chunk > sz - done) chunk = sz - done;

        uint32_t b = (bi < di.blocks) ? inode_get_blk(&di, bi) : 0;

        if (b == 0) {
            /* Выделяем недостающие блоки до bi включительно */
            while (di.blocks <= bi) {
                int nb = blk_alloc();
                if (nb < 0) goto out;
                if (inode_set_blk(&di, di.blocks, (uint32_t)nb) != 0) goto out;
                di.blocks++;
            }
            b = inode_get_blk(&di, bi);
            if (b == 0) goto out;
        }

        uint8_t bb[512];
        if (boff > 0 || chunk < VTXFS_BLOCK_SIZE)
            blk_read(b, bb);                 /* частичная запись → читаем */

        for (uint32_t i = 0; i < chunk; i++) bb[boff + i] = buf[done + i];
        if (blk_write(b, bb) != 0) break;
        done += chunk;
    }
out:
    {
        uint32_t end = off + done;
        if (end > di.size) di.size = end;
        ino_write(vn->inode, &di);
        vn->size = di.size;
    }
    return (int32_t)done;
}

/* --- finddir ------------------------------------------------------------ */
static vfs_node_t *vtxfs_finddir_cb(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return 0;
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return 0;

    for (uint32_t bi = 0; bi < di.blocks; bi++) {
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) continue;

        uint8_t bb[512];
        if (blk_read(b, bb) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)bb;
        for (int e = 0; e < VTXFS_DIRENTS_PER_BLOCK; e++) {
            if (ent[e].name[0] && str_eq(ent[e].name, name)) {
                vfs_node_t *child = node_make(ent[e].inode);
                if (child)
                    str_copy(child->name, ent[e].name, VFS_MAX_NAME);
                return child;
            }
        }
    }
    return 0;
}

/* --- readdir ------------------------------------------------------------ */
static const char *vtxfs_readdir_cb(vfs_node_t *vn, uint32_t index) {
    if (!vn || vn->type != VFS_DIR) return 0;
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return 0;

    static char rbuf[VTXFS_NAME_MAX + 1];
    uint32_t cnt = 0;

    for (uint32_t bi = 0; bi < di.blocks; bi++) {
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) continue;

        uint8_t bb[512];
        if (blk_read(b, bb) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)bb;
        for (int e = 0; e < VTXFS_DIRENTS_PER_BLOCK; e++) {
            if (ent[e].name[0] == 0) continue;
            if (cnt == index) {
                str_copy(rbuf, ent[e].name, VTXFS_NAME_MAX + 1);
                return rbuf;
            }
            cnt++;
        }
    }
    return 0;
}

/* --- mkdir -------------------------------------------------------------- */
static int vtxfs_mkdir_cb(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return -1;
    if (vtxfs_finddir_cb(vn, name)) return -1; /* уже есть */

    int ino = ino_alloc();
    if (ino < 0) return -1;

    vtxfs_inode_t di;
    uint8_t *p = (uint8_t *)&di;
    for (int i = 0; i < VTXFS_INODE_SIZE; i++) p[i] = 0;
    di.type   = VFS_DIR;
    di.size   = 0;
    di.blocks = 0;
    ino_write((uint32_t)ino, &di);

    if (dir_add(vn->inode, name, (uint32_t)ino) != 0) {
        ino_free((uint32_t)ino);
        return -1;
    }
    return 0;
}

/* --- create (файл) ------------------------------------------------------ */
static int vtxfs_create_cb(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return -1;
    if (vtxfs_finddir_cb(vn, name)) return -1;

    int ino = ino_alloc();
    if (ino < 0) return -1;

    vtxfs_inode_t di;
    uint8_t *p = (uint8_t *)&di;
    for (int i = 0; i < VTXFS_INODE_SIZE; i++) p[i] = 0;
    di.type   = VFS_FILE;
    di.size   = 0;
    di.blocks = 0;
    ino_write((uint32_t)ino, &di);

    if (dir_add(vn->inode, name, (uint32_t)ino) != 0) {
        ino_free((uint32_t)ino);
        return -1;
    }
    return 0;
}

/* --- unlink ------------------------------------------------------------- */
static void free_all_blocks(vtxfs_inode_t *di) {
    /* Direct blocks */
    for (uint32_t i = 0; i < di->blocks && i < VTXFS_MAX_DIRECT; i++)
        if (di->direct[i]) blk_free(di->direct[i]);

    /* Single indirect */
    if (di->indirect) {
        uint32_t ibuf[128];
        if (blk_read(di->indirect, ibuf) == 0) {
            uint32_t cnt = (di->blocks > VTXFS_MAX_DIRECT)
                             ? di->blocks - VTXFS_MAX_DIRECT : 0;
            if (cnt > 128) cnt = 128;
            for (uint32_t i = 0; i < cnt; i++)
                if (ibuf[i]) blk_free(ibuf[i]);
        }
        blk_free(di->indirect);
    }

    /* Double indirect */
    if (di->dbl_indirect) {
        uint32_t d1[128];
        if (blk_read(di->dbl_indirect, d1) == 0) {
            /* Remaining blocks after direct + single indirect */
            uint32_t used = (di->blocks > VTXFS_MAX_DIRECT + 128)
                             ? di->blocks - VTXFS_MAX_DIRECT - 128 : 0;
            for (uint32_t s = 0; s < 128 && used > 0; s++) {
                if (d1[s] == 0) break;
                uint32_t d2[128];
                if (blk_read(d1[s], d2) == 0) {
                    uint32_t n = (used > 128) ? 128 : used;
                    for (uint32_t j = 0; j < n; j++)
                        if (d2[j]) blk_free(d2[j]);
                    used -= n;
                }
                blk_free(d1[s]);
            }
        }
        blk_free(di->dbl_indirect);
    }
}

static int vtxfs_unlink_cb(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return -1;

    vfs_node_t *child = vtxfs_finddir_cb(vn, name);
    if (!child) return -1;

    /* Директория должна быть пустой */
    if (child->type == VFS_DIR) {
        vtxfs_inode_t ci;
        if (ino_read(child->inode, &ci) != 0) return -1;
        if (ci.size > 0) return -1;
    }

    vtxfs_inode_t ci;
    if (ino_read(child->inode, &ci) != 0) return -1;
    free_all_blocks(&ci);

    dir_remove(vn->inode, name);
    ino_free(child->inode);

    /* Очищаем из кэша */
    node_uncache(child);
    kfree(child);
    return 0;
}

/* ========================================================================= *
 *  mkfs — форматирование раздела                                           *
 * ========================================================================= */

int vortexfs_mkfs(uint8_t ata_slave, uint32_t start_lba,
                  uint32_t total_sectors)
{
    g_slave     = ata_slave;
    g_start_lba = start_lba;

    /* Вычисляем layout */
    uint32_t ibm_sec = (VTXFS_MAX_INODES / 8 + 511) / 512;    /* 1  */
    uint32_t bbm_sec = (total_sectors    / 8 + 511) / 512;     /* ~8 */
    uint32_t itb_sec = ((uint32_t)VTXFS_MAX_INODES
                         * VTXFS_INODE_SIZE + 511) / 512;       /* 512 */

    uint32_t ibm_start = 1;
    uint32_t bbm_start = ibm_start + ibm_sec;
    uint32_t itb_start = bbm_start + bbm_sec;
    uint32_t dat_start = itb_start + itb_sec;

    if (dat_start >= total_sectors) return -1;

    uint32_t data_blocks = total_sectors - dat_start;

    /* --- Суперблок --- */
    uint8_t *p = (uint8_t *)&g_sb;
    for (uint32_t i = 0; i < sizeof(g_sb); i++) p[i] = 0;

    g_sb.magic              = VTXFS_MAGIC;
    g_sb.version            = VTXFS_VERSION;
    g_sb.total_blocks       = data_blocks;
    g_sb.total_inodes       = VTXFS_MAX_INODES;
    g_sb.free_blocks        = data_blocks - 2;  /* block 0 reserved + block 1 root */
    g_sb.free_inodes        = VTXFS_MAX_INODES - 1;
    g_sb.inode_bitmap_start = ibm_start;
    g_sb.block_bitmap_start = bbm_start;
    g_sb.inode_table_start  = itb_start;
    g_sb.data_start         = dat_start;
    g_sb.root_inode         = 0;

    if (sb_write() != 0) return -1;

    /* --- Обнуляем метаданные --- */
    uint8_t zero[512];
    for (int i = 0; i < 512; i++) zero[i] = 0;
    for (uint32_t s = 1; s < dat_start; s++)
        if (wr_sector(s, zero) != 0) return -1;

    /* --- Резервируем block 0 (sentinel) --- */
    bm_set(g_sb.block_bitmap_start, 0, 1);

    /* --- Root inode (inode 0) --- */
    bm_set(g_sb.inode_bitmap_start, 0, 1);

    /* Выделяем block 1 для данных root-каталога */
    bm_set(g_sb.block_bitmap_start, 1, 1);

    vtxfs_inode_t root;
    p = (uint8_t *)&root;
    for (int i = 0; i < VTXFS_INODE_SIZE; i++) p[i] = 0;
    root.type      = VFS_DIR;
    root.size      = 0;
    root.blocks    = 1;
    root.direct[0] = 1;  /* data block 1 */
    ino_write(0, &root);

    /* Обнуляем первый блок данных root-каталога */
    wr_sector(g_sb.data_start + 1, zero);

    fb_puts("[VortexFS] mkfs OK (");
    uint32_t n = g_sb.free_blocks;
    char nbuf[12]; int i = 10; nbuf[11] = 0;
    if (!n) nbuf[i--] = '0';
    else while (n) { nbuf[i--] = '0' + n%10; n /= 10; }
    fb_puts(&nbuf[i+1]);
    fb_puts(" blocks free)\n");

    return 0;
}

/* ========================================================================= *
 *  init — монтирование существующего раздела                                *
 * ========================================================================= */

int vortexfs_init(uint8_t ata_slave, uint32_t start_lba) {
    g_slave     = ata_slave;
    g_start_lba = start_lba;
    g_mounted   = 0;
    node_cache_n = 0;

    if (sb_read() != 0) {
        fb_puts("[VortexFS] No valid superblock\n");
        return -1;
    }

    /* Заполняем таблицу VFS операций */
    vtxfs_ops.open    = vtxfs_open_cb;
    vtxfs_ops.close   = vtxfs_close_cb;
    vtxfs_ops.read    = vtxfs_read_cb;
    vtxfs_ops.write   = vtxfs_write_cb;
    vtxfs_ops.finddir = vtxfs_finddir_cb;
    vtxfs_ops.readdir = vtxfs_readdir_cb;
    vtxfs_ops.mkdir   = vtxfs_mkdir_cb;
    vtxfs_ops.create  = vtxfs_create_cb;
    vtxfs_ops.unlink  = vtxfs_unlink_cb;

    g_root = node_make(g_sb.root_inode);
    if (!g_root) {
        fb_puts("[VortexFS] Failed to read root inode\n");
        return -1;
    }
    g_root->name[0] = '/';
    g_root->name[1] = 0;
    g_mounted = 1;

    fb_puts("[VortexFS] Mounted (");
    uint32_t n = g_sb.free_blocks;
    char nbuf[12]; int i = 10; nbuf[11] = 0;
    if (!n) nbuf[i--] = '0';
    else while (n) { nbuf[i--] = '0' + n%10; n /= 10; }
    fb_puts(&nbuf[i+1]);
    fb_puts("/");
    n = g_sb.total_blocks;
    i = 10;
    if (!n) nbuf[i--] = '0';
    else while (n) { nbuf[i--] = '0' + n%10; n /= 10; }
    fb_puts(&nbuf[i+1]);
    fb_puts(" blocks free)\n");

    return 0;
}

vfs_node_t *vortexfs_get_root(void) {
    return g_root;
}
