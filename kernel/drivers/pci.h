#ifndef VOS_PCI_H
#define VOS_PCI_H

#include "types.h"

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if;
    uint8_t  header;
    uint32_t bar[6];
    uint8_t  irq_line, irq_pin;
} pci_device_t;

void          pci_init(void);
uint32_t      pci_device_count_get(void);
pci_device_t *pci_get_device(uint32_t index);
pci_device_t *pci_find(uint8_t class, uint8_t subclass);

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);

#endif
