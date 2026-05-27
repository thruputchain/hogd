ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0, LENGTH = 64K
}

SECTIONS
{
  .text 0 : ALIGN(4)
  {
    KEEP(*(.text._start))
    *(.text .text.*)
  } > RAM

  . = ALIGN(8);
  .rodata : ALIGN(8)
  {
    *(.srodata .srodata.*)
    *(.rodata .rodata.*)
  } > RAM

  . = ALIGN(8);
  .data : ALIGN(8)
  {
    *(.sdata .sdata.*)
    *(.data .data.*)
  } > RAM

  . = ALIGN(8);
  .bss (NOLOAD) : ALIGN(8)
  {
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
  } > RAM

  /DISCARD/ :
  {
    *(.got .got.*)
    *(.got.plt .got.plt.*)
    *(.plt .plt.*)
    *(.data.rel.ro .data.rel.ro.*)
    *(.tdata .tdata.*)
    *(.tbss .tbss.*)
    *(.init_array .init_array.*)
    *(.eh_frame .eh_frame.*)
    *(.debug_* .debug*)
    *(.comment)
  }
}
