/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(bt_glue_write)

btldd_ring_put = 0x503f0bfc;
btldd_ring_read = 0x503f0c38;
btldd_complete = 0x503f2ab8;

SECTIONS
{
    .hook 0x503f1cd0 : { KEEP(*(.hook)) }
    .objsize 0x503f28b0 : { KEEP(*(.objsize)) }
    /* the erased tail of the ROM image after nosdriver.ldd, no directory entry (BT0 section 7) */
    .text 0x511b4600 : {
        KEEP(*(.text.glue))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
}

ASSERT(SIZEOF(.hook) == 12, "hook is three words")
ASSERT(SIZEOF(.objsize) == 4, "size word")
ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x511c0000, "blob past the pad")
