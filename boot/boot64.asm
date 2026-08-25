; ============================================================
;  KvantOS 3.0 - 64-bit entry (UEFI path)
;
;  The UEFI loader (boot/kvantefi.c) has already switched the
;  CPU into long mode, identity-mapped the memory and exited
;  the boot services. It jumps to _start with:
;      rdi = the Multiboot magic (0x2BADB002)
;      rsi = physical address of the multiboot_info structure
;  Everything else is exactly like the 32-bit boot: kmain gets
;  the magic and the info pointer, the rest is C.
; ============================================================
bits 64

section .bss
align 16
global kernel_stack_bottom
global kernel_stack_top
kernel_stack_bottom:
    resb 65536                      ; 64 KiB kernel stack
kernel_stack_top:

section .text
global _start
extern kmain

_start:
    cli
    mov rsp, kernel_stack_top
    xor ebp, ebp                    ; end of the frame chain
    ; rdi/rsi already carry magic / multiboot_info
    call kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- reloading the GDT (called from gdt.c, rdi = pointer) ----
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    lea rax, [rel .reload]
    push qword 0x08                 ; 64-bit code selector
    push rax
    o64 retf                        ; far return reloads CS
.reload:
    ret

global tss_flush
tss_flush:
    ltr di
    ret

; ---- loading the IDT ----
global idt_flush
idt_flush:
    lidt [rdi]
    ret

; ---- paging helpers ----
global paging_enable
paging_enable:
    mov cr3, rdi
    ret

global read_cr2
read_cr2:
    mov rax, cr2
    ret

; ---- scheduler: save one stack, load the other ----
; void context_switch(kv_addr_t *old_rsp, kv_addr_t new_rsp)
; The frame must match task_create: r15,r14,r13,r12,rbx,rbp,rflags
global context_switch
context_switch:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret
