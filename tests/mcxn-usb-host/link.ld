/*
 * Minimal MCXN947 bring-up link script: vectors at flash 0x10000000, code after.
 *
 * Internal flash is NOT writable by CPU stores on this part.  The FMU
 * program/erase command sequence is the only way to change it (see
 * hw/misc/mcxn_fmu.c), so writable globals must be linked into SRAM, exactly as
 * they are on silicon.  Leaving them to land in flash "works" only against a
 * model that wrongly treats flash as RAM.
 */
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

    /* Writable globals, anchored high in SRAM so they clear both the scratch
     * buffers these tests hardcode (<= 0x2000_8300) and their stacks
     * (0x2001_0000, and 0x2002_0000 for the second core).  The address is on
     * .bss deliberately: ld ignores an explicit VMA on an empty .data, which
     * would drop .bss back to the start of SRAM and onto those buffers. */
    .bss 0x20030000 (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
    .data : { *(.data*) } > SRAM

    /DISCARD/ : { *(.ARM.*) *(.comment) }
}

/*
 * These smoke tests jump straight from the vector table to cpu0_main: there is
 * no startup code to copy initialised data from its flash load address into
 * SRAM.  A non-zero-initialised writable global would therefore silently read
 * as zero.  Fail the link rather than ship that trap — declare the global
 * zero-init (.bss) and assign it at run time instead.
 */
ASSERT(SIZEOF(.data) == 0,
       "no startup .data copy: use a zero-init global and assign it at run time")
