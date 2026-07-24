/*
 * MCXN947 QSPI execute-in-place BOOT image.
 *
 * A production QSPI-boot image lives entirely in the external FlexSPI NOR (AHB-mapped at
 * 0x8000_0000 non-secure / 0x9000_0000 secure): the vector table at the base, the FlexSPI
 * Config Block (FCB) at offset 0x400, and the code after it.  The boot ROM validates the FCB,
 * configures FlexSPI, and boots from the vector table -- no internal-flash image involved.
 * This image links at the secure XIP alias 0x9000_0000.  Writable
 * globals live in SRAM (the NOR is read-only to CPU stores), exactly as on silicon.
 */
MEMORY
{
    QSPI (rx)  : ORIGIN = 0x90000000, LENGTH = 8M     /* secure FlexSPI XIP NOR alias */
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY(cpu0_main)

SECTIONS
{
    .vectors 0x90000000 : { KEEP(*(.vectors)) } > QSPI   /* SP + reset PC, fetched at reset */
    .fcb     0x90000400 : { KEEP(*(.fcb)) }     > QSPI   /* FlexSPI Config Block, tag "FCFB" */
    .text    0x90000600 : { *(.text*) *(.rodata*) } > QSPI

    .bss 0x20030000 (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
    .data : { *(.data*) } > SRAM

    /DISCARD/ : { *(.ARM.*) *(.comment) }
}

/* No startup copies .data from QSPI to SRAM, so a non-zero writable global would read as
 * zero -- fail the link instead of shipping that trap. */
ASSERT(SIZEOF(.data) == 0,
       "no startup .data copy: use a zero-init global and assign it at run time")
