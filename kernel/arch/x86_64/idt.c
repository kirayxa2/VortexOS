/* =============================================================================
 * VortexOS — kernel/arch/x86_64/idt.c
 * Архитектура: interrupt_handler возвращает uint64_t (новый rsp).
 * Если rsp не меняется — та же задача. Если меняется — переключение.
 * Это точная копия схемы из рабочего проекта VortexOS.
 * ============================================================================= */

#include "idt.h"
#include "fb.h"
#include "types.h"
#include "sched.h"
#include "serial.h"

#define KERNEL_CS 0x08
#define IDT_ENTRIES 256

static idt_entry_t idt[IDT_ENTRIES];
static idtr_t      idtr;

extern void *isr_stub_table[IDT_ENTRIES];
extern void  idt_flush(idtr_t *);

static const char *exception_names[] = {
    "Division By Zero",       "Debug",
    "NMI",                    "Breakpoint",
    "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",         "Device Not Available",
    "Double Fault",           "Coprocessor Segment Overrun",
    "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",    "General Protection Fault",
    "Page Fault",             "Reserved",
    "x87 FPU Error",          "Alignment Check",
    "Machine Check",          "SIMD FP Exception",
    "Virtualization Exception","Control Protection Exception",
};

static void (*irq_handlers[16])(interrupt_frame_t *) = {0};

void irq_register(uint8_t irq, void (*handler)(interrupt_frame_t *)) {
    if (irq < 16) irq_handlers[irq] = handler;
}

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void io_wait(void) { outb(0x80, 0); }

static void pic_remap(void) {
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
}

static void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static void print_hex(uint64_t v) {
    fb_puts("0x");
    char buf[17]; buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t n = v & 0xF;
        buf[i] = n < 10 ? '0'+n : 'a'+n-10;
        v >>= 4;
    }
    fb_puts(buf);
}

/* serial-only hex: НИКОГДА не трогает framebuffer — безопасно в panic-пути,
 * даже если активный CR3 вообще не содержит маппинга fb. */
static void serial_hex(uint64_t v) {
    serial_puts("0x");
    char buf[17]; buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t n = v & 0xF;
        buf[i] = n < 10 ? '0'+n : 'a'+n-10;
        v >>= 4;
    }
    serial_puts(buf);
}

/* Полный дамп исключения ТОЛЬКО в COM1 (port I/O, не может сфолтить).
 * Урок чёрного экрана 2026-06-12: старый обработчик начинал с fb_puts,
 * fb-запись фолтила (CR3 без маппинга fb / битые таблицы) → вложенный #PF →
 * снова fb_puts → бесконечный поток "[" в serial и ноль информации. */
static void serial_dump_exception(interrupt_frame_t *frame) {
    uint64_t vec = frame->int_no;
    uint64_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    serial_puts("\n[EXCEPTION] vec=");
    serial_hex(vec);
    serial_puts(" ");
    if (vec < 22) serial_puts(exception_names[vec]);
    else          serial_puts("Reserved");
    serial_puts("\n  err="); serial_hex(frame->err_code);
    serial_puts(" rip=");    serial_hex(frame->rip);
    serial_puts(" cs=");     serial_hex(frame->cs);
    serial_puts("\n  rsp="); serial_hex(frame->rsp);
    serial_puts(" rflags="); serial_hex(frame->rflags);
    serial_puts("\n  cr2="); serial_hex(cr2);
    serial_puts(" cr3=");    serial_hex(cr3);
    serial_puts("\n  rax="); serial_hex(frame->rax);
    serial_puts(" rbx=");    serial_hex(frame->rbx);
    serial_puts(" rcx=");    serial_hex(frame->rcx);
    serial_puts("\n  rdx="); serial_hex(frame->rdx);
    serial_puts(" rsi=");    serial_hex(frame->rsi);
    serial_puts(" rdi=");    serial_hex(frame->rdi);
    serial_puts("\n  rbp="); serial_hex(frame->rbp);
    serial_puts(" r8=");     serial_hex(frame->r8);
    serial_puts(" r9=");     serial_hex(frame->r9);

    task_t *t = sched_current();
    if (t) {
        serial_puts("\n  task pid=");
        serial_hex(t->pid);
        serial_puts(" name=");
        serial_puts(t->name);
    }
    serial_puts("\n");
}

/*
 * interrupt_handler — вызывается из isr_common в ASM.
 * Принимает: rdi = rsp (указатель на interrupt_frame_t, включая fxsave).
 * Возвращает: uint64_t — новый rsp (для переключения задачи).
 *   Если возвращаем то же значение — остаёмся в текущей задаче.
 *   Если возвращаем другое — ASM делает mov rsp, rax → переключение.
 */
uint64_t interrupt_handler(interrupt_frame_t *frame) {
    uint64_t vec = frame->int_no;

    if (vec < 32) {
        /* Исключение.
         * ПОРЯДОК КРИТИЧЕН: сначала ВЕСЬ дамп в COM1 (только port I/O — не
         * может сфолтить), и только потом попытка вывода на экран. Старый
         * код начинал с fb_puts: если fb-запись фолтит, обработчик
         * рекурсивно падал на 2-м символе → бесконечные "[" в serial. */
        static volatile int in_exception = 0;

        if (in_exception) {
            /* Вложенный фолт = упал сам panic-путь (почти наверняка fb).
             * Минимальный serial-дамп и стоп — без рекурсии. */
            uint64_t cr2n;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2n));
            serial_puts("\n[EXCEPTION-NESTED] vec=");
            serial_hex(vec);
            serial_puts(" rip="); serial_hex(frame->rip);
            serial_puts(" cr2="); serial_hex(cr2n);
            serial_puts(" — panic path faulted (fb unmapped in this CR3?), halting\n");
            __asm__ volatile("cli");
            for (;;) __asm__ volatile("hlt");
        }
        in_exception = 1;

        serial_dump_exception(frame);

        /* Фолт из ring3: убиваем ТОЛЬКО этот процесс, а не всю машину.
         * Мы на kernel-стеке этой задачи (TSS.RSP0) — ровно тот же контекст,
         * из которого sys_exit зовёт task_exit, так что это безопасно.
         * vinit увидит CHILD_EXIT и (для сервисов с restart=yes) перезапустит. */
        if ((frame->cs & 3) == 3) {
            task_t *t = sched_current();
            serial_puts("[EXCEPTION] user-mode fault -> killing task\n");
            if (t) t->exit_code = 128 + (uint64_t)vec;
            in_exception = 0;
            task_exit();              /* не возвращается */
        }

        /* Фолт в ядре — фатально. Пробуем продублировать на экран (может и
         * сфолтить — тогда сработает ветка in_exception выше, но полный дамп
         * уже ушёл в COM1). */
        fb_puts("\n[EXCEPTION] ");
        if (vec < 22) fb_puts(exception_names[vec]);
        else fb_puts("Reserved");
        fb_puts(" err="); print_hex(frame->err_code);
        fb_puts(" rip="); print_hex(frame->rip);
        fb_puts(" cs=");  print_hex(frame->cs);
        fb_puts("\n");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        if (irq_handlers[irq])
            irq_handlers[irq](frame);
        pic_eoi(irq);
    }

    /* Планировщик: если нужна смена задачи — возвращаем новый rsp */
    if (vos_need_resched) {
        return sched_pick((uint64_t)frame);
    }

    return (uint64_t)frame;
}

void idt_set_handler(uint8_t vector, void *handler, uint8_t type) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = KERNEL_CS;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = type;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].zero        = 0;
}

void idt_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_handler((uint8_t)i, isr_stub_table[i], IDT_INTERRUPT_GATE);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    pic_remap();
    outb(PIC1_DATA, 0xF8);  /* 11111000 — открыты IRQ0, IRQ1, IRQ2 (как в VortexOS) */
    outb(PIC2_DATA, 0xEF);  /* 11101111 — открыт IRQ12 (мышь) */

    idt_flush(&idtr);
    __asm__ volatile("sti");
}
