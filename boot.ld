ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
  RAM (rwx)  : ORIGIN = 0x24000000, LENGTH = 512K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_app_start = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } >FLASH
  .text : { *(.text .text.*) *(.rodata .rodata.*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); } >FLASH
  .ARM.extab : { *(.ARM.extab*) } >FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
  .preinit_array : {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH
  .init_array : {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH
  .fini_array : {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH
  .data : {
    . = ALIGN(4); _sdata = .;
    *(.data .data.* .RamFunc .RamFunc.*)
    . = ALIGN(4); _edata = .;
  } >RAM AT>FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : {
    . = ALIGN(4); _sbss = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(8); _ebss = .;
  } >RAM
  PROVIDE(end = _ebss);
  PROVIDE(_end = _ebss);
  ASSERT(g_pfnVectors == ORIGIN(FLASH), "Boot vectors must start at the flash origin")
  ASSERT(_ebss + 8K <= _estack, "Bootloader stack overlaps its data")
}
