#include "elf.h"
#include "vfs.h"
#include "heap.h"
#include "vmm.h"
#include <stddef.h>

extern void* kmalloc(uint64_t size);
extern void* kmalloc_aligned(uint64_t size, uint64_t align);
extern void fb_puts(const char *s);
extern pte_t *vmm_kernel_pml4;

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

    // Open file through VFS
    vfs_node_t *node = vfs_open(path, 0);
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

    // Load program headers
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);
        
        if (vfs_read(node, offset, sizeof(Elf64_Phdr), (uint8_t*)&phdr) != sizeof(Elf64_Phdr)) {
            serial_write("[ELF] Error: Could not read program header\n");
            continue;
        }

        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_memsz == 0) continue;

        serial_write("[ELF] Loading segment: vaddr=");
        // Simple hex print
        char hexbuf[20];
        uint64_t addr = phdr.p_vaddr;
        int pos = 0;
        hexbuf[pos++] = '0';
        hexbuf[pos++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4) {
            int digit = (addr >> shift) & 0xF;
            hexbuf[pos++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }
        hexbuf[pos] = '\0';
        serial_write(hexbuf);
        serial_write("\n");

        // Calculate alignment offset within page
        uint64_t vaddr_offset = phdr.p_vaddr & 0xFFF;
        
        // Allocate physical memory for segment, page-aligned
        // Add extra space for alignment offset
        void *segment_phys = kmalloc_aligned(phdr.p_memsz + vaddr_offset + 4096, 4096);
        if (!segment_phys) {
            serial_write("[ELF] Error: Out of memory\n");
            vfs_close(node);
            return result;
        }

        // Zero out entire allocation (for BSS and alignment padding)
        mem_memset(segment_phys, 0, phdr.p_memsz + vaddr_offset);

        // Load file data if present - IMPORTANT: add vaddr_offset!
        if (phdr.p_filesz > 0) {
            if (vfs_read(node, phdr.p_offset, phdr.p_filesz, (uint8_t*)segment_phys + vaddr_offset) != phdr.p_filesz) {
                serial_write("[ELF] Error: Could not read segment data\n");
                vfs_close(node);
                return result;
            }
        }

        // Get physical address via page table walk (correct for heap memory)
        // NOTE: vmm_kernel_virt_to_phys uses HHDM arithmetic which is WRONG for heap
        // Heap is mapped via pmm_alloc, not HHDM. Use page table walk instead.
        uint64_t segment_virt = (uint64_t)segment_phys;
        // segment_phys was allocated with kmalloc_aligned(4096) so it IS page-aligned
        // Walk the kernel page table to find the real physical address
        uint64_t segment_phys_raw = vmm_virt_to_phys(vmm_kernel_pml4, segment_virt);
        if (!segment_phys_raw) {
            serial_write("[ELF] Error: vmm_virt_to_phys returned 0 for heap buffer!\n");
            vfs_close(node);
            return result;
        }
        uint64_t segment_phys_aligned = segment_phys_raw; // Already page-aligned
        
        serial_write("[ELF] segment_virt=");
        char svbuf[20];
        int svp = 0;
        svbuf[svp++] = '0';
        svbuf[svp++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4) {
            int digit = (segment_virt >> shift) & 0xF;
            svbuf[svp++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }
        svbuf[svp] = '\0';
        serial_write(svbuf);
        
        serial_write(", segment_phys_raw=");
        char sprb[20];
        int spr = 0;
        sprb[spr++] = '0';
        sprb[spr++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4) {
            int digit = (segment_phys_raw >> shift) & 0xF;
            sprb[spr++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }
        sprb[spr] = '\0';
        serial_write(sprb);
        
        serial_write(", aligned=");
        char spab[20];
        int spa = 0;
        spab[spa++] = '0';
        spab[spa++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4) {
            int digit = (segment_phys_aligned >> shift) & 0xF;
            spab[spa++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }
        spab[spa] = '\0';
        serial_write(spab);
        serial_write("\n");
        
        // Map to userspace vaddr
        uint64_t vaddr_start = phdr.p_vaddr & ~0xFFF; // Page align
        // vaddr_offset already calculated above
        
        serial_write("[ELF] Mapping ");
        
        // Calculate number of pages needed (INCLUDING offset!)
        size_t total_size = phdr.p_memsz + vaddr_offset;
        size_t num_pages = (total_size + 4095) / 4096;
        
        char npbuf[20];
        int np = 0;
        npbuf[np++] = '0' + (num_pages / 10);
        npbuf[np++] = '0' + (num_pages % 10);
        npbuf[np] = '\0';
        serial_write(npbuf);
        serial_write(" pages, offset=");
        char obuf[20];
        int op = 0;
        obuf[op++] = '0';
        obuf[op++] = 'x';
        for (int shift = 12; shift >= 0; shift -= 4) {
            int digit = (vaddr_offset >> shift) & 0xF;
            obuf[op++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        }
        obuf[op] = '\0';
        serial_write(obuf);
        serial_write("\n");
        
        for (size_t p = 0; p < num_pages; p++) {
            uint64_t vaddr = vaddr_start + (p * 4096);
            /* Получаем физический адрес каждой страницы отдельно — куча
             * не гарантирует физическую непрерывность страниц. */
            uint64_t kvirt_page = (segment_virt & ~0xFFFULL) + (p * 4096);
            uint64_t paddr = vmm_virt_to_phys(vmm_kernel_pml4, kvirt_page);
            if (!paddr) {
                serial_write("  [ELF] WARN: vmm_virt_to_phys=0 for page ");
                serial_write("\n");
                continue;
            }
            
            serial_write("  [ELF] Map page vaddr=");
            char vbuf[20];
            int vp = 0;
            vbuf[vp++] = '0';
            vbuf[vp++] = 'x';
            for (int shift = 60; shift >= 0; shift -= 4) {
                int digit = (vaddr >> shift) & 0xF;
                vbuf[vp++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            }
            vbuf[vp] = '\0';
            serial_write(vbuf);
            
            serial_write(" -> paddr=");
            char pbuf[20];
            int pp = 0;
            pbuf[pp++] = '0';
            pbuf[pp++] = 'x';
            for (int shift = 60; shift >= 0; shift -= 4) {
                int digit = (paddr >> shift) & 0xF;
                pbuf[pp++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
            }
            pbuf[pp] = '\0';
            serial_write(pbuf);
            serial_write("\n");
            
            // Map into USER page table as user-accessible
            vmm_map(user_pml4, vaddr, paddr, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
            
            // DEBUG: Verify the mapping has USER flag
            uint64_t verify_phys = vmm_virt_to_phys(user_pml4, vaddr);
            if (!verify_phys) {
                serial_write("  [ELF] ERROR: Just-mapped page not visible!\n");
            }
        }

        serial_write("[ELF] Mapped to userspace vaddr\n");
    }

    vfs_close(node);
    
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
    uint8_t *stack_buf = kmalloc_aligned(16384, 4096);
    if (!stack_buf) {
        serial_write("[ELF] Failed to allocate user stack\n");
        return result;
    }
    
    uint64_t stack_phys = vmm_virt_to_phys(vmm_kernel_pml4, (uint64_t)stack_buf);
    if (!stack_phys) {
        serial_write("[ELF] Cannot get physical address of stack\n");
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
