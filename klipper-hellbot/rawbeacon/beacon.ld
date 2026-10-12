ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08007000, LENGTH = 480K
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
  .text : { KEEP(*(.isr_vector)) *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
}
