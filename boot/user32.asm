; CPL3 transition and full-frame continuation; kernel-owned runtime only.
bits 32
extern ku_saved_sp
extern ku_kernel_cr3
global ku_mode_enter
global ku_mode_leave
ku_mode_enter:
    pushfd
    push ebp
    push ebx
    push esi
    push edi
    mov [ku_saved_sp],esp
    mov eax,[esp+24] ; entry
    mov edx,[esp+28] ; user stack
    mov ecx,[esp+32] ; private root
    mov cr3,ecx
    mov bx,0x23
    mov ds,bx
    mov es,bx
    mov fs,bx
    mov gs,bx
    push dword 0x23
    push edx
    push dword 0x202
    push dword 0x1b
    push eax
    iretd
global ku_mode_resume
ku_mode_resume:
    pushfd
    push ebp
    push ebx
    push esi
    push edi
    mov [ku_saved_sp],esp
    mov edx,[esp+24]
    mov eax,[esp+28]
    mov cr3,eax
    mov esp,edx
    pop eax
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    popa
    add esp,8
    iretd
ku_mode_leave:
    cli
    mov ax,0x10
    mov ss,ax
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov eax,[ku_kernel_cr3]
    mov cr3,eax
    mov esp,[ku_saved_sp]
    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
