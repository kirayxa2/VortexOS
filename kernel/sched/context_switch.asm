; =============================================================================
; VOS — kernel/sched/context_switch.asm
; =============================================================================
bits 64

global context_switch_to

; void context_switch_to(uint64_t rsp)  [rdi = saved_rsp задачи]
; Точно как в VortexOS/src/arch/process_asm.asm
context_switch_to:
    mov  rsp, rdi

    fxrstor [rsp]
    add  rsp, 512

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16        ; drop int_no + err_code
    iretq
