#ifndef VOS_IDT_H
#define VOS_IDT_H

#include "types.h"

#define IDT_INTERRUPT_GATE  0x8E
#define IDT_TRAP_GATE       0x8F

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

/*
 * Фрейм стека после всех push в isr_common (ring0→ring0, без fxsave).
 * rsp указывает на r15.
 *
 * Смещения от rsp:
 *   +0   r15
 *   +8   r14
 *   ...
 *   +112 rax
 *   +120 vector
 *   +128 error
 *   +136 rip    <- CPU
 *   +144 cs
 *   +152 rflags
 */
/* Точно как registers_t в VortexOS — всегда 5 qword для iretq (rip,cs,rflags,rsp,ss) */
typedef struct __attribute__((packed, aligned(16))) {
    uint8_t  fxsave_region[512];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_t;

void     idt_init(void);
void     idt_set_handler(uint8_t vector, void *handler, uint8_t type);
void     irq_register(uint8_t irq, void (*handler)(interrupt_frame_t *));
uint64_t interrupt_handler(interrupt_frame_t *frame);

#endif
