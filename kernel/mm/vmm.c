/* =============================================================================
 * VortexOS — kernel/mm/vmm.c
 * Виртуальный менеджер памяти. 4-уровневый paging x86_64.
 *
 * Структура: PML4 -> PDPT -> PD -> PT -> физическая страница
 * Каждая таблица — 512 записей по 8 байт = 4096 байт (одна страница).
 * ============================================================================= */

#include "vmm.h"
#include "pmm.h"
#include "types.h"
#include "fb.h"

/* Смещение прямого маппинга физики (HHDM от Limine) */
static uint64_t hhdm_off = 0;
uint64_t hhdm_offset = 0; /* Экспортируем для syscall */

pte_t *vmm_kernel_pml4 = 0;

/* Превращаем физический адрес в виртуальный через HHDM */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_off);
}

/* Превращаем kernel виртуальный адрес в физический через HHDM */
static inline uint64_t virt_to_phys_hhdm(uint64_t virt) {
    return virt - hhdm_off;
}

/* Выделяем и обнуляем страницу для таблицы через HHDM */
static pte_t *alloc_table(void) {
    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    pte_t *virt = (pte_t *)phys_to_virt(phys);
    for (int i = 0; i < 512; i++) virt[i] = 0;
    return virt;
}

/* Физический адрес виртуального указателя на таблицу */
static inline uint64_t table_phys(pte_t *t) {
    return (uint64_t)t - hhdm_off;
}

/* Индексы в каждом уровне таблицы для виртуального адреса */
#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)
#define PHYS_MASK   0x000FFFFFFFFFF000ULL

/* Получаем или создаём следующий уровень таблицы */
static pte_t *get_or_create(pte_t *table, uint64_t idx, uint64_t flags) {
    (void)flags; // Unused — intermediate tables always get fixed flags
    
    if (!(table[idx] & VMM_PRESENT)) {
        pte_t *child = alloc_table();
        if (!child) return 0;
        
        // Intermediate tables ALWAYS need: PRESENT | WRITABLE | USER
        // This ensures CPU can traverse the page table hierarchy
        table[idx] = table_phys(child) | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    }
    return (pte_t *)phys_to_virt(table[idx] & PHYS_MASK);
}

void vmm_init(uint64_t hhdm_offset_param) {
    hhdm_off = hhdm_offset_param;
    hhdm_offset = hhdm_offset_param; /* Экспортируем */
    
    /* Включаем NXE (No-Execute Enable) в IA32_EFER */
    /* Это необходимо, иначе бит 63 в PTE считается reserved! */
    /* Без NXE любой бит в старших 12 битах физ. адреса вызовет Page Fault 0x1c */
    uint32_t efer_msr = 0xC0000080;
    uint32_t eax, edx;
    __asm__ volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(efer_msr));
    uint32_t old_eax = eax;
    eax |= (1 << 11); /* NXE = bit 11 */
    __asm__ volatile("wrmsr" :: "a"(eax), "d"(edx), "c"(efer_msr));
    
    fb_puts("[VMM] Enabled IA32_EFER.NXE (EFER: ");
    fb_puthex(old_eax);
    fb_puts(" -> ");
    fb_puthex(eax);
    fb_puts(")\n");

    uint64_t old_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    pte_t *old_pml4 = (pte_t *)phys_to_virt(old_cr3 & PHYS_MASK);

    vmm_kernel_pml4 = alloc_table();

    for (int i = 256; i < 512; i++)
        vmm_kernel_pml4[i] = old_pml4[i];
}

/* Маппим одну страницу virt -> phys */
void vmm_map(pte_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    // Проверка: физический адрес должен быть выровнен по странице
    if (phys & 0xFFF) {
        fb_puts("[VMM ERROR] Physical address not page-aligned: ");
        fb_puthex(phys);
        fb_puts("\n");
        return;
    }
    
    // Проверка: используем только биты 12-51 для физического адреса
    if (phys & ~PHYS_MASK) {
        fb_puts("[VMM ERROR] Physical address has reserved bits set: ");
        fb_puthex(phys);
        fb_puts("\n");
        return;
    }
    
    // get_or_create will set PRESENT|WRITABLE|USER for intermediate tables
    pte_t *pdpt = get_or_create(pml4,  PML4_IDX(virt), 0);
    if (!pdpt) { fb_puts("[VMM] Failed to get/create PDPT\n"); return; }
    pte_t *pd   = get_or_create(pdpt,  PDPT_IDX(virt), 0);
    if (!pd)   { fb_puts("[VMM] Failed to get/create PD\n"); return; }
    pte_t *pt   = get_or_create(pd,    PD_IDX(virt),   0);
    if (!pt)   { fb_puts("[VMM] Failed to get/create PT\n"); return; }

    // Конечная страница — используем переданные флаги (только младшие 12 бит)
    uint64_t pte_value = (phys & PHYS_MASK) | (flags & 0xFFF);
    pt[PT_IDX(virt)] = pte_value;
    
    // DEBUG: Проверяем что запись прошла
    if (pt[PT_IDX(virt)] != pte_value) {
        fb_puts("[VMM] ERROR: PT entry write failed! Expected ");
        fb_puthex(pte_value);
        fb_puts(" but got ");
        fb_puthex(pt[PT_IDX(virt)]);
        fb_puts("\n");
    }

    /* Сбрасываем TLB для этой страницы */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_unmap(pte_t *pml4, uint64_t virt) {
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return;
    pte_t *pdpt = (pte_t *)phys_to_virt(pml4[PML4_IDX(virt)] & PHYS_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return;
    pte_t *pd   = (pte_t *)phys_to_virt(pdpt[PDPT_IDX(virt)] & PHYS_MASK);
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return;
    pte_t *pt   = (pte_t *)phys_to_virt(pd[PD_IDX(virt)] & PHYS_MASK);

    pt[PT_IDX(virt)] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

uint64_t vmm_virt_to_phys(pte_t *pml4, uint64_t virt) {
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) {
        fb_puts("[VMM] PML4 entry not present for virt ");
        fb_puthex(virt);
        fb_puts("\n");
        return 0;
    }
    pte_t *pdpt = (pte_t *)phys_to_virt(pml4[PML4_IDX(virt)] & PHYS_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) {
        fb_puts("[VMM] PDPT entry not present for virt ");
        fb_puthex(virt);
        fb_puts("\n");
        return 0;
    }
    pte_t *pd   = (pte_t *)phys_to_virt(pdpt[PDPT_IDX(virt)] & PHYS_MASK);
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) {
        fb_puts("[VMM] PD entry not present for virt ");
        fb_puthex(virt);
        fb_puts("\n");
        return 0;
    }
    pte_t *pt   = (pte_t *)phys_to_virt(pd[PD_IDX(virt)] & PHYS_MASK);
    if (!(pt[PT_IDX(virt)] & VMM_PRESENT)) {
        fb_puts("[VMM] PT entry not present for virt ");
        fb_puthex(virt);
        fb_puts("\n");
        return 0;
    }
    
    uint64_t result = (pt[PT_IDX(virt)] & PHYS_MASK) | (virt & 0xFFF);
    return result;
}

void vmm_switch(pte_t *pml4) {
    uint64_t phys = table_phys(pml4);
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

/* Создаём user page table — копируем верхнюю половину (kernel) */
pte_t *vmm_create_user_pml4(void) {
    pte_t *user_pml4 = alloc_table();
    if (!user_pml4) return 0;
    
    /* Копируем kernel mappings (верхняя половина, индексы 256-511) */
    for (int i = 256; i < 512; i++) {
        user_pml4[i] = vmm_kernel_pml4[i];
    }
    
    /* Нижняя половина (0-255) остаётся пустой для userspace */
    return user_pml4;
}

/* Преобразование kernel virtual address в physical (для HHDM области) */
uint64_t vmm_kernel_virt_to_phys(uint64_t virt) {
    return virt_to_phys_hhdm(virt);
}
