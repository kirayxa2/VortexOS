/* =============================================================================
 * VortexOS — kernel/drivers/pci.c
 * PCI enumeration через механизм Configuration Space (ports 0xCF8/0xCFC).
 * Сканирует все шины/устройства/функции и строит таблицу устройств.
 * ============================================================================= */

#include "pci.h"
#include "fb.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAX_DEVICES 64

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static uint32_t     pci_device_count = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

/* Формирует адрес в PCI Configuration Space */
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    return (1u << 31)           /* Enable bit */
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func <<  8)
         | (reg & 0xFC);        /* выравниваем по 4 байта */
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, reg));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t val = pci_read32(bus, dev, func, reg & ~3u);
    return (uint16_t)(val >> ((reg & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t val = pci_read32(bus, dev, func, reg & ~3u);
    return (uint8_t)(val >> ((reg & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, reg));
    outl(PCI_CONFIG_DATA, val);
}

/* Возвращает строку по class/subclass */
static const char *pci_class_name(uint8_t class, uint8_t subclass) {
    switch (class) {
        case 0x00: return "Unknown";
        case 0x01:
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x06: return "SATA Controller (AHCI)";
                case 0x08: return "NVMe Controller";
                default:   return "Mass Storage";
            }
        case 0x02: return "Network Controller";
        case 0x03: return "Display Controller";
        case 0x04: return "Multimedia Controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                default:   return "Bridge";
            }
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                default:   return "Serial Bus Controller";
            }
        default: return "Unknown";
    }
}

static void pci_scan_func(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vendor = pci_read16(bus, dev, func, 0x00);
    if (vendor == 0xFFFF) return; /* нет устройства */

    if (pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *d = &pci_devices[pci_device_count++];
    d->bus      = bus;
    d->dev      = dev;
    d->func     = func;
    d->vendor   = vendor;
    d->device   = pci_read16(bus, dev, func, 0x02);
    d->class    = pci_read8 (bus, dev, func, 0x0B);
    d->subclass = pci_read8 (bus, dev, func, 0x0A);
    d->prog_if  = pci_read8 (bus, dev, func, 0x09);
    d->header   = pci_read8 (bus, dev, func, 0x0E) & 0x7F;

    /* BAR0..5 */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, dev, func, 0x10 + i * 4);

    d->irq_line = pci_read8(bus, dev, func, 0x3C);
    d->irq_pin  = pci_read8(bus, dev, func, 0x3D);
}

static void pci_scan_device(uint8_t bus, uint8_t dev) {
    uint16_t vendor = pci_read16(bus, dev, 0, 0x00);
    if (vendor == 0xFFFF) return;

    pci_scan_func(bus, dev, 0);

    /* Мульти-функциональное устройство? */
    uint8_t header = pci_read8(bus, dev, 0, 0x0E);
    if (header & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            if (pci_read16(bus, dev, func, 0x00) != 0xFFFF)
                pci_scan_func(bus, dev, func);
        }
    }
}

void pci_init(void) {
    pci_device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            pci_scan_device((uint8_t)bus, dev);

    fb_puts("[PCI] Enumeration done. Devices found: ");
    /* вывод числа */
    char buf[8];
    uint32_t n = pci_device_count;
    int i = 6; buf[7] = 0;
    if (n == 0) { buf[i--] = '0'; }
    else { while (n) { buf[i--] = '0' + (n % 10); n /= 10; } }
    fb_puts(&buf[i+1]);
    fb_puts("\n");

    for (uint32_t k = 0; k < pci_device_count; k++) {
        pci_device_t *d = &pci_devices[k];
        fb_puts("  [");
        /* bus:dev.func */
        fb_putchar('0' + d->bus / 100);
        fb_putchar('0' + (d->bus / 10) % 10);
        fb_putchar('0' + d->bus % 10);
        fb_putchar(':');
        fb_putchar('0' + d->dev / 10);
        fb_putchar('0' + d->dev % 10);
        fb_putchar('.');
        fb_putchar('0' + d->func);
        fb_puts("] ");
        fb_puts(pci_class_name(d->class, d->subclass));
        fb_puts(" (");
        /* vendor:device hex */
        static const char hex[] = "0123456789abcdef";
        fb_putchar(hex[(d->vendor >> 12) & 0xF]);
        fb_putchar(hex[(d->vendor >>  8) & 0xF]);
        fb_putchar(hex[(d->vendor >>  4) & 0xF]);
        fb_putchar(hex[(d->vendor >>  0) & 0xF]);
        fb_putchar(':');
        fb_putchar(hex[(d->device >> 12) & 0xF]);
        fb_putchar(hex[(d->device >>  8) & 0xF]);
        fb_putchar(hex[(d->device >>  4) & 0xF]);
        fb_putchar(hex[(d->device >>  0) & 0xF]);
        fb_puts(")\n");
    }
}

uint32_t pci_device_count_get(void) { return pci_device_count; }

pci_device_t *pci_get_device(uint32_t index) {
    if (index >= pci_device_count) return 0;
    return &pci_devices[index];
}

pci_device_t *pci_find(uint8_t class, uint8_t subclass) {
    for (uint32_t i = 0; i < pci_device_count; i++)
        if (pci_devices[i].class == class && pci_devices[i].subclass == subclass)
            return &pci_devices[i];
    return 0;
}
