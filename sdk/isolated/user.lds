ENTRY(_start)
SECTIONS {
  . = 0x00e00000;
  __text_start = .;
  .text : { *(.text*) *(.rodata*) }
  __text_end = .;
  . = ALIGN(4096);
  __data_start = .;
  .data : { *(.data*) }
  __data_end = .;
  .bss (NOLOAD) : { *(.bss*) *(COMMON) }
  __memory_end = .;
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
