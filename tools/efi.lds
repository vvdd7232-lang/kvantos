/* Linker script for the KvantOS UEFI stub: a relocatable shared
   object that objcopy turns into a PE/COFF x86-64 EFI application. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = 0;

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
    }

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
