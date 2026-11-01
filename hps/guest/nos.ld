/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(nos_ctor)

nos_complete = 0x50334fcc;
nos_delete = 0x503a2f64;

SECTIONS
{
    .exp4 0x511b4028 : { KEEP(*(.exp4)) }
    .exp6 0x511b4030 : { KEEP(*(.exp6)) }
    .objsize 0x511b413c : { KEEP(*(.objsize)) }
    /* erased ROM tail after the BT blob (0x511b4600..0x511b4f14) */
    .text 0x511b6000 : {
        *(.text .text.*)
        KEEP(*(.rodata.nosv))
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
}

ASSERT(SIZEOF(.exp4) == 4, "export 4 word")
ASSERT(SIZEOF(.exp6) == 4, "export 6 word")
ASSERT(SIZEOF(.objsize) == 4, "new size instruction")
ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x511c0000, "blob past the pad")
