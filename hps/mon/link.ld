/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start)
SECTIONS
{
    . = MON_VA;
    .text : { *(.text.start) *(.text.vectors) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
    ASSERT(__bss_end <= MON_VA + 0x100000, "monitor image exceeds 1 MB")
    __fiq_top = MON_VA + 0x1E0000;
    __svc_top = MON_VA + 0x1E8000;
    __irq_top = MON_VA + 0x1F0000;
    __und_top = MON_VA + 0x1F8000;
    __abt_top = MON_VA + 0x200000;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
