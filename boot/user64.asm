; CPL3 transition and full-frame continuation; kernel-owned runtime only.
bits 64
extern ku_saved_sp
extern ku_kernel_cr3
global ku_mode_enter
global ku_mode_leave
ku_mode_enter:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rel ku_saved_sp],rsp
    mov cr3,rdx
    mov ax,0x23
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1b
    push rdi
    iretq
global ku_mode_resume
ku_mode_resume:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rel ku_saved_sp],rsp
    mov cr3,rsi
    mov ax,0x23
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov rsp,rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp,16
    iretq
ku_mode_leave:
    cli
    mov ax,0x10
    mov ss,ax
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov rax,[rel ku_kernel_cr3]
    mov cr3,rax
    mov rsp,[rel ku_saved_sp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
