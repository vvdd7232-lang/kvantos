/* Linker script for the KvantOS UEFI stub: a relocatable shared
   object that objcopy turns into a PE/COFF x86-64 EFI application. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS
{
    /* Leave the first page for PE headers; sections must not overlap them. */
    . = 4096;

    .text :
    {
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)
    }

    . = ALIGN(4096);

    .data :
    {
        *(.rodata*)
        *(.got.plt)
        *(.got)
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.gnu.linkonce.d.*)
        /* objcopy selects .data; include zero-initialized globals as well. */
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(4096);

    /* Fully RIP-relative code needs no fixups, but EFI loaders require a
       base relocation directory when loading away from ImageBase zero.
       IMAGE_REL_BASED_ABSOLUTE entries are deliberate no-ops. */
    .reloc : { LONG(0); LONG(12); SHORT(0); SHORT(0); }

    . = ALIGN(4096);

    .dynamic : { *(.dynamic) }

    . = ALIGN(4096);

    .rela :
    {
        *(.rela.data*)
        *(.rela.got*)
        *(.rela.stab*)
    }

    .rel :
    {
        *(.rel.data*)
        *(.rel.got*)
        *(.rel.stab*)
    }

    . = ALIGN(4096);

    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash   : { *(.hash) }
}
