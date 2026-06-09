#ifndef VOS_ATA_H
#define VOS_ATA_H

#include "types.h"

typedef struct {
    uint8_t  exists;
    uint8_t  slave;
    uint32_t sectors;
    char     model[41];
} ata_drive_t;

void         ata_init(void);
int          ata_read(uint8_t slave, uint32_t lba, uint8_t count, uint16_t *buf);
int          ata_write(uint8_t slave, uint32_t lba, uint8_t count, const uint16_t *buf);
int          ata_read_sector(uint8_t slave, uint32_t lba, void *buf); /* Wrapper для 1 сектора */
ata_drive_t *ata_get_drive(uint8_t slave);

#endif
