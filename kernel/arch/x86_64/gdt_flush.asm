; =============================================================================
; VortexOS — kernel/arch/x86_64/gdt_flush.asm
; Загружает GDTR, перезагружает сегментные регистры и TR.
;
; Вызывается из gdt.c:
;   extern void gdt_flush(uint64_t gdt_ptr_addr);
;   extern void tss_flush(void);
; =============================================================================

bits 64
section .text

global gdt_flush
global tss_flush

; Селекторы (должны совпадать с gdt.c)
KERNEL_CODE_SEL equ 0x08
KERNEL_DATA_SEL equ 0x10
TSS_SEL         equ 0x28        ; слот 5 * 8 = 40 = 0x28

; -----------------------------------------------------------------------------
; gdt_flush(uint64_t gdt_ptr_addr)
;   RDI = адрес структуры gdt_ptr_t { uint16_t limit; uint64_t base; }
; -----------------------------------------------------------------------------
gdt_flush:
    lgdt [rdi]              ; загружаем GDTR

    ; Перезагружаем CS через дальний ret (прямой far jump в 64-bit NASM неудобен)
    ; Кладём на стек: новый CS и адрес метки .reload_cs
    push KERNEL_CODE_SEL    ; новый Code Segment
    lea  rax, [rel .reload_cs]
    push rax
    retfq                   ; far return — меняет CS:RIP

.reload_cs:
    ; Перезагружаем остальные сегментные регистры
    mov ax, KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; -----------------------------------------------------------------------------
; tss_flush — загружает TR (Task Register) селектором TSS
; -----------------------------------------------------------------------------
tss_flush:
    mov ax, TSS_SEL
    ltr ax                  ; Load Task Register
    ret
