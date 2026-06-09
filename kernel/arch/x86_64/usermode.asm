; =============================================================================
; usermode.asm — Переключение в ring 3 (user mode)
; =============================================================================

bits 64
section .text

global enter_usermode

; Селекторы для user mode (GDT: index3=udata, index4=ucode)
USER_DATA_SEL equ 0x1B  ; GDT entry 3 (udata), RPL=3 -> 3*8 | 3 = 0x1B
USER_CODE_SEL equ 0x23  ; GDT entry 4 (ucode), RPL=3 -> 4*8 | 3 = 0x23

; -----------------------------------------------------------------------------
; void enter_usermode(uint64_t entry_point, uint64_t user_stack)
;   RDI = entry_point (адрес функции в userspace)
;   RSI = user_stack  (указатель на userspace стек)
;
; Использует iretq для перехода в ring 3:
;   SS, RSP, RFLAGS, CS, RIP должны быть на стеке
; -----------------------------------------------------------------------------
enter_usermode:
    cli                     ; отключаем прерывания

    mov rcx, rsi            ; user_stack -> RCX
    mov rdx, rdi            ; entry_point -> RDX

    ; Подготовка стека для iretq (формат: SS, RSP, RFLAGS, CS, RIP)
    push USER_DATA_SEL      ; SS (user data segment)
    push rcx                ; RSP (user stack)
    
    ; RFLAGS: IF=1 (прерывания включены в usermode)
    pushfq
    pop rax
    or rax, 0x200           ; установить IF (бит 9)
    push rax
    
    push USER_CODE_SEL      ; CS (user code segment)
    push rdx                ; RIP (entry point)

    ; Обнуляем регистры для безопасности
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; Переключаемся в ring 3
    iretq
