#ifndef TSS_H
#define TSS_H

#include <stdint.h>

// TSS structure for x86_64
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;       // Kernel stack pointer (ring 0)
    uint64_t rsp1;       // Ring 1 stack (unused)
    uint64_t rsp2;       // Ring 2 stack (unused)
    uint64_t reserved1;
    uint64_t ist[7];     // Interrupt Stack Table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset; // I/O permission bitmap offset
} tss_t;

void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);

#endif
