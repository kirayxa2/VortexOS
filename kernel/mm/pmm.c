/* =============================================================================
 * VortexOS — kernel/mm/pmm.c
 * Физический менеджер памяти — bitmap аллокатор.
 * Инициализируется через pmm_init_region() которую вызывает kmain
 * после получения карты памяти от Limine.
 * ============================================================================= */

#include "pmm.h"
#include "types.h"

#define BITS_PER_WORD   64ULL
#define MAX_FRAMES      (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)
#define BITMAP_WORDS    (MAX_FRAMES / BITS_PER_WORD)

static uint64_t bitmap[BITMAP_WORDS];
static uint64_t total_frames  = 0;
static uint64_t free_frames   = 0;
static uint64_t last_free_idx = 0;

#define FRAME_TO_WORD(f) ((f) / BITS_PER_WORD)
#define FRAME_TO_BIT(f)  ((f) % BITS_PER_WORD)
#define BIT_SET(f)  (bitmap[FRAME_TO_WORD(f)] |=  (1ULL << FRAME_TO_BIT(f)))
#define BIT_CLR(f)  (bitmap[FRAME_TO_WORD(f)] &= ~(1ULL << FRAME_TO_BIT(f)))
#define BIT_TEST(f) (bitmap[FRAME_TO_WORD(f)] &   (1ULL << FRAME_TO_BIT(f)))

/* Инициализация: помечаем всё занятым */
void pmm_init(void)
{
    for (uint64_t i = 0; i < BITMAP_WORDS; i++)
        bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
    total_frames  = MAX_FRAMES;
    free_frames   = 0;
    last_free_idx = 0;
}

/* Освобождаем регион памяти (вызывается из kmain для каждого usable региона) */
void pmm_init_region(uint64_t base, uint64_t length)
{
    /* Первый МБ не трогаем — там BIOS, VGA и прочее */
    if (base < 0x100000) {
        if (base + length <= 0x100000) return;
        length -= (0x100000 - base);
        base    = 0x100000;
    }

    uint64_t first = (base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t last  = (base + length) / PAGE_SIZE;

    for (uint64_t f = first; f < last && f < MAX_FRAMES; f++) {
        if (BIT_TEST(f)) {
            BIT_CLR(f);
            free_frames++;
        }
        if (f + 1 > total_frames) total_frames = f + 1;
    }
}

uint64_t pmm_alloc(void)
{
    uint64_t start_word = FRAME_TO_WORD(last_free_idx);
    for (uint64_t w = start_word; w < BITMAP_WORDS; w++) {
        if (bitmap[w] == 0xFFFFFFFFFFFFFFFFULL) continue;
        uint64_t bit   = __builtin_ctzll(~bitmap[w]);
        uint64_t frame = w * BITS_PER_WORD + bit;
        if (frame >= total_frames) return 0;
        BIT_SET(frame);
        free_frames--;
        last_free_idx = frame + 1;
        return frame * PAGE_SIZE;
    }
    /* Попробуем с начала */
    if (start_word > 0) { last_free_idx = 0; return pmm_alloc(); }
    return 0;
}

uint64_t pmm_alloc_zero(void)
{
    /* Обнуление делается в VMM через HHDM — PMM не знает виртуальных адресов */
    return pmm_alloc();
}

void pmm_free(uint64_t phys_addr)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= total_frames) return;
    if (!BIT_TEST(frame)) return;
    BIT_CLR(frame);
    free_frames++;
    if (frame < last_free_idx) last_free_idx = frame;
}

uint64_t pmm_free_count(void)  { return free_frames;  }
uint64_t pmm_total_count(void) { return total_frames; }
