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
#include "serial.h"

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

/* Диагностика порчи кучи (черный экран virtio 2026-06-12: фолт внутри kmalloc
 * при загрузке ELF). Любой указатель free-list ОБЯЗАН лежать внутри heap и
 * быть 8-байт выровнен. Битый указатель = кто-то записал за границы своего
 * блока и затёр заголовок. Вместо разыменования мусора (#PF с бессмысленным
 * RIP) — громкий лог в COM1 с адресом: видно, ЧЕЙ заголовок затёрт. */
static int heap_ptr_valid(heap_block_t *b) {
    uint64_t a = (uint64_t)b;
    if (a < heap_virt || a + BLOCK_HDR > heap_virt + heap_size) return 0;
    if (a & 7) return 0;
    return 1;
}

static void heap_report_corrupt(const char *where, heap_block_t *prev, heap_block_t *bad) {
    serial_puts("[HEAP] CORRUPT free-list in ");
    serial_puts(where);
    serial_puts(": block ");
    serial_puts("0x");
    char buf[17]; buf[16] = 0;
    uint64_t v = (uint64_t)bad;
    for (int i = 15; i >= 0; i--) { uint8_t n = v & 0xF; buf[i] = n < 10 ? '0'+n : 'a'+n-10; v >>= 4; }
    serial_puts(buf);
    serial_puts(" (prev hdr 0x");
    v = (uint64_t)prev;
    for (int i = 15; i >= 0; i--) { uint8_t n = v & 0xF; buf[i] = n < 10 ? '0'+n : 'a'+n-10; v >>= 4; }
    serial_puts(buf);
    serial_puts(") — overflow затёр заголовок?\n");
}

void *kmalloc(uint64_t size) {
    if (!size || !heap_head) return 0;

    /* Выравниваем до 8 байт */
    size = (size + 7) & ~7ULL;

    heap_block_t *prev = 0;
    heap_block_t *cur = heap_head;
    while (cur) {
        if (!heap_ptr_valid(cur)) {
            heap_report_corrupt("kmalloc", prev, cur);
            return 0;   /* не разыменовываем мусор — лучше OOM, чем #PF */
        }
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
        prev = cur;
        cur = cur->next;
    }
    return 0; /* OOM */
}

/* Как kmalloc_aligned, но через *raw_out отдаёт СЫРОЙ указатель kmalloc —
 * только его можно отдать в kfree (выровненный смещён и kfree его не примет).
 * Это ключ к освобождению памяти процесса при exit: ELF-сегменты и user-стек
 * выделяются выровненно, и раньше их raw-указатель терялся навсегда. */
void *kmalloc_aligned2(uint64_t size, uint64_t align, void **raw_out) {
    uint8_t *raw = kmalloc(size + align);
    if (raw_out) *raw_out = raw;
    if (!raw) return 0;
    uint64_t addr = (uint64_t)raw;
    uint64_t aligned = (addr + align - 1) & ~(align - 1);
    return (void *)aligned;
}

void *kmalloc_aligned(uint64_t size, uint64_t align) {
    /* Выделяем с запасом, потом выравниваем */
    return kmalloc_aligned2(size, align, 0);
}

void kfree(void *ptr) {
    if (!ptr) return;
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HDR);
    if (!heap_ptr_valid(block)) {
        heap_report_corrupt("kfree", 0, block);
        return;   /* чужой/битый указатель — не трогаем кучу */
    }
    block->free = 1;

    /* Объединяем соседние свободные блоки (coalescing) */
    heap_block_t *cur = heap_head;
    while (cur && cur->next) {
        if (!heap_ptr_valid(cur->next)) {
            heap_report_corrupt("kfree-coalesce", cur, cur->next);
            cur->next = 0;   /* обрезаем хвост: теряем память, но живём */
            return;
        }
        if (cur->free && cur->next->free) {
            cur->size += BLOCK_HDR + cur->next->size;
            cur->next  = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
