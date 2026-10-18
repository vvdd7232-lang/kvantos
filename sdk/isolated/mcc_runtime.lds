ENTRY(_start)
SECTIONS {
  . = 0x00e06000;
  __text_start = .;
  .text : { *(.text*) *(.rodata*) }
  __text_end = .;
  ASSERT(__text_end <= 0x00e08000, "MCC user runtime text exceeds 8KiB")
  . = 0x00e0b000;
  __data_start = .;
  .data : { *(.data*) }
  ASSERT(SIZEOF(.data) == 0, "MCC runtime mutable data must be zero initialized")
  .bss (NOLOAD) : { *(.bss*) *(COMMON) }
  __memory_end = .;
  ASSERT(__memory_end < 0x00fe0000, "MCC runtime overlaps bootstrap")
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
