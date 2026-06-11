#ifndef VOS_HEAP_H
#define VOS_HEAP_H

#include "types.h"

void  heap_init(uint64_t virt_start, uint64_t size);
void *kmalloc(uint64_t size);
void *kmalloc_aligned(uint64_t size, uint64_t align);
void *kmalloc_aligned2(uint64_t size, uint64_t align, void **raw_out);
void  kfree(void *ptr);

#endif /* VOS_HEAP_H */
