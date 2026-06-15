/* Minimal MCXN947 bring-up link script: vectors at flash 0x0, code after. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 2M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(cpu0_main)

SECTIONS
{
    .vectors 0x00000000 : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    /* This smoke test uses only MMIO + fixed SRAM addresses: no .data/.bss. */
    /DISCARD/ : { *(.ARM.*) *(.comment) }
}
