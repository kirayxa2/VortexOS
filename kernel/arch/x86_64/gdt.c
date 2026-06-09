/* =============================================================================
 * VortexOS — kernel/arch/x86_64/gdt.c
 * ============================================================================= */

#include "gdt.h"
#include "types.h"

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_entry_tss_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

#define GDT_NULL        0
#define GDT_KERNEL_CODE 1
#define GDT_KERNEL_DATA 2
#define GDT_USER_DATA   3   /* index 3 = selector 0x18, RPL3=0x1B — ПЕРЕД ucode для sysretq */
#define GDT_USER_CODE   4   /* index 4 = selector 0x20, RPL3=0x23 */
#define GDT_TSS         5
#define GDT_ENTRIES     7

#define ACCESS_PRESENT      (1 << 7)
#define ACCESS_DPL_RING0    (0 << 5)
#define ACCESS_DPL_RING3    (3 << 5)
#define ACCESS_CODE_DATA    (1 << 4)
#define ACCESS_EXEC         (1 << 3)
#define ACCESS_RW           (1 << 1)
#define ACCESS_TSS_TYPE     0x09

#define GRAN_LONG_MODE      (1 << 5)
#define GRAN_4KB            (1 << 7)
#define GRAN_32BIT          (1 << 6)

static gdt_entry_t  gdt[GDT_ENTRIES];
static gdt_ptr_t    gdt_ptr;
static tss_t        kernel_tss;

extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(void);

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t granularity)
{
    gdt[idx].base_low    = (base & 0xFFFF);
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = (limit & 0xFFFF);
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (granularity & 0xF0);
    gdt[idx].access      = access;
}

static void gdt_set_tss(int idx, uint64_t base, uint32_t limit)
{
    gdt_entry_tss_t *tss_desc = (gdt_entry_tss_t *)&gdt[idx];
    tss_desc->limit_low  = (limit & 0xFFFF);
    tss_desc->base_low   = (base & 0xFFFF);
    tss_desc->base_mid   = (base >> 16) & 0xFF;
    tss_desc->access     = ACCESS_PRESENT | ACCESS_TSS_TYPE;
    tss_desc->granularity= ((limit >> 16) & 0x0F);
    tss_desc->base_high  = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved   = 0;
}

void gdt_init(void)
{
    gdt_set_entry(GDT_NULL, 0, 0, 0, 0);

    gdt_set_entry(GDT_KERNEL_CODE, 0, 0xFFFFFFFF,
        ACCESS_PRESENT | ACCESS_DPL_RING0 | ACCESS_CODE_DATA | ACCESS_EXEC | ACCESS_RW,
        GRAN_LONG_MODE | GRAN_4KB);

    gdt_set_entry(GDT_KERNEL_DATA, 0, 0xFFFFFFFF,
        ACCESS_PRESENT | ACCESS_DPL_RING0 | ACCESS_CODE_DATA | ACCESS_RW,
        GRAN_4KB | GRAN_32BIT);

    /* GDT порядок (нужен для sysretq: SS=STAR+8, CS=STAR+16):
     * 0=null, 1=kcode(0x08), 2=kdata(0x10), 3=udata(0x18/0x1B), 4=ucode(0x20/0x23)
     * STAR[63:48]=0x10 → SS=0x18|3=0x1B(udata) ✓, CS=0x20|3=0x23(ucode) ✓
     */
    gdt_set_entry(GDT_USER_DATA, 0, 0xFFFFFFFF,
        ACCESS_PRESENT | ACCESS_DPL_RING3 | ACCESS_CODE_DATA | ACCESS_RW,
        GRAN_4KB | GRAN_32BIT);

    gdt_set_entry(GDT_USER_CODE, 0, 0xFFFFFFFF,
        ACCESS_PRESENT | ACCESS_DPL_RING3 | ACCESS_CODE_DATA | ACCESS_EXEC | ACCESS_RW,
        GRAN_LONG_MODE | GRAN_4KB);

    __builtin_memset(&kernel_tss, 0, sizeof(tss_t));
    kernel_tss.iopb_offset = sizeof(tss_t);
    gdt_set_tss(GDT_TSS, (uint64_t)&kernel_tss, sizeof(tss_t) - 1);

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdt_ptr);
    tss_flush();
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
}

void gdt_install_tss(uint64_t tss_addr)
{
    // Already done in gdt_init, this is for compatibility
    (void)tss_addr;
}
