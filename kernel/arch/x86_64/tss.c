#include "tss.h"
#include "gdt.h"
#include <stddef.h>

extern void fb_puts(const char *s);

static tss_t tss;

void tss_init(void) {
    // Zero out TSS
    uint8_t *ptr = (uint8_t*)&tss;
    for (size_t i = 0; i < sizeof(tss_t); i++) {
        ptr[i] = 0;
    }
    
    // Set I/O permission bitmap offset (no I/O permissions)
    tss.iopb_offset = sizeof(tss_t);
    
    // Install TSS descriptor in GDT
    gdt_install_tss((uint64_t)&tss);
    
    fb_puts("[OK] TSS initialized\n");
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}
