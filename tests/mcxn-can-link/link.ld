/* MCXN947 USB enum test: vectors+code in flash, .bss buffers in SRAM. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(cpu0_main)

SECTIONS
{
    .vectors 0x10000000 : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    /* Zero-init buffers (setup/EP0/flags) live low in SRAM, clear of the
     * BDT @ 0x2000_2000 and the stack @ 0x2001_0000. */
    .bss 0x20000000 (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
    /DISCARD/ : { *(.ARM.*) *(.comment) }
}
