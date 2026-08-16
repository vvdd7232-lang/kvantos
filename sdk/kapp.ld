/* ============================================================
 *  KvantOS SDK - application memory layout
 *
 *  An application is built for the fixed address 0x00E00000
 *  (14 MiB) - exactly where the kernel unpacks the image. There
 *  are no relocation tables, so the address must match
 *  KAPP_LOAD_BASE from include/kvapp.h.
 *
 *  The entry point is kapp_main. The sections are glued into one
 *  contiguous chunk: the packer simply cuts it out of the ELF and
 *  prepends a 64-byte header.
 * ============================================================ */
ENTRY(kapp_main)

SECTIONS
{
    . = 0x00E00000;

    .text : {
        *(.text.start)      /* when the application defines a start section */
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(16) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(16) : {
        *(.data)
        *(.data.*)
    }

    /* Everything above goes into the file. What follows is
       uninitialised data: the kernel zeroes that part after loading. */
    __kapp_file_end = .;

    .bss ALIGN(16) : {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        __bss_end = .;
    }

    __kapp_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
