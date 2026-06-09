#ifndef VOS_PMM_H
#define VOS_PMM_H

#include "types.h"

#define PAGE_SIZE 4096ULL

void     pmm_init(void);
void     pmm_init_region(uint64_t base, uint64_t length);
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_zero(void);
void     pmm_free(uint64_t phys_addr);
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

#endif /* VOS_PMM_H */
