; ============================================================
;  KvantOS - entry point, Multiboot 1 header
; ============================================================
bits 32

MB_MAGIC    equ 0x1BADB002
; bit0 - module alignment, bit1 - memory map, bit2 - video mode
MB_FLAGS    equ 0x00000007
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd 0, 0, 0, 0, 0        ; a.out fields (unused, this is ELF)
    dd 0                    ; mode_type: 0 = linear framebuffer
    ; IMPORTANT: for a Multiboot kernel GRUB takes the mode from HERE,
    ; not from gfxpayload in grub.cfg. Left at zero it picks a modest
    ; 800x600, so we ask for 1024x768 - a mode present in the VBE tables
    ; of virtually every adapter, including the Intel GMA 4500M.
    ; Should the adapter refuse, the kernel survives without a
    ; framebuffer and falls back to the text console (see have_fb in
    ; main.c).
    dd 1024                 ; desired width
    dd 768                  ; desired height
    dd 32                   ; desired colour depth

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
    mov esp, kernel_stack_top
    mov ebp, 0                      ; end of the frame chain
    push ebx                        ; pointer to the multiboot info
    push eax                        ; magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- reloading the GDT ----
global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload
.reload:
    ret

global tss_flush
tss_flush:
    mov ax, 0x28
    ltr ax
    ret

; ---- loading the IDT ----
global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; ---- enabling paging ----
global paging_enable
paging_enable:
    mov eax, [esp+4]
    mov cr3, eax
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax
    ret

global read_cr2
read_cr2:
    mov eax, cr2
    ret

; ---- task context switching ----
; void context_switch(uint32_t *old_esp, uint32_t new_esp)
global context_switch
context_switch:
    mov eax, [esp+4]
    mov edx, [esp+8]
    push ebp
    push ebx
    push esi
    push edi
    pushfd
    mov [eax], esp
    mov esp, edx
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
