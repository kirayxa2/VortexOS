; =============================================================================
; syscall_entry.asm — точка входа для SYSCALL, возврат через iretq
; =============================================================================

bits 64
section .data
align 8
global syscall_kernel_stack
syscall_kernel_stack: dq 0
syscall_user_rsp: dq 0

section .text
global syscall_entry
extern syscall_dispatch

syscall_entry:
    ; rcx = RIP возврата, r11 = RFLAGS, rsp = user RSP
    
    ; Переключаемся на kernel stack
    mov [rel syscall_user_rsp], rsp    ; Сохраняем user RSP в памяти
    mov rsp, [rel syscall_kernel_stack]
    
    ; Строим iretq-фрейм (SS, RSP, RFLAGS, CS, RIP)
    push 0x1B                           ; SS (user data)
    push qword [rel syscall_user_rsp]   ; user RSP
    push r11                            ; RFLAGS
    push 0x23                           ; CS (user code)
    push rcx                            ; RIP
    
    ; Сохраняем все регистры (кроме rax - его перезапишем результатом)
    push rbx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15
    
    ; Вызываем C: syscall_dispatch(num, a1, a2, a3, a4, a5, a6)
    ; У нас: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
    ; Нужно: rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, stack=a6
    mov rcx, rdx        ; a3 -> rcx
    mov rdx, rsi        ; a2 -> rdx
    mov rsi, rdi        ; a1 -> rsi
    mov rdi, rax        ; num -> rdi
    ; r8, r9, r10 уже на месте для первых 6 аргументов
    ; Но по System V ABI первые 6 аргументов: rdi, rsi, rdx, rcx, r8, r9
    ; У нас нужно передать 7 аргументов, 7-й идет на стек
    push r9             ; a6 на стек
    mov r9, r8          ; a5 -> r9
    mov r8, r10         ; a4 -> r8
    ; rcx, rdx, rsi, rdi уже установлены выше
    call syscall_dispatch
    add rsp, 8          ; Очистка стека от a6
    
    ; Результат в rax - НЕ трогаем его!
    
    ; Восстанавливаем регистры
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    
    ; Возвращаемся через iretq
    iretq
