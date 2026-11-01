// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_XCAP_H
#define NGH_XCAP_H

#include <stdint.h>

#define XC_PK(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

#define XC_K_XTYP XC_PK('X', 'T', 'Y', 'P')
#define XC_K_XRSN XC_PK('X', 'R', 'S', 'N')
#define XC_K_XCT0 XC_PK('X', 'C', 'T', '0')
#define XC_K_XCT1 XC_PK('X', 'C', 'T', '1')
#define XC_K_XCT2 XC_PK('X', 'C', 'T', '2')
#define XC_K_XCT3 XC_PK('X', 'C', 'T', '3')
#define XC_K_XTHR XC_PK('X', 'T', 'H', 'R')
#define XC_K_XTIM XC_PK('X', 'T', 'I', 'M')
#define XC_K_XLVC XC_PK('X', 'L', 'V', 'C')
#define XC_K_XLVD XC_PK('X', 'L', 'V', 'D')
#define XC_K_XFRA XC_PK('X', 'F', 'R', 'A')
#define XC_K_XFRE XC_PK('X', 'F', 'R', 'E')
#define XC_K_XDLR XC_PK('X', 'D', 'L', 'R')
#define XC_K_XEPC XC_PK('X', 'E', 'P', 'C')
#define XC_K_XELR XC_PK('X', 'E', 'L', 'R')
#define XC_K_XEFA XC_PK('X', 'E', 'F', 'A')
#define XC_K_XEFS XC_PK('X', 'E', 'F', 'S')
#define XC_K_XLC0 XC_PK('X', 'L', 'C', '0')
#define XC_K_XLC1 XC_PK('X', 'L', 'C', '1')
#define XC_K_XLC2 XC_PK('X', 'L', 'C', '2')
#define XC_K_XLT0 XC_PK('X', 'L', 'T', '0')
#define XC_K_XLT1 XC_PK('X', 'L', 'T', '1')
#define XC_K_XLT2 XC_PK('X', 'L', 'T', '2')
#define XC_K_XKCN XC_PK('X', 'K', 'C', 'N')
#define XC_K_XKCT XC_PK('X', 'K', 'C', 'T')
#define XC_K_LARM XC_PK('L', 'A', 'R', 'M')

// row order inside the block; the first XC_NEXIT rows are rewritten at each exit capture
enum {
    XC_R_TYP, XC_R_RSN, XC_R_CT0, XC_R_CT1, XC_R_CT2, XC_R_CT3, XC_R_THR, XC_R_TIM, XC_R_LVC, XC_R_LVD, XC_R_FRA,
    XC_R_FRE, XC_NROW
};
#define XC_NEXIT 8u

// a second block the live fork does not read (harness only): lr at the Die, the newest guest forward
enum { XC_D_DLR, XC_D_EPC, XC_D_ELR, XC_D_EFA, XC_D_EFS, XC_NDROW };

// a third harness-only block: the caller (guest lr at the entry) and the thread of the newest three Leaves
enum { XC_L_C0, XC_L_C1, XC_L_C2, XC_L_T0, XC_L_T1, XC_L_T2, XC_NLROW };

// a fourth block the fork reads: card-identity reads (game process << 16 | all, each saturating at 0xFFFF) and
// the newest game-process read in XC_TIM_MS units, the XTIM clock
enum { XC_C_CN, XC_C_CT, XC_NCROW };

// XTYP: exit type (r1: 0 Kill, 1 Terminate, 2 Panic, 3 Pending) | flags | launcher hand-over count
#define XC_TYP_TYPE     0xFFu
#define XC_F_CUR        0x100u
#define XC_F_OWN        0x200u
#define XC_F_CAT        0x400u
#define XC_F_LONG       0x800u
#define XC_F_FOREIGN    0x1000u
#define XC_F_ALL        0x1F00u
// harness only, above the fork's XC_F_ALL mask: Die in a kernel-process thread, XTHR from the call-site register, held
#define XC_F_KDIE       0x2000u
#define XC_F_SREG       0x4000u
#define XC_F_HELD       0x8000u
#define XC_F_HX         0xE000u
#define XC_TYP_FOL_SHIFT 24
#define XC_TYP_FOL_MAX  15u

// XCT0..XCT3: up to XC_CAT_MAX category characters, the first in bits 31:24 of XCT0, '?' outside 0x20-0x7E
#define XC_CAT_MAX      16u

// XTIM: exit time | time from the newest captured Leave to the exit, both in XC_TIM_MS units since guest boot
#define XC_TIM_MS       100u
#define XC_TIM_EXIT     0xFFFFFu
#define XC_TIM_LV_SHIFT 20
#define XC_TIM_LV_MAX   4094u
#define XC_TIM_LV_NONE  4095u

// XLVC = code0 << 16 | code1, XLVD = code2 << 16 | count; codes newest first as saturated int16
#define XC_LV_NCODE     3u
#define XC_LV_CNT_MAX   0xFFFFu

// XFRA = free KB at learn << 16 | at the newest Leave, XFRE = at exit << 16 | minimum of the samples
#define XC_KB_NONE      0xFFFFu
#define XC_KB_MAX       0xFFFEu

// XEPC, XELR, XEFA: pc, the faulting mode's lr, far (pc for an undefined); XEFS = fsr & 0xFF | kind << 8 |
// mode << 10 | time in XC_TIM_MS units << 15, saturating
enum { XC_FWD_DABT = 1, XC_FWD_PABT = 2, XC_FWD_UND = 3 };
#define XC_EFS_KIND_SHIFT 8
#define XC_EFS_MODE_SHIFT 10
#define XC_EFS_TIM_SHIFT 15
#define XC_EFS_TIM_MAX  0x1FFFFu

// the User::Leave entry `mov r4, r0`: r0 is the leave code and lr still holds the caller (the `bl` at
// 0x503a6730 is the only other such word); the mark emulates the MOV, so the entry runs as on the ROM
#define XC_LEAVE_VA     0x503A672Cu
#define XC_LEAVE_ORIG   0xE1A04000u

// GD1DRV.ldd DoControl case 6 `ldr r3, [r4, #0x44]`, the ROM driver any game calls to read the card
// identity (MmcIf channel); the record it copies was cached at card init, so no MMC command shows the read
#define XC_CID_VA       0x503FFCC8u
#define XC_CID_ORIG     0xE5943044u

// kernel RAM page allocator: free pages = [[XC_FREE_CELL] + XC_FREE_AVAIL], 4 KB pages
#define XC_FREE_CELL    0x80006710u
#define XC_FREE_AVAIL   4u
#define XC_PAGE_KB      4u

// LARM as lim_rows (hps/mon/log.c) writes it: limiter arm cause << 28 | kick << 16 | arm time in 10 ms units
#define XC_LARM_CAUSE_SHIFT 28
#define XC_LARM_CAUSE_MASK 0xFu
#define XC_LARM_KICK_SHIFT 16
#define XC_LARM_KICK_MASK 0xFFFu
#define XC_LARM_T_MASK  0xFFFFu
#define XC_LARM_FRAME   1u
#define XC_LARM_TIMER   2u

#endif
