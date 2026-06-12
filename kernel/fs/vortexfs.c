/* =============================================================================
 * VortexOS — kernel/fs/vortexfs.c
 * VortexFS — нативная файловая система VortexOS.
 *
 * Работает поверх ATA PIO. Поддерживает файлы, каталоги, mkdir, create,
 * unlink, read, write, finddir, readdir, права, журнал метаданных.
 *
 * v3: логический блок 4096 байт (8 секторов), инод 128 байт (size 64 бит,
 * triple indirect) — файлы до ~4 ТБ. v1/v2 (блок 512, инод 64 байта)
 * монтируются как раньше: все параметры выбираются при mount по version.
 *
 * Блок 0 зарезервирован как sentinel — в direct[]/indirect значение 0
 * означает «блок не выделен».
 *
 * ВНИМАНИЕ (как и раньше): код НЕ реентерабелен — статические буферы
 * (g_dirbuf/g_datbuf/g_ind*) общие. Все вызовы идут под глобальной
 * последовательностью VFS, параллельных обращений нет.
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

/* --- параметры формата (заполняются при mount/mkfs по version) --- */
static uint32_t g_ver;     /* версия ФС (1/2/3)                             */
static uint32_t g_bs;      /* размер логического блока (512 или 4096)       */
static uint32_t g_spb;     /* секторов в блоке (1 или 8)                    */
static uint32_t g_ppb;     /* указателей (uint32) в блоке (128 или 1024)    */
static uint32_t g_dpb;     /* dirent'ов в блоке (8 или 64)                  */
static uint32_t g_ino_sz;  /* размер инода на диске (64 или 128)            */

static vfs_ops_t     vtxfs_ops;        /* единая таблица VFS-операций      */
static vfs_node_t   *g_root;           /* корневая VFS нода                */

/* Статические буферы — не на стеке, экономим kernel stack.
 * sec_tmp/sec_tmp2 — метаданные посекторно (bitmap, иноды, суперблок).
 * g_dirbuf — блоки каталогов, g_datbuf — блоки данных файлов,
 * g_ind1..3 — таблицы указателей (3 уровня могут быть нужны одновременно),
 * g_zero — вечно нулевой блок (BSS) для зануления свежих блоков. */
static uint8_t  sec_tmp[512];
static uint8_t  sec_tmp2[512];
static uint8_t  g_dirbuf[VTXFS_BS_MAX];
static uint8_t  g_datbuf[VTXFS_BS_MAX];
static uint32_t g_ind1[VTXFS_PPB_MAX];
static uint32_t g_ind2[VTXFS_PPB_MAX];
static uint32_t g_ind3[VTXFS_PPB_MAX];
static const uint8_t g_zero[VTXFS_BS_MAX]; /* BSS → нули, никогда не пишем */

/* Простой кэш VFS-нод (чтобы finddir не плодил дубликаты) */
#define NODE_CACHE_MAX 256
static vfs_node_t *node_cache[NODE_CACHE_MAX];
static uint32_t    node_cache_n;

/* ========================================================================= *
 *  Доступ к секторам (со смещением раздела)                                 *
 * ========================================================================= */

/* --- состояние журнала (metadata WAL, см. vortexfs.h) --- */
static uint8_t  g_jrn_on;        /* журнал включён (v2+ и journal_start)     */
static uint32_t g_jrn_depth;     /* вложенность транзакций                   */
static uint32_t g_jrn_seq;       /* счётчик транзакций                       */
static uint8_t  g_jrn_bypass;    /* пишем сам журнал — не перехватывать      */
static uint32_t g_jrn_n;         /* захвачено секторов                       */
static uint32_t g_jrn_lba[VTXFS_JRN_MAX_CAP];
static uint8_t  g_jrn_buf[VTXFS_JRN_MAX_CAP][512];   /* 15 KiB BSS */

static int jrn_capture(uint32_t rel, const void *buf);

static int rd_sector(uint32_t rel, void *buf) {
    /* read-your-writes: внутри txn захваченные сектора ещё не на диске */
    if (g_jrn_on && g_jrn_depth && !g_jrn_bypass)
        for (uint32_t i = 0; i < g_jrn_n; i++)
            if (g_jrn_lba[i] == rel) {
                const uint8_t *s = g_jrn_buf[i];
                uint8_t *d = (uint8_t *)buf;
                for (int k = 0; k < 512; k++) d[k] = s[k];
                return 0;
            }
    return ata_read_sector(g_slave, g_start_lba + rel, buf);
}
static int wr_sector(uint32_t rel, const void *buf) {
    /* внутри txn запись откладывается в журнальный буфер до commit */
    if (g_jrn_on && g_jrn_depth && !g_jrn_bypass)
        return jrn_capture(rel, buf);
    return ata_write(g_slave, g_start_lba + rel, 1, (const uint16_t *)buf);
}

/* ========================================================================= *
 *  Суперблок                                                                *
 * ========================================================================= */

static int sb_read(void) {
    if (rd_sector(0, &g_sb) != 0) return -1;
    /* v1 (без журнала), v2 (журнал) и v3 (4К блоки) монтируются все */
    return (g_sb.magic == VTXFS_MAGIC &&
            g_sb.version >= 1 && g_sb.version <= VTXFS_VERSION) ? 0 : -1;
}
static int sb_write(void) { return wr_sector(0, &g_sb); }

/* Выставить рантайм-параметры формата по версии суперблока. 0 = ok. */
static int fmt_params_set(void) {
    g_ver = g_sb.version;
    if (g_ver <= 2) {
        g_bs = VTXFS_BS_V2;
        g_ino_sz = VTXFS_INO_SZ_V2;
    } else {
        /* v3: размер блока из суперблока (валидируем жёстко) */
        if (g_sb.block_size != VTXFS_BS_V3) return -1;
        g_bs = VTXFS_BS_V3;
        g_ino_sz = VTXFS_INO_SZ_V3;
    }
    g_spb = g_bs / 512;
    g_ppb = g_bs / 4;
    g_dpb = g_bs / VTXFS_DIRENT_SIZE;
    return 0;
}

/* ========================================================================= *
 *  Журнал — write-ahead log для метаданных                                  *
 * ========================================================================= */

/* Сбросить накопленную транзакцию: журнал (hdr + payload + commit) →
 * применение in-place → обнуление заголовка. WAL-порядок гарантирует:
 * упали до commit-маркера — txn отброшена, после — реиграна при mount. */
static uint8_t jrn_tmp[512];   /* свой буфер: sec_tmp2 может быть buf вызывающего */

static int jrn_flush(void) {
    if (!g_jrn_n) return 0;
    uint32_t js = g_sb.journal_start;
    g_jrn_bypass = 1;

    vtxfs_jrn_hdr_t hdr;
    uint8_t *p = (uint8_t *)&hdr;
    for (uint32_t i = 0; i < sizeof(hdr); i++) p[i] = 0;
    hdr.magic = VTXFS_JRN_HDR_MAGIC;
    hdr.seq   = ++g_jrn_seq;
    hdr.count = g_jrn_n;
    hdr.checksum = hdr.seq + hdr.count;
    for (uint32_t i = 0; i < g_jrn_n; i++) {
        hdr.lba[i] = g_jrn_lba[i];
        hdr.checksum += g_jrn_lba[i];
    }
    for (int i = 0; i < 512; i++) jrn_tmp[i] = 0;
    uint8_t *h = (uint8_t *)&hdr;
    for (uint32_t i = 0; i < sizeof(hdr); i++) jrn_tmp[i] = h[i];

    int rc = wr_sector(js, jrn_tmp);                          /* 1. header  */
    for (uint32_t i = 0; i < g_jrn_n && rc == 0; i++)          /* 2. payload */
        rc = wr_sector(js + 1 + i, g_jrn_buf[i]);
    if (rc == 0) {                                             /* 3. commit  */
        for (int i = 0; i < 512; i++) jrn_tmp[i] = 0;
        ((uint32_t *)jrn_tmp)[0] = VTXFS_JRN_CMT_MAGIC;
        ((uint32_t *)jrn_tmp)[1] = g_jrn_seq;
        rc = wr_sector(js + 1 + g_jrn_n, jrn_tmp);
    }
    for (uint32_t i = 0; i < g_jrn_n && rc == 0; i++)          /* 4. in-place */
        rc = wr_sector(g_jrn_lba[i], g_jrn_buf[i]);
    if (rc == 0) {                                             /* 5. done    */
        for (int i = 0; i < 512; i++) jrn_tmp[i] = 0;
        rc = wr_sector(js, jrn_tmp);
    }

    g_jrn_bypass = 0;
    g_jrn_n = 0;
    return rc;
}

/* Захватить сектор в текущую txn (дедуп по LBA). Переполнение буфера —
 * chained flush: предыдущая пачка коммитится, txn продолжается новой. */
static int jrn_capture(uint32_t rel, const void *buf) {
    uint32_t slot = g_jrn_n;
    for (uint32_t i = 0; i < g_jrn_n; i++)
        if (g_jrn_lba[i] == rel) { slot = i; break; }
    if (slot == g_jrn_n) {
        if (g_jrn_n == VTXFS_JRN_MAX_CAP) {
            if (jrn_flush() != 0) return -1;
            slot = 0;
        }
        g_jrn_lba[slot] = rel;
        g_jrn_n = slot + 1;
    }
    const uint8_t *s = (const uint8_t *)buf;
    for (int k = 0; k < 512; k++) g_jrn_buf[slot][k] = s[k];
    return 0;
}

/* Транзакции с вложенностью: реально сбрасывает только внешний commit. */
static void jrn_begin(void) {
    if (g_jrn_on) g_jrn_depth++;
}
static int jrn_commit(void) {
    if (!g_jrn_on || !g_jrn_depth) return 0;
    if (--g_jrn_depth == 0) return jrn_flush();
    return 0;
}

/* Replay при mount: валидный header + commit-маркер той же seq → применяем
 * payload (идемпотентно), затем стираем заголовок. Без commit — отбрасываем. */
static void jrn_replay(void) {
    uint32_t js = g_sb.journal_start;
    vtxfs_jrn_hdr_t hdr;
    if (rd_sector(js, sec_tmp) != 0) return;
    uint8_t *h = (uint8_t *)&hdr;
    for (uint32_t i = 0; i < sizeof(hdr); i++) h[i] = sec_tmp[i];
    if (hdr.magic != VTXFS_JRN_HDR_MAGIC) return;

    uint32_t sum = hdr.seq + hdr.count;
    for (uint32_t i = 0; i < hdr.count && i < VTXFS_JRN_MAX_CAP; i++)
        sum += hdr.lba[i];
    int valid = (hdr.count > 0 && hdr.count <= VTXFS_JRN_MAX_CAP &&
                 hdr.checksum == sum);

    if (valid && rd_sector(js + 1 + hdr.count, sec_tmp) == 0 &&
        ((uint32_t *)sec_tmp)[0] == VTXFS_JRN_CMT_MAGIC &&
        ((uint32_t *)sec_tmp)[1] == hdr.seq) {
        fb_puts("[VortexFS] journal: replaying committed txn\n");
        for (uint32_t i = 0; i < hdr.count; i++) {
            if (rd_sector(js + 1 + i, sec_tmp) != 0) return;
            wr_sector(hdr.lba[i], sec_tmp);
        }
        /* суперблок могли реиграть — перечитаем кэш */
        rd_sector(0, &g_sb);
    } else if (valid) {
        fb_puts("[VortexFS] journal: dropping uncommitted txn\n");
    }
    g_jrn_seq = hdr.seq;
    for (int i = 0; i < 512; i++) sec_tmp[i] = 0;
    wr_sector(js, sec_tmp);
}

/* ========================================================================= *
 *  Bitmap-операции (гранулярность — сектор, формат одинаков для всех версий)*
 * ========================================================================= */

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
 *  Inode I/O — v3: 4 инода/сектор (128 байт), v1/v2: 8 инодов/сектор (64)  *
 *  In-memory формат един (= v3 on-disk); v1/v2 конвертируются на границе.  *
 * ========================================================================= */

static int ino_read(uint32_t ino, vtxfs_inode_t *out) {
    uint32_t per = 512 / g_ino_sz;
    uint32_t sec = g_sb.inode_table_start + ino / per;
    uint32_t off = (ino % per) * g_ino_sz;
    if (rd_sector(sec, sec_tmp) != 0) return -1;

    uint8_t *d = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) d[i] = 0;

    if (g_ver >= 3) {
        const uint8_t *s = sec_tmp + off;
        for (uint32_t i = 0; i < VTXFS_INO_SZ_V3; i++) d[i] = s[i];
    } else {
        /* legacy 64-байтный инод → in-memory */
        vtxfs_inode_v2_t L;
        const uint8_t *s = sec_tmp + off;
        uint8_t *ld = (uint8_t *)&L;
        for (uint32_t i = 0; i < VTXFS_INO_SZ_V2; i++) ld[i] = s[i];
        out->type   = L.type;
        out->blocks = L.blocks;
        out->size   = L.size;
        for (int i = 0; i < VTXFS_MAX_DIRECT; i++) out->direct[i] = L.direct[i];
        out->indirect      = L.indirect;
        out->dbl_indirect  = L.dbl_indirect;
        out->trpl_indirect = 0;
        out->mode = L.mode;
        out->uid  = L.uid;
        out->gid  = L.gid;
        out->nlinks = 1;
    }
    return 0;
}

static int ino_write(uint32_t ino, const vtxfs_inode_t *in) {
    uint32_t per = 512 / g_ino_sz;
    uint32_t sec = g_sb.inode_table_start + ino / per;
    uint32_t off = (ino % per) * g_ino_sz;
    if (rd_sector(sec, sec_tmp) != 0) return -1;

    if (g_ver >= 3) {
        uint8_t *d = sec_tmp + off;
        const uint8_t *s = (const uint8_t *)in;
        for (uint32_t i = 0; i < VTXFS_INO_SZ_V3; i++) d[i] = s[i];
    } else {
        /* in-memory → legacy (size усечётся до 32 бит, trpl потеряется —
         * на v1/v2 они и не используются: set_blk не даёт уйти в triple) */
        vtxfs_inode_v2_t L;
        uint8_t *lp = (uint8_t *)&L;
        for (uint32_t i = 0; i < sizeof(L); i++) lp[i] = 0;
        L.type   = in->type;
        L.size   = (uint32_t)in->size;
        L.blocks = in->blocks;
        for (int i = 0; i < VTXFS_MAX_DIRECT; i++) L.direct[i] = in->direct[i];
        L.indirect     = in->indirect;
        L.dbl_indirect = in->dbl_indirect;
        L.mode = in->mode;
        L.uid  = in->uid;
        L.gid  = in->gid;
        uint8_t *d = sec_tmp + off;
        for (uint32_t i = 0; i < VTXFS_INO_SZ_V2; i++) d[i] = lp[i];
    }
    return wr_sector(sec, sec_tmp);
}

static int ino_alloc(void) {
    jrn_begin();   /* bitmap + суперблок — одной транзакцией */
    int n = bm_alloc(g_sb.inode_bitmap_start, g_sb.total_inodes);
    if (n >= 0) { g_sb.free_inodes--; sb_write(); }
    jrn_commit();
    return n;
}

static void ino_free(uint32_t ino) {
    jrn_begin();
    bm_set(g_sb.inode_bitmap_start, ino, 0);
    g_sb.free_inodes++;
    sb_write();
    jrn_commit();
}

/* ========================================================================= *
 *  Блоки данных (логический блок = g_spb секторов)                          *
 * ========================================================================= */

static int blk_read(uint32_t blk, void *buf) {
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t s = 0; s < g_spb; s++)
        if (rd_sector(g_sb.data_start + blk * g_spb + s, d + s * 512) != 0)
            return -1;
    return 0;
}
static int blk_write(uint32_t blk, const void *buf) {
    const uint8_t *s8 = (const uint8_t *)buf;
    for (uint32_t s = 0; s < g_spb; s++)
        if (wr_sector(g_sb.data_start + blk * g_spb + s, s8 + s * 512) != 0)
            return -1;
    return 0;
}

static int blk_alloc(void) {
    jrn_begin();
    int n = bm_alloc(g_sb.block_bitmap_start, g_sb.total_blocks);
    if (n >= 0) {
        g_sb.free_blocks--;
        sb_write();
        /* Обнуляем свежий блок (g_zero — вечно нулевой BSS-буфер) */
        blk_write((uint32_t)n, g_zero);
    }
    jrn_commit();
    return n;
}

static void blk_free(uint32_t blk) {
    jrn_begin();
    bm_set(g_sb.block_bitmap_start, blk, 0);
    g_sb.free_blocks++;
    sb_write();
    jrn_commit();
}

/* Получить номер блока по логическому индексу внутри файла.
 * 0 = «не выделен» (sentinel).
 *
 * Уровни (P = g_ppb: 128 на v1/v2, 1024 на v3):
 *   [0 .. 9]                — direct
 *   [10 .. 10+P-1]          — indirect
 *   [.. +P^2]               — double-indirect
 *   [.. +P^3]               — triple-indirect (только v3)                   */
static uint32_t inode_get_blk(vtxfs_inode_t *di, uint32_t idx) {
    if (idx < VTXFS_MAX_DIRECT)
        return di->direct[idx];

    uint32_t off = idx - VTXFS_MAX_DIRECT;
    uint32_t P = g_ppb;

    /* Single indirect */
    if (off < P) {
        if (di->indirect == 0) return 0;
        if (blk_read(di->indirect, g_ind1) != 0) return 0;
        return g_ind1[off];
    }
    off -= P;

    /* Double indirect */
    if (off < P * P) {
        if (di->dbl_indirect == 0) return 0;
        if (blk_read(di->dbl_indirect, g_ind1) != 0) return 0;
        uint32_t slot = off / P;
        if (g_ind1[slot] == 0) return 0;
        if (blk_read(g_ind1[slot], g_ind2) != 0) return 0;
        return g_ind2[off % P];
    }
    off -= P * P;

    /* Triple indirect (v3) */
    if (g_ver < 3 || di->trpl_indirect == 0) return 0;
    /* off < P^3 гарантировано вызывающим (P^3 = 2^30 при P=1024) */
    if (blk_read(di->trpl_indirect, g_ind1) != 0) return 0;
    uint32_t s1  = off / (P * P);
    uint32_t rem = off % (P * P);
    if (s1 >= P || g_ind1[s1] == 0) return 0;
    if (blk_read(g_ind1[s1], g_ind2) != 0) return 0;
    uint32_t s2 = rem / P;
    if (g_ind2[s2] == 0) return 0;
    if (blk_read(g_ind2[s2], g_ind3) != 0) return 0;
    return g_ind3[rem % P];
}

/* Установить блок по логическому индексу, при необходимости выделяя
 * indirect / double / triple блоки-указатели. */
static int inode_set_blk(vtxfs_inode_t *di, uint32_t idx, uint32_t blk) {
    if (idx < VTXFS_MAX_DIRECT) {
        di->direct[idx] = blk;
        return 0;
    }

    uint32_t off = idx - VTXFS_MAX_DIRECT;
    uint32_t P = g_ppb;

    /* Single indirect */
    if (off < P) {
        if (di->indirect == 0) {
            int ib = blk_alloc();
            if (ib < 0) return -1;
            di->indirect = (uint32_t)ib;
        }
        if (blk_read(di->indirect, g_ind1) != 0) return -1;
        g_ind1[off] = blk;
        return blk_write(di->indirect, g_ind1);
    }
    off -= P;

    /* Double indirect */
    if (off < P * P) {
        if (di->dbl_indirect == 0) {
            int db = blk_alloc();
            if (db < 0) return -1;
            di->dbl_indirect = (uint32_t)db;
        }
        if (blk_read(di->dbl_indirect, g_ind1) != 0) return -1;
        uint32_t slot = off / P;
        if (g_ind1[slot] == 0) {
            int nb = blk_alloc();
            if (nb < 0) return -1;
            /* blk_alloc мог переписать g_ind1? Нет: он пишет только g_zero,
             * bitmap (sec_tmp) и суперблок. Перечитаем на всякий случай —
             * дёшево и убирает класс ошибок при будущих правках. */
            if (blk_read(di->dbl_indirect, g_ind1) != 0) return -1;
            g_ind1[slot] = (uint32_t)nb;
            if (blk_write(di->dbl_indirect, g_ind1) != 0) return -1;
        }
        if (blk_read(g_ind1[slot], g_ind2) != 0) return -1;
        g_ind2[off % P] = blk;
        return blk_write(g_ind1[slot], g_ind2);
    }
    off -= P * P;

    /* Triple indirect — только v3 (v1/v2 формат его не хранит) */
    if (g_ver < 3) return -1;
    if (off >= 0x40000000u) return -1;   /* P^3 при P=1024 */

    if (di->trpl_indirect == 0) {
        int tb = blk_alloc();
        if (tb < 0) return -1;
        di->trpl_indirect = (uint32_t)tb;
    }
    if (blk_read(di->trpl_indirect, g_ind1) != 0) return -1;
    uint32_t s1  = off / (P * P);
    uint32_t rem = off % (P * P);
    if (s1 >= P) return -1;
    if (g_ind1[s1] == 0) {
        int nb = blk_alloc();
        if (nb < 0) return -1;
        if (blk_read(di->trpl_indirect, g_ind1) != 0) return -1;
        g_ind1[s1] = (uint32_t)nb;
        if (blk_write(di->trpl_indirect, g_ind1) != 0) return -1;
    }
    if (blk_read(g_ind1[s1], g_ind2) != 0) return -1;
    uint32_t s2 = rem / P;
    if (g_ind2[s2] == 0) {
        int nb = blk_alloc();
        if (nb < 0) return -1;
        if (blk_read(g_ind1[s1], g_ind2) != 0) return -1;
        g_ind2[s2] = (uint32_t)nb;
        if (blk_write(g_ind1[s1], g_ind2) != 0) return -1;
    }
    if (blk_read(g_ind2[s2], g_ind3) != 0) return -1;
    g_ind3[rem % P] = blk;
    return blk_write(g_ind2[s2], g_ind3);
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
    n->size    = (uint32_t)di.size;   /* VFS пока 32-битный по размеру */
    n->ops     = &vtxfs_ops;
    n->fs_data = 0;
    n->flags   = VFS_FL_CACHED;   /* нода в node_cache — kfree запрещён */
    /* Права: mode==0 — инод из старого образа (v1 без прав) → дефолт */
    n->mode    = di.mode ? di.mode
                         : ((di.type == VFS_DIR) ? VTXFS_DEF_DIR_MODE
                                                 : VTXFS_DEF_FILE_MODE);
    n->uid     = di.uid;
    n->gid     = di.gid;

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

        if (blk_read(b, g_dirbuf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)g_dirbuf;
        for (uint32_t e = 0; e < g_dpb; e++) {
            if (ent[e].name[0] == 0) {
                str_copy(ent[e].name, name, VTXFS_NAME_MAX + 1);
                ent[e].inode = child_ino;
                blk_write(b, g_dirbuf);
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

    for (uint32_t i = 0; i < g_bs; i++) g_dirbuf[i] = 0;
    vtxfs_dirent_t *ent = (vtxfs_dirent_t *)g_dirbuf;
    str_copy(ent[0].name, name, VTXFS_NAME_MAX + 1);
    ent[0].inode = child_ino;
    blk_write((uint32_t)nb, g_dirbuf);

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

        if (blk_read(b, g_dirbuf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)g_dirbuf;
        for (uint32_t e = 0; e < g_dpb; e++) {
            if (ent[e].name[0] && str_eq(ent[e].name, name)) {
                ent[e].name[0] = 0;
                ent[e].inode   = 0;
                blk_write(b, g_dirbuf);
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

    if ((uint64_t)off >= di.size) return 0;
    if ((uint64_t)off + sz > di.size) sz = (uint32_t)(di.size - off);

    uint32_t done = 0;
    while (done < sz) {
        uint32_t cur   = off + done;
        uint32_t bi    = cur / g_bs;
        uint32_t boff  = cur % g_bs;
        uint32_t chunk = g_bs - boff;
        if (chunk > sz - done) chunk = sz - done;

        if (bi >= di.blocks) break;
        uint32_t b = inode_get_blk(&di, bi);
        if (b == 0) break;

        if (blk_read(b, g_datbuf) != 0) break;
        for (uint32_t i = 0; i < chunk; i++) buf[done + i] = g_datbuf[boff + i];
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
        uint32_t bi    = cur / g_bs;
        uint32_t boff  = cur % g_bs;
        uint32_t chunk = g_bs - boff;
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

        if (boff > 0 || chunk < g_bs)
            blk_read(b, g_datbuf);           /* частичная запись → читаем */

        for (uint32_t i = 0; i < chunk; i++) g_datbuf[boff + i] = buf[done + i];
        if (blk_write(b, g_datbuf) != 0) break;
        done += chunk;
    }
out:
    {
        uint64_t end = (uint64_t)off + done;
        if (end > di.size) di.size = end;
        ino_write(vn->inode, &di);
        vn->size = (uint32_t)di.size;
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

        if (blk_read(b, g_dirbuf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)g_dirbuf;
        for (uint32_t e = 0; e < g_dpb; e++) {
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

        if (blk_read(b, g_dirbuf) != 0) continue;

        vtxfs_dirent_t *ent = (vtxfs_dirent_t *)g_dirbuf;
        for (uint32_t e = 0; e < g_dpb; e++) {
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
static int vtxfs_mkdir_inner(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return -1;
    if (vtxfs_finddir_cb(vn, name)) return -1; /* уже есть */

    int ino = ino_alloc();
    if (ino < 0) return -1;

    vtxfs_inode_t di;
    uint8_t *p = (uint8_t *)&di;
    for (uint32_t i = 0; i < sizeof(di); i++) p[i] = 0;
    di.type   = VFS_DIR;
    di.size   = 0;
    di.blocks = 0;
    di.mode   = VTXFS_DEF_DIR_MODE;
    di.uid    = (uint8_t)vfs_cur_uid();
    di.gid    = (uint8_t)vfs_cur_gid();
    di.nlinks = 1;
    ino_write((uint32_t)ino, &di);

    if (dir_add(vn->inode, name, (uint32_t)ino) != 0) {
        ino_free((uint32_t)ino);
        return -1;
    }
    return 0;
}

/* --- create (файл) ------------------------------------------------------ */
static int vtxfs_create_inner(vfs_node_t *vn, const char *name) {
    if (!vn || vn->type != VFS_DIR) return -1;
    if (vtxfs_finddir_cb(vn, name)) return -1;

    int ino = ino_alloc();
    if (ino < 0) return -1;

    vtxfs_inode_t di;
    uint8_t *p = (uint8_t *)&di;
    for (uint32_t i = 0; i < sizeof(di); i++) p[i] = 0;
    di.type   = VFS_FILE;
    di.size   = 0;
    di.blocks = 0;
    di.mode   = VTXFS_DEF_FILE_MODE;
    di.uid    = (uint8_t)vfs_cur_uid();
    di.gid    = (uint8_t)vfs_cur_gid();
    di.nlinks = 1;
    ino_write((uint32_t)ino, &di);

    if (dir_add(vn->inode, name, (uint32_t)ino) != 0) {
        ino_free((uint32_t)ino);
        return -1;
    }
    return 0;
}

/* --- unlink ------------------------------------------------------------- */
static void free_all_blocks(vtxfs_inode_t *di) {
    uint32_t P = g_ppb;

    /* Direct blocks */
    for (uint32_t i = 0; i < di->blocks && i < VTXFS_MAX_DIRECT; i++)
        if (di->direct[i]) blk_free(di->direct[i]);

    /* Single indirect */
    if (di->indirect) {
        if (blk_read(di->indirect, g_ind1) == 0) {
            uint32_t cnt = (di->blocks > VTXFS_MAX_DIRECT)
                             ? di->blocks - VTXFS_MAX_DIRECT : 0;
            if (cnt > P) cnt = P;
            for (uint32_t i = 0; i < cnt; i++)
                if (g_ind1[i]) blk_free(g_ind1[i]);
        }
        blk_free(di->indirect);
    }

    /* Double indirect */
    if (di->dbl_indirect) {
        if (blk_read(di->dbl_indirect, g_ind1) == 0) {
            uint32_t used = (di->blocks > VTXFS_MAX_DIRECT + P)
                             ? di->blocks - VTXFS_MAX_DIRECT - P : 0;
            if (used > P * P) used = P * P;
            for (uint32_t s = 0; s < P && used > 0; s++) {
                if (g_ind1[s] == 0) break;
                if (blk_read(g_ind1[s], g_ind2) == 0) {
                    uint32_t n = (used > P) ? P : used;
                    for (uint32_t j = 0; j < n; j++)
                        if (g_ind2[j]) blk_free(g_ind2[j]);
                    used -= (used > P) ? P : used;
                } else {
                    used -= (used > P) ? P : used;
                }
                blk_free(g_ind1[s]);
            }
        }
        blk_free(di->dbl_indirect);
    }

    /* Triple indirect (v3). g_ind1 = таблица 1-го уровня, перечитывается
     * на каждый внешний слот, т.к. вложенные уровни используют g_ind2/g_ind3. */
    if (g_ver >= 3 && di->trpl_indirect) {
        uint32_t used = (di->blocks > VTXFS_MAX_DIRECT + P + P * P)
                         ? di->blocks - VTXFS_MAX_DIRECT - P - P * P : 0;
        for (uint32_t s1 = 0; s1 < P && used > 0; s1++) {
            if (blk_read(di->trpl_indirect, g_ind1) != 0) break;
            uint32_t t2 = g_ind1[s1];
            if (t2 == 0) break;
            for (uint32_t s2 = 0; s2 < P && used > 0; s2++) {
                if (blk_read(t2, g_ind2) != 0) break;
                uint32_t t3 = g_ind2[s2];
                if (t3 == 0) break;
                if (blk_read(t3, g_ind3) == 0) {
                    uint32_t n = (used > P) ? P : used;
                    for (uint32_t j = 0; j < n; j++)
                        if (g_ind3[j]) blk_free(g_ind3[j]);
                    used -= n;
                } else {
                    used -= (used > P) ? P : used;
                }
                blk_free(t3);
            }
            blk_free(t2);
        }
        blk_free(di->trpl_indirect);
    }
}

static int vtxfs_unlink_inner(vfs_node_t *vn, const char *name) {
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

/* --- chmod / chown -------------------------------------------------------
 * Политика (владелец/root) проверена в vfs_chmod/vfs_chown — здесь только
 * запись инода + обновление кэшированной ноды. */
static int vtxfs_chmod_inner(vfs_node_t *vn, uint32_t mode) {
    if (!vn) return -1;
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return -1;
    di.mode = (uint16_t)(mode & 07777);
    if (ino_write(vn->inode, &di) != 0) return -1;
    vn->mode = di.mode;
    return 0;
}

static int vtxfs_chown_inner(vfs_node_t *vn, uint32_t uid, uint32_t gid) {
    if (!vn || uid > 255 || gid > 255) return -1;  /* в иноде по байту */
    vtxfs_inode_t di;
    if (ino_read(vn->inode, &di) != 0) return -1;
    di.uid = (uint8_t)uid;
    di.gid = (uint8_t)gid;
    if (ino_write(vn->inode, &di) != 0) return -1;
    vn->uid = uid;
    vn->gid = gid;
    return 0;
}


/* ========================================================================= *
 *  Транзакционные обёртки: вся структурная операция — одна journal-txn      *
 * ========================================================================= */
#define VTXFS_TXN_CB(rtype, op, sig, callargs)            \
    static rtype vtxfs_##op##_cb sig {                    \
        jrn_begin();                                      \
        rtype r = vtxfs_##op##_inner callargs;            \
        if (jrn_commit() != 0) r = -1;                    \
        return r;                                         \
    }

VTXFS_TXN_CB(int, mkdir,  (vfs_node_t *vn, const char *name), (vn, name))
VTXFS_TXN_CB(int, create, (vfs_node_t *vn, const char *name), (vn, name))
VTXFS_TXN_CB(int, unlink, (vfs_node_t *vn, const char *name), (vn, name))
VTXFS_TXN_CB(int, chmod,  (vfs_node_t *vn, uint32_t mode),    (vn, mode))
VTXFS_TXN_CB(int, chown,  (vfs_node_t *vn, uint32_t uid, uint32_t gid),
                          (vn, uid, gid))

/* ========================================================================= *
 *  mkfs — форматирование раздела (всегда v3: 4К блоки)                      *
 * ========================================================================= */

int vortexfs_mkfs(uint8_t ata_slave, uint32_t start_lba,
                  uint32_t total_sectors)
{
    g_slave     = ata_slave;
    g_start_lba = start_lba;

    /* Параметры v3 нужны уже при форматировании */
    g_ver    = VTXFS_VERSION;
    g_bs     = VTXFS_BS_V3;
    g_spb    = g_bs / 512;
    g_ppb    = g_bs / 4;
    g_dpb    = g_bs / VTXFS_DIRENT_SIZE;
    g_ino_sz = VTXFS_INO_SZ_V3;

    /* Вычисляем layout (метаданные — посекторно, данные — блоками) */
    uint32_t max_blocks = total_sectors / g_spb;                /* верхняя оценка */
    uint32_t ibm_sec = (VTXFS_MAX_INODES / 8 + 511) / 512;      /* 1    */
    uint32_t bbm_sec = (max_blocks / 8 + 511) / 512;
    uint32_t itb_sec = ((uint32_t)VTXFS_MAX_INODES
                         * VTXFS_INO_SZ_V3 + 511) / 512;        /* 1024 */

    uint32_t ibm_start = 1;
    uint32_t bbm_start = ibm_start + ibm_sec;
    uint32_t itb_start = bbm_start + bbm_sec;
    uint32_t jrn_start = itb_start + itb_sec;   /* журнал перед данными */
    uint32_t dat_start = jrn_start + VTXFS_JOURNAL_SECTORS;

    if (dat_start + 2 * g_spb >= total_sectors) return -1;

    uint32_t data_blocks = (total_sectors - dat_start) / g_spb;

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
    g_sb.journal_start      = jrn_start;
    g_sb.journal_sectors    = VTXFS_JOURNAL_SECTORS;
    g_sb.block_size         = g_bs;

    g_jrn_on = 0;   /* во время mkfs журнал выключен — пишем напрямую */
    node_cache_n = 0;

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
    for (uint32_t i = 0; i < sizeof(root); i++) p[i] = 0;
    root.type      = VFS_DIR;
    root.size      = 0;
    root.blocks    = 1;
    root.direct[0] = 1;  /* data block 1 */
    root.mode      = VTXFS_DEF_DIR_MODE;   /* root:root rwxr-xr-x */
    root.nlinks    = 1;
    ino_write(0, &root);

    /* Обнуляем блок данных root-каталога (все g_spb секторов) */
    blk_write(1, g_zero);

    fb_puts("[VortexFS] mkfs v3 OK (");
    uint32_t n = g_sb.free_blocks;
    char nbuf[12]; int i = 10; nbuf[11] = 0;
    if (!n) nbuf[i--] = '0';
    else while (n) { nbuf[i--] = '0' + n%10; n /= 10; }
    fb_puts(&nbuf[i+1]);
    fb_puts(" x 4K blocks free)\n");

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
    if (fmt_params_set() != 0) {
        fb_puts("[VortexFS] Bad block_size in superblock\n");
        return -1;
    }

    /* Журнал: только v2+ с валидной областью. Сначала replay (вдруг упали
     * посреди транзакции в прошлой сессии), потом включаем перехват. */
    g_jrn_on = 0; g_jrn_depth = 0; g_jrn_n = 0;
    g_jrn_bypass = 0; g_jrn_seq = 0;
    if (g_sb.version >= 2 && g_sb.journal_start &&
        g_sb.journal_sectors >= VTXFS_JRN_MAX_CAP + 2) {
        jrn_replay();
        g_jrn_on = 1;
        fb_puts("[VortexFS] journal enabled\n");
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
    vtxfs_ops.chmod   = vtxfs_chmod_cb;
    vtxfs_ops.chown   = vtxfs_chown_cb;

    g_root = node_make(g_sb.root_inode);
    if (!g_root) {
        fb_puts("[VortexFS] Failed to read root inode\n");
        return -1;
    }
    g_root->name[0] = '/';
    g_root->name[1] = 0;
    g_mounted = 1;

    fb_puts("[VortexFS] Mounted v");
    char vbuf[2] = { (char)('0' + g_ver), 0 };
    fb_puts(vbuf);
    fb_puts(" (");
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
