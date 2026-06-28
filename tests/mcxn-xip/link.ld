/*
 * MCXN947 XIP link script: vectors + startup in internal flash (0x1000_0000),
 * the .xip application section in the FlexSPI0 AHB NOR window (0x9000_0000).
 * The reset handler runs from flash and calls into QSPI, executing in place.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2M
    QSPI  (rx)  : ORIGIN = 0x90000000, LENGTH = 8M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(cpu0_main)

SECTIONS
{
    .vectors 0x10000000 : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    /* Application code executed in place from the FlexSPI AHB window. */
    .xip 0x90000000 : { KEEP(*(.xip)) } > QSPI
    /* This test uses only MMIO + fixed SRAM addresses: no .data/.bss. */
    /DISCARD/ : { *(.ARM.*) *(.comment) }
}
