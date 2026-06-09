/* =============================================================================
 * VortexOS — kernel/mm/heap.c
 * Простой heap allocator для ядра (free-list, first-fit).
 *
 * Блок памяти:  [heap_block header][...данные...]
 * Свободные блоки связаны в односвязный список.
 * ============================================================================= */

#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "types.h"

/* Заголовок блока */
typedef struct heap_block {
    uint64_t            size;   /* размер данных (без заголовка) */
    uint8_t             free;   /* 1 = свободен */
    struct heap_block  *next;   /* следующий блок в списке */
} heap_block_t;

#define BLOCK_HDR sizeof(heap_block_t)
#define MIN_SPLIT 32  /* не дробить блок если остаток меньше этого */

static heap_block_t *heap_head = 0;
static uint64_t      heap_virt = 0;
static uint64_t      heap_size = 0;

/* Маппим страницы heap в виртуальное пространство */
void heap_init(uint64_t virt_start, uint64_t size) {
    heap_virt = virt_start;
    heap_size = size;

    /* Маппим все страницы heap */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc();
        if (!phys) return;
        vmm_map(vmm_kernel_pml4, virt_start + off, phys,
                VMM_PRESENT | VMM_WRITABLE);
    }

    /* Один большой свободный блок на весь heap */
    heap_head        = (heap_block_t *)virt_start;
    heap_head->size  = size - BLOCK_HDR;
    heap_head->free  = 1;
    heap_head->next  = 0;
}

void *kmalloc(uint64_t size) {
    if (!size || !heap_head) return 0;

    /* Выравниваем до 8 байт */
    size = (size + 7) & ~7ULL;

    heap_block_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Можно ли разбить блок? */
            if (cur->size >= size + BLOCK_HDR + MIN_SPLIT) {
                heap_block_t *split = (heap_block_t *)((uint8_t *)cur + BLOCK_HDR + size);
                split->size = cur->size - size - BLOCK_HDR;
                split->free = 1;
                split->next = cur->next;
                cur->next   = split;
                cur->size   = size;
            }
            cur->free = 0;
            return (void *)((uint8_t *)cur + BLOCK_HDR);
        }
        cur = cur->next;
    }
    return 0; /* OOM */
}

void *kmalloc_aligned(uint64_t size, uint64_t align) {
    /* Выделяем с запасом, потом выравниваем */
    uint8_t *raw = kmalloc(size + align);
    if (!raw) return 0;
    uint64_t addr = (uint64_t)raw;
    uint64_t aligned = (addr + align - 1) & ~(align - 1);
    return (void *)aligned;
}

void kfree(void *ptr) {
    if (!ptr) return;
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HDR);
    block->free = 1;

    /* Объединяем соседние свободные блоки (coalescing) */
    heap_block_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += BLOCK_HDR + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
