#include "elf.h"
#include "vfs.h"
#include "heap.h"
#include "vmm.h"
#include "sched.h"   /* task_track_alloc: память сегментов/стека — kfree при exit */
#include <stddef.h>

extern void* kmalloc(uint64_t size);
extern void* kmalloc_aligned(uint64_t size, uint64_t align);
extern void* kmalloc_aligned2(uint64_t size, uint64_t align, void **raw_out);
extern void fb_puts(const char *s);
extern pte_t *vmm_kernel_pml4;
extern uint64_t pmm_alloc(void);
extern uint64_t pmm_alloc_zero(void);
extern void     pmm_free(uint64_t phys_addr);

void mem_memset(void *ptr, int value, size_t num) {
    uint8_t *p = (uint8_t *)ptr;
    while (num--) *p++ = (uint8_t)value;
}

void mem_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void serial_write(const char *s) {
    fb_puts(s);
}

/* -----------------------------------------------------------------------------
 * Поиск исполняемого файла а-ля $PATH (FS-иерархия как в Linux).
 *
 * Бинарники теперь живут в /bin (см. Makefile disk-with-apps). Порядок:
 *   "vterm"      → /bin/vterm, потом /vterm
 *   "/vterm"     → /vterm, потом /bin/vterm   (старые захардкоженные пути)
 *   "/bin/vterm" → /bin/vterm, потом /vterm   (новый диск ↔ старый диск)
 * Пути с подкаталогами ("/etc/motd") идут как есть, без фоллбеков.
 * ----------------------------------------------------------------------------- */
/* vfs_open + проверка «это файл и есть право X» (права VortexFS). */
static vfs_node_t *open_if_exec(const char *p) {
    vfs_node_t *n = vfs_open(p, 0);
    if (!n) return 0;
    if (n->type != VFS_FILE || vfs_access(n, VFS_PERM_X) != 0) return 0;
    return n;
}

vfs_node_t *elf_open_exec(const char *path) {
    if (!path || !path[0]) return 0;

    char alt[VFS_MAX_PATH];
    const char *name = 0;   /* короткое имя без каталога, если удалось выделить */

    if (path[0] != '/') {
        name = path;
    } else if (path[1]) {
        const char *slash2 = 0;
        for (int i = 1; path[i]; i++)
            if (path[i] == '/') { slash2 = &path[i]; break; }
        if (!slash2) {
            name = path + 1;                       /* "/vterm" */
        } else {
            /* "/bin/vterm" → name = "vterm"; глубже ("/etc/x/y") не трогаем */
            if (path[1] == 'b' && path[2] == 'i' && path[3] == 'n' &&
                path[4] == '/' && path[5]) {
                int deeper = 0;
                for (int i = 5; path[i]; i++)
                    if (path[i] == '/') { deeper = 1; break; }
                if (!deeper) name = path + 5;
            }
        }
        vfs_node_t *node = open_if_exec(path);    /* абсолютный путь — как есть */
        if (node) return node;
    }

    if (!name || !name[0]) return 0;

    /* /bin/<name> */
    int n = 0;
    alt[n++] = '/'; alt[n++] = 'b'; alt[n++] = 'i'; alt[n++] = 'n'; alt[n++] = '/';
    for (int i = 0; name[i] && n < VFS_MAX_PATH - 1; i++) alt[n++] = name[i];
    alt[n] = 0;
    vfs_node_t *node = open_if_exec(alt);
    if (node) return node;

    /* /<name> — fallback для старых образов диска */
    n = 0;
    alt[n++] = '/';
    for (int i = 0; name[i] && n < VFS_MAX_PATH - 1; i++) alt[n++] = name[i];
    alt[n] = 0;
    return open_if_exec(alt);
}

elf_load_result_t elf_load(const char *path) {
    elf_load_result_t result = {0, NULL};
    
    serial_write("[ELF] Loading ");
    serial_write(path);
    serial_write("\n");

    // Create user page table
    pte_t *user_pml4 = vmm_create_user_pml4();
    if (!user_pml4) {
        serial_write("[ELF] Error: Cannot create user page table\n");
        return result;
    }
    /* Отдаём pml4 в result СРАЗУ: на любом фейле ниже (entry_point останется 0)
     * вызывающий обязан снести её vmm_destroy_user_pml4 — раньше частично
     * построенная таблица текла при каждом неудачном запуске. */
    result.user_pml4 = user_pml4;

    // Open file through VFS (с поиском в /bin — см. elf_open_exec)
    vfs_node_t *node = elf_open_exec(path);
    if (!node) {
        serial_write("[ELF] Error: File not found\n");
        return result;
    }

    // Read ELF header
    Elf64_Ehdr ehdr;
    if (vfs_read(node, 0, sizeof(Elf64_Ehdr), (uint8_t*)&ehdr) != sizeof(Elf64_Ehdr)) {
        serial_write("[ELF] Error: Could not read ELF header\n");
        vfs_close(node);
        return result;
    }

    // Validate magic
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        serial_write("[ELF] Error: Invalid ELF magic\n");
        vfs_close(node);
        return result;
    }

    // Validate 64-bit
    if (ehdr.e_ident[4] != ELFCLASS64) {
        serial_write("[ELF] Error: Not a 64-bit ELF\n");
        vfs_close(node);
        return result;
    }

    // Validate x86_64
    if (ehdr.e_machine != EM_X86_64) {
        serial_write("[ELF] Error: Not x86_64 architecture\n");
        vfs_close(node);
        return result;
    }

    // Validate executable
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        serial_write("[ELF] Error: Not an executable\n");
        vfs_close(node);
        return result;
    }

    serial_write("[ELF] Valid ELF64 x86_64 executable\n");

    /* Demand paging: вместо чтения всего PT_LOAD сегмента в kmalloc'd буфер
     * и постраничного маппинга, теперь только регистрируем VMA в текущей
     * задаче — физические страницы выделяются в обработчике #PF (см.
     * pf_demand_load ниже) по мере обращения. ELF-узел остаётся открытым
     * до task_exit, который закроет его. */
    task_t *cur = sched_current();
    if (!cur) {
        serial_write("[ELF] Error: no current task\n");
        vfs_close(node);
        return result;
    }
    cur->n_vmas = 0;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);

        if (vfs_read(node, offset, sizeof(Elf64_Phdr), (uint8_t*)&phdr) != sizeof(Elf64_Phdr)) {
            serial_write("[ELF] Error: Could not read program header\n");
            continue;
        }

        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_memsz == 0) continue;

        if (cur->n_vmas >= TASK_MAX_VMAS) {
            serial_write("[ELF] Error: too many PT_LOAD segments\n");
            vfs_close(node);
            return result;
        }

        uint64_t va_start = phdr.p_vaddr & ~0xFFFULL;                       /* page-align вниз */
        uint64_t va_end   = (phdr.p_vaddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL; /* вверх */
        /* file_offset для page va: file_offset + (va - va_start). При aligned
         * p_vaddr (vaddr_offset = 0) это совпадает с phdr.p_offset. */
        uint64_t vaddr_offset = phdr.p_vaddr & 0xFFFULL;
        uint64_t file_off_base = phdr.p_offset - vaddr_offset;

        elf_vma_t *v = &cur->vmas[cur->n_vmas++];
        v->vaddr_start    = va_start;
        v->vaddr_end      = va_end;
        v->file_offset    = file_off_base;
        v->file_end_vaddr = phdr.p_vaddr + phdr.p_filesz;
        v->flags = VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    }

    /* ELF-узел сохраняем в задаче — pf_demand_load будет читать страницы
     * из него по запросу, а task_exit закроет его. */
    cur->elf_node = node;
    
    // Return the entry point from ELF header
    result.entry_point = ehdr.e_entry;
    result.user_pml4 = user_pml4;
    
    serial_write("[ELF] Entry point: ");
    char hexbuf[20];
    int pos = 0;
    hexbuf[pos++] = '0';
    hexbuf[pos++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        int digit = (result.entry_point >> shift) & 0xF;
        hexbuf[pos++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
    }
    hexbuf[pos] = '\0';
    serial_write(hexbuf);
    serial_write("\n");
    
    /* Map user stack (16KB) at 0x800000 */
    serial_write("[ELF] Allocating user stack...\n");
    void *stack_raw = 0;
    uint8_t *stack_buf = kmalloc_aligned2(16384, 4096, &stack_raw);
    if (!stack_buf) {
        serial_write("[ELF] Failed to allocate user stack\n");
        result.entry_point = 0;   /* без стека процесс не жилец */
        return result;
    }
    if (task_track_alloc(0, stack_raw) < 0)
        serial_write("[ELF] WARN: alloc list full, stack will leak\n");
    
    uint64_t stack_phys = vmm_virt_to_phys(vmm_kernel_pml4, (uint64_t)stack_buf);
    if (!stack_phys) {
        serial_write("[ELF] Cannot get physical address of stack\n");
        result.entry_point = 0;   /* нельзя пускать процесс без стека */
        return result;
    }
    
    uint64_t stack_vaddr = 0x800000;
    uint64_t stack_kbase = (uint64_t)stack_buf & ~0xFFFULL;
    for (int i = 0; i < 4; i++) {
        uint64_t spage_phys = vmm_virt_to_phys(vmm_kernel_pml4, stack_kbase + i * 4096);
        if (!spage_phys) {
            serial_write("[ELF] WARN: stack page phys=0\n");
            continue;
        }
        vmm_map(user_pml4, stack_vaddr + i * 4096, spage_phys,
                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }
    serial_write("[ELF] User stack mapped at 0x800000\n");

    return result;
}

/* =============================================================================
 * pf_demand_load — обработчик page fault от idt.c.
 *
 * Вызывается ТОЛЬКО для user-mode #PF (CPL=3). Если cr2 попадает в один из
 * VMA текущей задачи — выделяем физическую страницу через pmm_alloc, читаем
 * нужный кусок файла (или зануляем, если за filesz), мапим в user_pml4.
 * После return обратно в idt.c инструкция CPU перезапускается и теперь
 * видит замапленную страницу.
 *
 * Не наша зона ответственности (cr2 вне всех VMA / err указывает на
 * нарушение прав, а не на отсутствующую страницу) — возвращаем -1, и
 * idt.c убивает задачу как раньше.
 * ============================================================================= */
int pf_demand_load(task_t *t, uint64_t cr2, uint64_t err) {
    if (!t || !t->elf_node) return -1;
    /* Бит 0 err: 0 = страница не present (наш случай), 1 = protection fault.
     * Protection fault (запись в read-only и т.п.) — это уже не demand load. */
    if (err & 0x1) return -1;
    /* Бит 3 err: reserved-bit set — баг в таблицах, не наш случай. */
    if (err & 0x8) return -1;

    uint64_t page_va = cr2 & ~0xFFFULL;

    for (uint8_t i = 0; i < t->n_vmas; i++) {
        elf_vma_t *v = &t->vmas[i];
        if (page_va < v->vaddr_start || page_va >= v->vaddr_end) continue;

        uint64_t phys = pmm_alloc();
        if (!phys) return -1;

        /* Запись в свежую страницу через HHDM (всё физическое отображено
         * по +HHDM_OFFSET, см. vmm_init). */
        uint8_t *kbuf = (uint8_t *)(phys + HHDM_OFFSET);
        mem_memset(kbuf, 0, 4096);

        /* Загружаем содержимое файла, если эта страница не целиком в BSS. */
        if (page_va < v->file_end_vaddr) {
            uint64_t file_off = v->file_offset + (page_va - v->vaddr_start);
            uint64_t bytes = 4096;
            if (page_va + 4096 > v->file_end_vaddr)
                bytes = v->file_end_vaddr - page_va;
            int32_t got = vfs_read((vfs_node_t *)t->elf_node,
                                   (uint32_t)file_off,
                                   (uint32_t)bytes,
                                   kbuf);
            if (got < 0) {
                pmm_free(phys);
                return -1;
            }
            /* Хвост страницы за filesz уже занулён memset'ом выше. */
        }

        vmm_map((pte_t *)t->pml4, page_va, phys, v->flags);
        return 0;
    }
    return -1;
}
