; Shared with the instruction-level regression test. Protected mode, flat DS/ES.
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
    mov eax, dword [ebx]            ; reload BASE, eax currently holds TYPE
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

