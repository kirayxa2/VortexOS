/* =============================================================================
 * VortexOS — kernel/drivers/ata.c
 * Простой ATA PIO драйвер для чтения/записи секторов с диска.
 * Работает с Primary ATA (0x1F0) и Secondary (0x170).
 * PIO Mode — медленно, но просто и не требует DMA/AHCI.
 * ============================================================================= */

#include "ata.h"
#include "fb.h"

/* Порты Primary ATA */
#define ATA_PRIMARY_DATA        0x1F0
#define ATA_PRIMARY_ERROR       0x1F1
#define ATA_PRIMARY_SECCOUNT    0x1F2
#define ATA_PRIMARY_LBA_LO      0x1F3
#define ATA_PRIMARY_LBA_MID     0x1F4
#define ATA_PRIMARY_LBA_HI      0x1F5
#define ATA_PRIMARY_DRIVE       0x1F6
#define ATA_PRIMARY_STATUS      0x1F7
#define ATA_PRIMARY_CMD         0x1F7
#define ATA_PRIMARY_CTRL        0x3F6

/* Статусные биты */
#define ATA_SR_BSY  0x80  /* Busy */
#define ATA_SR_DRDY 0x40  /* Drive Ready */
#define ATA_SR_DRQ  0x08  /* Data Request */
#define ATA_SR_ERR  0x01  /* Error */

/* Команды */
#define ATA_CMD_READ_PIO  0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY  0xEC
#define ATA_CMD_FLUSH     0xE7

static ata_drive_t drives[2]; /* 0=master, 1=slave */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(port)); return v;
}

/* 400нс задержка (4 чтения status) */
static void ata_delay(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

/* Ждём пока BSY не снимется */
static int ata_wait_not_busy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRIMARY_STATUS);
        if (!(st & ATA_SR_BSY)) return 0;
    }
    return -1; /* таймаут */
}

/* Ждём DRQ или ERR */
static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRIMARY_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (st & ATA_SR_DRQ) return 0;
    }
    return -1;
}

/* IDENTIFY — получаем информацию о диске */
static int ata_identify(uint8_t slave) {
    /* Выбираем диск */
    outb(ATA_PRIMARY_DRIVE, slave ? 0xB0 : 0xA0);
    ata_delay();

    /* Обнуляем регистры */
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO,   0);
    outb(ATA_PRIMARY_LBA_MID,  0);
    outb(ATA_PRIMARY_LBA_HI,   0);

    /* Посылаем IDENTIFY */
    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);
    ata_delay();

    uint8_t st = inb(ATA_PRIMARY_STATUS);
    if (!st) return -1; /* диска нет */

    if (ata_wait_not_busy() < 0) return -1;

    /* Проверяем что это ATA, не ATAPI */
    if (inb(ATA_PRIMARY_LBA_MID) || inb(ATA_PRIMARY_LBA_HI)) return -1;

    if (ata_wait_drq() < 0) return -1;

    /* Читаем 256 слов (512 байт) IDENTIFY данных */
    uint16_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(ATA_PRIMARY_DATA);

    ata_drive_t *d = &drives[slave];
    d->exists = 1;
    d->slave  = slave;

    /* LBA28 секторов — слова 60-61 */
    d->sectors = ((uint32_t)buf[61] << 16) | buf[60];

    /* Имя модели — слова 27-46, каждое слово = 2 ASCII символа (swap) */
    for (int i = 0; i < 20; i++) {
        d->model[i*2]   = (char)(buf[27+i] >> 8);
        d->model[i*2+1] = (char)(buf[27+i] & 0xFF);
    }
    d->model[40] = 0;

    /* Убираем trailing spaces */
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;

    return 0;
}

void ata_init(void) {
    /* Сброс контроллера */
    outb(ATA_PRIMARY_CTRL, 0x04); /* SRST = 1 */
    ata_delay();
    outb(ATA_PRIMARY_CTRL, 0x00); /* SRST = 0 */
    ata_delay();

    drives[0].exists = 0;
    drives[1].exists = 0;

    if (ata_identify(0) == 0) {
        fb_puts("[ATA] Master: ");
        fb_puts(drives[0].model);
        fb_puts(" (");
        /* вывод числа секторов */
        uint32_t n = drives[0].sectors;
        char buf[12]; int i = 10; buf[11] = 0;
        if (!n) buf[i--] = '0';
        else while (n) { buf[i--] = '0' + n%10; n/=10; }
        fb_puts(&buf[i+1]);
        fb_puts(" sectors)\n");
    } else {
        fb_puts("[ATA] Master: not found\n");
    }

    if (ata_identify(1) == 0) {
        fb_puts("[ATA] Slave:  ");
        fb_puts(drives[1].model);
        fb_puts("\n");
    }
}

/* Читает count секторов начиная с lba в buf (PIO 28-bit LBA) */
int ata_read(uint8_t slave, uint32_t lba, uint8_t count, uint16_t *buf) {
    if (!drives[slave].exists) return -1;
    if (ata_wait_not_busy() < 0) return -1;

    outb(ATA_PRIMARY_DRIVE,    (slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_ERROR,    0x00);
    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO,   (uint8_t)(lba));
    outb(ATA_PRIMARY_LBA_MID,  (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HI,   (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_CMD,      ATA_CMD_READ_PIO);

    for (int s = 0; s < count; s++) {
        if (ata_wait_not_busy() < 0) return -1;
        if (ata_wait_drq()      < 0) return -1;
        for (int i = 0; i < 256; i++)
            buf[s * 256 + i] = inw(ATA_PRIMARY_DATA);
    }
    return 0;
}

/* Записывает count секторов начиная с lba из buf */
int ata_write(uint8_t slave, uint32_t lba, uint8_t count, const uint16_t *buf) {
    if (!drives[slave].exists) return -1;
    if (ata_wait_not_busy() < 0) return -1;

    outb(ATA_PRIMARY_DRIVE,    (slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_ERROR,    0x00);
    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO,   (uint8_t)(lba));
    outb(ATA_PRIMARY_LBA_MID,  (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HI,   (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_CMD,      ATA_CMD_WRITE_PIO);

    for (int s = 0; s < count; s++) {
        if (ata_wait_not_busy() < 0) return -1;
        if (ata_wait_drq()      < 0) return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_PRIMARY_DATA, buf[s * 256 + i]);
    }

    /* Flush cache */
    outb(ATA_PRIMARY_CMD, ATA_CMD_FLUSH);
    ata_wait_not_busy();
    return 0;
}

ata_drive_t *ata_get_drive(uint8_t slave) {
    if (!drives[slave].exists) return 0;
    return &drives[slave];
}

/* Wrapper для чтения 1 сектора (512 байт) */
int ata_read_sector(uint8_t slave, uint32_t lba, void *buf) {
    return ata_read(slave, lba, 1, (uint16_t *)buf);
}
