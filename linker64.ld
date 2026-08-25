/* KvantOS 3.0: 64-bit kernel, linked at physical address 1 MiB.
   Loaded by the UEFI stub (boot/kvantefi.c), which parses the ELF
   program headers - no Multiboot header needed on this path. */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    kernel_start = .;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    kernel_end = ALIGN(4K);
}
