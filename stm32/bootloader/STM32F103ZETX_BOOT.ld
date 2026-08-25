/*
 * STM32F103ZETX Bootloader Linker Script (GCC)
 *
 * 用于 GCC/ARM Toolchain (非 Keil)
 * Bootloader: 0x08000000, 16KB
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 栈顶 = SRAM 末尾 */

SECTIONS
{
  /* 向量表 */
  .isr_vector :
  {
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  /* 代码 */
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

  /* 只读数据 */
  .rodata :
  {
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);

  /* 已初始化数据 */
  .data :
  {
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT> FLASH

  /* 未初始化数据 (BSS) */
  .bss :
  {
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* RAM 函数 (Flash 写入必须在 RAM 中运行) */
  .ramfunc :
  {
    *(.ramfunc)
    . = ALIGN(4);
  } > RAM AT> FLASH
}
