ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
  DTCM (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
  AXI (rw)   : ORIGIN = 0x24000000, LENGTH = 512K
}

#if H743_TEST_DATA_DTCM
REGION_ALIAS("RAM_DATA", DTCM);
#else
REGION_ALIAS("RAM_DATA", AXI);
#endif

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) KEEP(*(.init)) KEEP(*(.fini)) } > FLASH
  .rodata : { *(.rodata*) } > FLASH
  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH
  .preinit_array : {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH
  .init_array : {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > FLASH
  .fini_array : {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH
  .data : ALIGN(8) {
    _sdata = .;
    *(.data*)
    . = ALIGN(8);
    _edata = .;
  } > RAM_DATA AT> FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : ALIGN(32) {
    _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(32);
    _ebss = .;
  } > RAM_DATA
  .h743_dtcm_bench (NOLOAD) : ALIGN(32) {
    *(.h743_dtcm_bench .h743_dtcm_bench.*)
    . = ALIGN(32);
    _dtcm_used = .;
  } > DTCM
  .h743_axi_bench (NOLOAD) : ALIGN(32) {
    *(.h743_axi_bench .h743_axi_bench.*)
    . = ALIGN(32);
  } > AXI
  ASSERT(_dtcm_used <= _estack - 0x4000, "DTCM must leave 16 KiB for MSP")
  /DISCARD/ : { *(.note.GNU-stack) }
}
