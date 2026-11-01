// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_PLATFORM_H
#define NGH_PLATFORM_H

#define PL_MON_BASE         0x38000000u
#define PL_MON_SIZE         0x00100000u
#define PL_MONDATA_BASE     0x38100000u
#define PL_MONDATA_SIZE     0x00100000u
#define PL_STATUS_BASE      0x38200000u
#define PL_STATUS_SIZE      0x00001000u
#define PL_PATCH_BASE       0x38201000u
#define PL_PATCH_SIZE       0x0000F000u
#define PL_EVRING_BASE      0x38210000u
#define PL_EVRING_SIZE      0x003F0000u
#define PL_SRAM_BASE        0x38600000u
#define PL_RAM2M_BASE       0x38800000u
#define PL_ROM_BASE         0x39000000u
#define PL_RAM16_BASE       0x3B000000u
#define PL_CDRV_BASE        0x38A00000u
#define PL_CDRV_SIZE        0x00400000u
#define PL_AUDIO_BASE       0x38E00000u
#define PL_AUDIO_SIZE       0x00040000u
#define PL_SCHK_BASE        0x38F00000u
#define PL_SCHK_SIZE        0x00080000u

#define PL_CORE_LO          0x30000000u
#define PL_CORE_HI          0x3FFFFFFFu

#define PL_SOCFWD_LO        0xC0000000u
#define PL_SOCFWD_HI        0xDFFFFFFFu
#define PL_HYBREG_LO        0xE0000000u
#define PL_HYBREG_HI        0xFBFFFFFFu
#define PL_HYBREG_MAGIC     0x4E474831u

// Placeholder: register index 6 of the register list; hps/common/hyb_regs.h owns the real offset.
#define PL_HYBREG_PAHI_OFF  0x18u

#define PL_HPS_LO           0xFFD00000u
#define PL_HPS_HI           0xFFFEFFFFu
#define PL_GICC_LO          0xFFFEC100u
#define PL_GICC_HI          0xFFFEC1FFu
#define PL_GICD_BASE        0xFFFED000u
#define PL_GICC_BASE        0xFFFEC100u
#define PL_L2_BASE          0xFFFEF000u
#define PL_L2_CACHE_SYNC    0x730u

#define PL_RSTMGR_MPUMODRST     0xFFD05010u
#define PL_MPUMODRST_CPU1       0x2u
#define PL_SYSMGR_CPU1STARTADDR 0xFFD080C4u
#define PL_RSTMGR_BRGMODRST     0xFFD0501Cu
#define PL_BRGMODRST_RESET      0x7u

// fpgamgr regs as read by Main fpga_io.cpp is_fpga_ready(0): GPIO_EXT_PORTA init_done (bit 2) and STAT mode == user (4)
#define PL_FPGAMGR_STAT         0xFF706000u
#define PL_FPGAMGR_GPIO_EXT     0xFF706850u
#define PL_FPGAMGR_MODE_MASK    0x7u
#define PL_FPGAMGR_MODE_USER    0x4u
#define PL_FPGAMGR_INITDONE     0x4u

#define PL_GIC_ID_HYB_IRQ   74u
#define PL_GIC_ID_HYB_FIQ   75u
#define PL_GIC_ID_PTIMER    29u

#define PL_CORENAME_HYB     "NGage"

// cpu1 GIC reset entry: no u suffix, start.S includes this file; flag and done sit past struct ngst
#define PL_C1RST_MARK_PA    0x38000004
#define PL_C1RST_MARK       0x30324252
#define PL_C1RST_FLAG       0x38200FF8
#define PL_C1RST_DONE       0x38200FFC
#define PL_C1RST_ARM        0x4D524131
#define PL_C1RST_DONE_V     0x454E4F44
#define PL_TT_MARK_PA       0x38000008
#define PL_TT_MARK          0x31305454

#ifndef __ASSEMBLER__
#include "hyb_regs.h"
_Static_assert(PL_CDRV_BASE >= PL_RAM2M_BASE + 0x00200000u, "C: shadow above RAM2M");
_Static_assert(PL_CDRV_BASE + PL_CDRV_SIZE <= PL_ROM_BASE, "C: shadow below the ROM slot");
_Static_assert(PL_AUDIO_BASE == HYB_AUD_BASE && PL_AUDIO_SIZE == HYB_AUD_SIZE, "audio ring is the FPGA reader's");
_Static_assert(PL_AUDIO_BASE >= PL_CDRV_BASE + PL_CDRV_SIZE && PL_AUDIO_BASE + PL_AUDIO_SIZE <= PL_ROM_BASE, "audio ring between C: and ROM");
_Static_assert(PL_SCHK_BASE >= PL_AUDIO_BASE + PL_AUDIO_SIZE && PL_SCHK_BASE + PL_SCHK_SIZE <= PL_ROM_BASE &&
               !(PL_SCHK_BASE & 0xFFFFFu), "checker log owns its MB between audio and ROM");
_Static_assert(PL_C1RST_MARK_PA == PL_MON_BASE + 4u, "marker is image word 1");
_Static_assert(PL_TT_MARK_PA == PL_MON_BASE + 8u, "time marker is image word 2");
_Static_assert(PL_C1RST_FLAG == PL_STATUS_BASE + 0xFF8u && PL_C1RST_DONE == PL_STATUS_BASE + 0xFFCu, "status tail");
#endif

#endif
