; ============================================================
;  KvantOS - direct boot stub (no GRUB needed)
;
;  A tiny self-contained Multiboot boot loader for the ISO's
;  El Torito image. The BIOS loads the first two sectors at
;  0x7C00 (no-emulation mode). The second sector holds a read
;  table: {LBA, sector count, destination} for every kernel
;  segment and every .kapp module. The stub:
;    1) enables the A20 line,
;    2) reads the E820 memory map,
;    3) asks VBE for a 1024x768x32 linear framebuffer
;       (exactly what the Multiboot header requests from GRUB),
;    4) loads the segments to 1 MiB and the modules to 2 MiB
;       with INT 13h/AH=42h,
;    5) enters protected mode, zeroes BSS, builds multiboot_info
;       and jumps to the kernel.
;
;  Offsets, sizes and the read table are injected by
;  tools/mkdirect.py through nasm -D definitions and the
;  generated include files direct-*.inc.
; ============================================================

bits 16
org 0x7C00

STACK_TOP   equ 0x7F00
E820_BUF    equ 0x80000
E820_MAX    equ 64
VBINFO      equ 0x81000
MODEINFO    equ 0x82000
; real-mode segment forms of the buffers above (16-bit instructions
; cannot encode a 20-bit displacement without a prefix, so the real
; mode part addresses them as seg:off)
E820_SEG    equ 0x8000
VB_SEG      equ 0x8100
MI_SEG      equ 0x8200
MBI         equ 0x90000
MODS_TAB    equ 0x90100
MMAP_TAB    equ 0x92000
STR_AREA    equ 0x91000
MB_MAGIC    equ 0x2BADB002
READ_TABLE  equ 0x8200              ; fourth sector of the loaded stub
COPY_TABLE  equ 0x8280              ; bounce -> final addresses, same sector
DAP         equ 0x6000
BOUNCE      equ 0x10000             ; INT 13h can only address seg:off
                                    ; (up to ~1.06 MiB), so everything is
                                    ; read here first and copied to the
                                    ; final addresses in protected mode

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    sti
    mov byte [boot_drive], dl

    mov si, msg_hello
    call say

    ; ---- A20: BIOS method first, port 0x92 as fallback ----
    mov ax, 0x2401
    int 0x15
    jc .a20_port
    test ah, ah
    jz .a20_done
.a20_port:
    in al, 0x92
    or al, 2
    and al, 0xFE                    ; keep fast reset OFF
    out 0x92, al
.a20_done:

    ; ---- E820 memory map ----
    mov ax, E820_SEG
    mov es, ax
    xor di, di
    mov word [e820_di], 0
    mov dword [e820_cnt], 0
    xor ebx, ebx
.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150             ; 'SMAP'
    mov di, word [e820_di]
    int 0x15
    jc .e820_end
    cmp eax, 0x534D4150
    jne .e820_end
    add word [e820_di], 24
    inc dword [e820_cnt]
    cmp dword [e820_cnt], E820_MAX
    jae .e820_end
    test ebx, ebx
    jnz .e820_loop
.e820_end:

    ; ---- VBE: find a 1024x768x32 linear mode ----
    xor cx, cx                      ; 0 = no mode found yet
    mov ax, VB_SEG
    mov es, ax
    xor di, di
    mov dword [es:di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_nomode
    ; iterate the mode list (far pointer at VbeInfoBlock+14)
    les di, [es:di + 14]
    mov word [mode_seg], es
    mov word [mode_off], di
.vbe_scan:
    mov es, word [mode_seg]
    mov di, word [mode_off]
    mov cx, word [es:di]
    add di, 2
    mov word [mode_off], di
    cmp cx, 0xFFFF
    je .vbe_nomode
    test cx, cx
    jz .vbe_nomode
    push cx
    mov ax, MI_SEG
    mov es, ax
    xor di, di
    mov ax, 0x4F01
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .vbe_scan
    ; ES still points at the ModeInfoBlock; INT 10h may clobber DI
    xor di, di
    mov ax, word [es:di]            ; ModeAttributes
    test ax, 0x0011                 ; supported + graphics mode
    jz .vbe_scan
    test ax, 0x0080                 ; linear framebuffer available
    jz .vbe_scan
    cmp byte [es:di + 27], 6        ; direct colour memory model
    jne .vbe_scan
    cmp byte [es:di + 25], 32       ; bits per pixel
    jne .vbe_scan
    cmp word [es:di + 18], 1024     ; X resolution
    jne .vbe_scan
    cmp word [es:di + 20], 768      ; Y resolution
    jne .vbe_scan
    jmp .vbe_found
.vbe_nomode:
    xor cx, cx
.vbe_found:
    mov word [vbe_mode_num], cx
    test cx, cx
    jz .vbe_text
    mov bx, cx
    or bx, 0x4000                   ; enable the LFB
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .vbe_ok
    mov word [vbe_mode_num], 0
.vbe_text:
    mov si, msg_text
    call say
.vbe_ok:

    ; ---- read the kernel segments and the modules into the
    ;      bounce area (INT 13h cannot reach above ~1.06 MiB) ----
    mov esi, READ_TABLE
    mov ecx, dword [esi]            ; entry count
    add esi, 4
.read_next:
    test ecx, ecx
    jz .read_done
    mov eax, dword [esi]            ; LBA
    mov ebx, dword [esi + 4]        ; sectors
    mov edi, dword [esi + 8]        ; bounce destination
    call read_sectors
    add esi, 12
    dec ecx
    mov si, msg_dot                 ; progress dot (SI only)
    call say
    jmp .read_next
.read_done:

    ; ---- protected mode ----
    mov si, msg_pm
    call say
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

msg_hello   db 'KvantOS direct boot v2', 13, 10, 0
msg_text    db 'no VBE mode, text console', 13, 10, 0
msg_pm      db 'protected mode..', 13, 10, 0
msg_dot     db '.', 0
msg_ioerr   db 'disk read error', 13, 10, 0
boot_drive   db 0
vbe_mode_num dw 0
e820_di  dw 0
e820_cnt dd 0
mode_seg dw 0
mode_off dw 0

; real-mode teletype
say:
    lodsb
    test al, al
    jz .done
    cmp al, 13
    je .skip_ch
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
.skip_ch:
    jmp say
.done:
    ret

; read EBX sectors starting at EAX into linear EDI, 16 chunks
read_sectors:
.rs_loop:
    test ebx, ebx
    jz .rs_done
    mov ebp, ebx
    cmp ebp, 16
    jbe .rs_small
    mov ebp, 16
.rs_small:
    mov word [DAP], 0x0010          ; packet size, zero
    mov word [DAP + 2], bp          ; block count
    mov edx, edi
    and edx, 0x0F
    mov word [DAP + 4], dx          ; offset
    mov edx, edi
    shr edx, 4
    mov word [DAP + 6], dx          ; segment
    mov dword [DAP + 8], eax        ; LBA low
    mov dword [DAP + 12], 0         ; LBA high
    mov ah, 0x42
    mov si, DAP
    mov dl, byte [boot_drive]
    int 0x13
    jc .rs_err
    shl ebp, 9
    add edi, ebp
    shr ebp, 9
    add eax, ebp
    sub ebx, ebp
    jmp .rs_loop
.rs_done:
    ret
.rs_err:
    mov si, msg_ioerr
    call say
    cli
.halt:
    hlt
    jmp .halt

align 8
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt                          ; physical address (org 0x7C00)
gdt:
    dq 0                            ; null
    dq 0x00CF9A000000FFFF           ; 0x08: flat 4 GiB code
    dq 0x00CF92000000FFFF           ; 0x10: flat 4 GiB data
gdt_end:

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7F000

    ; ---- move everything from the bounce area to the final
    ;      addresses (kernel segments to 1 MiB, modules to 2 MiB) ----
    mov ebx, COPY_TABLE
    mov ecx, dword [ebx]
    add ebx, 4
.copy_next:
    test ecx, ecx
    jz .copy_done
    mov esi, dword [ebx]            ; bounce source
    mov edi, dword [ebx + 4]        ; final destination
    mov ebp, dword [ebx + 8]        ; byte size
    add ebp, 3
    shr ebp, 2
    cld
    rep movsd
    add ebx, 12
    dec ecx
    jmp .copy_next
.copy_done:

    ; ---- zero the BSS tail (the Multiboot contract) ----
    mov edi, BSS_START
    mov ecx, BSS_END - BSS_START
    cmp ecx, 0
    jle .no_bss
    add ecx, 3
    shr ecx, 2
    xor eax, eax
    cld
    rep stosd
.no_bss:

    ; ---- copy the embedded strings (cmdline + module names) ----
    mov esi, strings_blob
    mov edi, STR_AREA
    mov ecx, strings_len
    rep movsb

    ; ---- mmap table from E820 (entry size forced to 20) ----
    mov ebx, E820_BUF
    mov edi, MMAP_TAB
    mov ecx, dword [e820_cnt]
    xor ebp, ebp                    ; mem_upper in KiB
.mmap_loop:
    test ecx, ecx
    jz .mmap_done
    push dword [ebx + 16]           ; type
    push dword [ebx + 12]           ; length high
    push dword [ebx + 8]            ; length low
    mov eax, dword [ebx]            ; base low
    mov edx, dword [ebx + 4]        ; base high
    mov dword [edi], 20             ; multiboot: size field
    mov dword [edi + 4], eax
    mov dword [edi + 8], edx
    pop eax                         ; length low
    mov dword [edi + 12], eax
    pop eax                         ; length high
    mov dword [edi + 16], eax
    pop eax                         ; type
    mov dword [edi + 20], eax
    ; track upper memory: type 1 regions that start above 1 MiB
    cmp dword [ebx + 16], 1
    jne .mmap_next
    cmp edx, 0
    jne .mmap_next                  ; region above 4 GiB - ignore
    cmp eax, 0x100000
    jb .mmap_next
    push ebx
    mov ebx, dword [ebx + 8]        ; length low
    add eax, ebx
    pop ebx
    sub eax, 0x100000
    shr eax, 10
    cmp eax, ebp
    jb .mmap_next
    mov ebp, eax
.mmap_next:
    add ebx, 24
    add edi, 24
    dec ecx
    jmp .mmap_loop
.mmap_done:
    mov dword [mmap_len], edi
    sub dword [mmap_len], MMAP_TAB
    test ebp, ebp
    jnz .have_upper
    mov ebp, 31744                  ; 32 MiB fallback
.have_upper:

    ; ---- modules table (destinations come from the read table) ----
    mov edi, MODS_TAB
    xor edx, edx
.mods_build:
    cmp edx, N_APPS
    jae .mods_built
    mov eax, dword [apps_table + edx * 8]      ; destination
    stosd
    add eax, dword [apps_table + edx * 8 + 4]  ; + size
    stosd
    mov eax, STR_AREA + CMDLINE_LEN
    add eax, dword [name_off + edx * 4]
    stosd
    xor eax, eax
    stosd
    inc edx
    jmp .mods_build
.mods_built:

    ; ---- multiboot_info: zero then fill ----
    mov edi, MBI
    xor eax, eax
    mov ecx, 32                     ; 128 bytes covers the struct
    rep stosd
    mov edi, MBI
    mov dword [edi + 0], 0x104D     ; mem|cmdline|mods|mmap|framebuffer
    mov dword [edi + 4], 640        ; mem_lower
    mov dword [edi + 8], ebp        ; mem_upper
    mov dword [edi + 16], STR_AREA  ; cmdline
    mov dword [edi + 20], N_APPS    ; mods_count
    mov dword [edi + 24], MODS_TAB  ; mods_addr
    mov eax, dword [mmap_len]
    mov dword [edi + 44], eax       ; mmap_length
    mov dword [edi + 48], MMAP_TAB  ; mmap_addr
    cmp word [vbe_mode_num], 0      ; VBE block
    je .no_vbe_info
    mov dword [edi + 72], VBINFO    ; vbe_control_info
    mov dword [edi + 76], MODEINFO  ; vbe_mode_info
    movzx eax, word [vbe_mode_num]
    or eax, 0x4000
    mov word [edi + 80], ax         ; vbe_mode
.no_vbe_info:
    cmp word [vbe_mode_num], 0      ; framebuffer fields
    je .fb_done
    mov eax, dword [MODEINFO + 40]  ; PhysBasePtr
    mov dword [edi + 88], eax       ; framebuffer_addr_low
    movzx eax, word [MODEINFO + 16] ; BytesPerScanLine
    mov dword [edi + 96], eax       ; pitch
    movzx eax, word [MODEINFO + 18]
    mov dword [edi + 100], eax      ; width
    movzx eax, word [MODEINFO + 20]
    mov dword [edi + 104], eax      ; height
    mov al, byte [MODEINFO + 25]
    mov byte [edi + 108], al        ; bpp
    mov byte [edi + 109], 1         ; type 1: direct RGB
    mov al, byte [MODEINFO + 31]    ; red mask size
    mov byte [edi + 110], al
    mov al, byte [MODEINFO + 32]    ; red field position
    mov byte [edi + 111], al
    mov al, byte [MODEINFO + 33]
    mov byte [edi + 112], al
    mov al, byte [MODEINFO + 34]
    mov byte [edi + 113], al
    mov al, byte [MODEINFO + 35]
    mov byte [edi + 114], al
    mov al, byte [MODEINFO + 36]
    mov byte [edi + 115], al
.fb_done:

    ; ---- one line of status on the text console ----
    mov esi, msg_go
    mov edi, 0xB8000
.prt:
    lodsb
    test al, al
    jz .prt_done
    cmp al, 13
    je .prt
    cmp al, 10
    je .prt
    mov byte [edi], al
    mov byte [edi + 1], 0x0F
    add edi, 2
    jmp .prt
.prt_done:

    ; ---- jump to the kernel: the Multiboot contract ----
    mov eax, MB_MAGIC
    mov ebx, MBI
    jmp ENTRY_POINT

msg_go db 'KvantOS starting..', 0

section .data
align 4
mmap_len dd 0
apps_table:
%include "direct-apps.inc"          ; dd <destination>, <size> pairs
name_off:
%include "direct-nameoffs.inc"      ; dd <offset within the names blob>
strings_blob:
    db CMDLINE, 0
%include "direct-names.inc"         ; module name strings
strings_end:
strings_len equ strings_end - strings_blob
