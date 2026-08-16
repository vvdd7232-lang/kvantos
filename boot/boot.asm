; ============================================================
;  KvantOS - точка входа, Multiboot 1 заголовок
; ============================================================
bits 32

MB_MAGIC    equ 0x1BADB002
; bit0 - выравнивание модулей, bit1 - карта памяти, bit2 - видеорежим
MB_FLAGS    equ 0x00000007
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd 0, 0, 0, 0, 0        ; поля a.out (не используются, ELF)
    dd 0                    ; mode_type: 0 = линейный фреймбуфер
    ; ВАЖНО: для Multiboot-ядра GRUB берёт режим ИМЕННО отсюда, а не из
    ; gfxpayload в grub.cfg. При нулях он выбирает скромные 800x600,
    ; поэтому запрашиваем 1024x768 - режим есть в таблицах VBE
    ; практически любой карты, включая Intel GMA 4500M.
    ; Если карта его не даст, ядро переживёт отсутствие фреймбуфера
    ; и уйдёт в текстовую консоль (см. have_fb в main.c).
    dd 1024                 ; желаемая ширина
    dd 768                  ; желаемая высота
    dd 32                   ; желаемая глубина цвета

section .bss
align 16
global kernel_stack_bottom
global kernel_stack_top
kernel_stack_bottom:
    resb 65536                      ; 64 KiB стек ядра
kernel_stack_top:

section .text
global _start
extern kmain

_start:
    cli
    mov esp, kernel_stack_top
    mov ebp, 0                      ; конец цепочки кадров
    push ebx                        ; указатель на multiboot info
    push eax                        ; magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- перезагрузка GDT ----
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

; ---- загрузка IDT ----
global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; ---- включение страничной адресации ----
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

; ---- переключение контекста задач ----
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
