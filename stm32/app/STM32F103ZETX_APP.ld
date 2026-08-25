/*
 * STM32F103ZET6 Application Linker Script (GCC)
 *
 * App A: ORIGIN = 0x08004000, LENGTH = 240K
 * 编译 App B 时修改 ORIGIN = 0x08040000
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08004000, LENGTH = 240K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
  } > FLASH

  .rodata :
  {
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  .bss :
  {
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  .ramfunc :
  {
    *(.ramfunc)
    . = ALIGN(4);
  } > RAM AT> FLASH
}
