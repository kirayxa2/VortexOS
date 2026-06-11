#ifndef VOS_VMM_H
#define VOS_VMM_H

#include "types.h"

/* Флаги записи page table */
#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITABLE  (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_HUGE      (1ULL << 7)
#define VMM_NX        (1ULL << 63)

/* Верхняя половина: куда маппим ядро */
#define KERNEL_VIRT_BASE  0xFFFFFFFF80000000ULL
#define HHDM_OFFSET       0xFFFF800000000000ULL  /* direct map всей физики */

typedef uint64_t pte_t;

/* Корневая таблица страниц (PML4) текущего адресного пространства */
extern pte_t *vmm_kernel_pml4;

void vmm_init(uint64_t hhdm_offset);
void vmm_map(pte_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(pte_t *pml4, uint64_t virt);
uint64_t vmm_virt_to_phys(pte_t *pml4, uint64_t virt);
void vmm_switch(pte_t *pml4);
pte_t *vmm_create_user_pml4(void);
void   vmm_destroy_user_pml4(pte_t *pml4);
uint64_t vmm_kernel_virt_to_phys(uint64_t virt);

#endif /* VOS_VMM_H */
