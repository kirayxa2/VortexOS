/* =============================================================================
 * VortexOS libc — malloc.c
 * Куча в статическом массиве в BSS. ELF-загрузчик ядра выделяет под сегмент
 * p_memsz целиком (kernel heap = 32 MiB), так что никакого sbrk/mmap не нужно:
 * процесс получает свою кучу «бесплатно» при загрузке.
 *
 * Аллокатор: first-fit по адресно-упорядоченному списку блоков с заголовками,
 * split при выделении, coalesce соседей при free. Выравнивание — 16 байт
 * (userspace собирается С SSE, malloc обязан отдавать 16-байт-выровненные
 * указатели, иначе movaps по ним = #GP).
 * ============================================================================= */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef VLIBC_HEAP_SIZE
#define VLIBC_HEAP_SIZE (256 * 1024)   /* 256 KiB на процесс; хватит и не жирно
                                          для QEMU -m 256M */
#endif

#define ALIGN     16
#define MAGIC_USED 0x55534544u  /* "USED" */
#define MAGIC_FREE 0x46524545u  /* "FREE" */

typedef struct block {
    uint32_t      magic;
    uint32_t      _pad;
    size_t        size;          /* размер полезной части (без заголовка) */
    struct block *next;          /* следующий блок по адресу */
} block_t;

#define HDR_SIZE  ((sizeof(block_t) + ALIGN - 1) & ~(size_t)(ALIGN - 1))

static uint8_t  heap[VLIBC_HEAP_SIZE] __attribute__((aligned(16)));
static block_t *head = 0;

static void heap_init_once(void) {
    if (head) return;
    head = (block_t *)heap;
    head->magic = MAGIC_FREE;
    head->size  = VLIBC_HEAP_SIZE - HDR_SIZE;
    head->next  = 0;
}

void *malloc(size_t size) {
    if (!size) return 0;
    heap_init_once();
    size = (size + ALIGN - 1) & ~(size_t)(ALIGN - 1);

    for (block_t *b = head; b; b = b->next) {
        if (b->magic != MAGIC_FREE || b->size < size) continue;
        /* Режем хвост в отдельный свободный блок, если есть смысл. */
        if (b->size >= size + HDR_SIZE + ALIGN) {
            block_t *tail = (block_t *)((uint8_t *)b + HDR_SIZE + size);
            tail->magic = MAGIC_FREE;
            tail->size  = b->size - size - HDR_SIZE;
            tail->next  = b->next;
            b->size = size;
            b->next = tail;
        }
        b->magic = MAGIC_USED;
        return (uint8_t *)b + HDR_SIZE;
    }
    return 0;  /* кучи не хватило */
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    if (b->magic != MAGIC_USED) return;  /* double free / мусор — игнорируем */
    b->magic = MAGIC_FREE;

    /* Слить со следующим свободным. */
    if (b->next && b->next->magic == MAGIC_FREE &&
        (uint8_t *)b + HDR_SIZE + b->size == (uint8_t *)b->next) {
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
    }
    /* Слить с предыдущим свободным (список адресно-упорядочен). */
    for (block_t *p = head; p && p->next; p = p->next) {
        if (p->next != b) continue;
        if (p->magic == MAGIC_FREE &&
            (uint8_t *)p + HDR_SIZE + p->size == (uint8_t *)b) {
            p->size += HDR_SIZE + b->size;
            p->next  = b->next;
        }
        break;
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return 0;  /* переполнение */
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (!size) { free(ptr); return 0; }
    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    if (b->magic != MAGIC_USED) return 0;
    if (b->size >= size) return ptr;   /* уже влезает */
    void *np = malloc(size);
    if (!np) return 0;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}
