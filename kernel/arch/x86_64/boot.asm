; =============================================================================
; VortexOS — boot.asm
; Точка входа под Limine протокол.
; Limine уже в long mode, уже настроил стек — просто вызываем kmain().
; Сборка: nasm -f elf64 boot.asm -o boot.o
; =============================================================================

section .text
bits 64
global _start
extern kmain

_start:
    cli
    call kmain
.halt:
    cli
    hlt
    jmp .halt
