// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HYB_REGS_H
#define HYB_REGS_H

#define HYB_WIN_PHYS              0xC0000000u
#define HYB_WIN_SIZE              0x40000000u
#define HYB_WIN_REGS              0x20000000u

#define HYB_FWD_ACC_SHIFT         27
#define HYB_FWD_LANE_SHIFT        25
#define HYB_FWD_PA_MASK           0x01FFFFFCu
#define HYB_PAHI_SHIFT            25
#define HYB_ACC_BYTE              0u
#define HYB_ACC_HALF              1u
#define HYB_ACC_WORD              2u

#define HYB_FWD_OFF(pa, acc)      ((((acc) & 3u) << HYB_FWD_ACC_SHIFT) | (((pa) & 3u) << HYB_FWD_LANE_SHIFT) | ((pa) & HYB_FWD_PA_MASK))
#define HYB_PAHI_OF(pa)           ((pa) >> HYB_PAHI_SHIFT)
#define HYB_REG_OFF(r)            (HYB_WIN_REGS | (r))

#define HYB_R_MAGIC               0x000u
#define HYB_R_VERSION             0x004u
#define HYB_R_SCRATCH             0x008u
#define HYB_R_CTRL                0x00Cu
#define HYB_R_STATUS              0x010u
#define HYB_R_ROM_SIZE            0x014u
#define HYB_R_PAHI                0x018u
#define HYB_R_TIMEOUTS            0x01Cu
#define HYB_R_HELD                0x020u
#define HYB_R_DBG0                0x024u
#define HYB_R_DBG1                0x028u
#define HYB_R_DBG2                0x02Cu
#define HYB_R_DBG3                0x030u
#define HYB_R_LCD_FPS             0x034u
#define HYB_R_IRQ_LEVEL           0x038u
#define HYB_R_DMA_DROPS           0x03Cu
#define HYB_R_DDR_ADDR            0x040u
#define HYB_R_DDR_WLO             0x044u
#define HYB_R_DDR_WHI             0x048u
#define HYB_R_DDR_RLO             0x04Cu
#define HYB_R_DDR_RHI             0x050u
#define HYB_R_DDR_CMD             0x054u
#define HYB_R_KEYS_OR             0x058u
#define HYB_R_AUD_CTL             0x05Cu
#define HYB_R_AUD_STEP            0x060u
#define HYB_R_AUD_WR              0x064u
#define HYB_R_AUD_RD              0x068u
#define HYB_R_AUD_UNDER           0x06Cu
#define HYB_R_RUMBLE              0x070u
#define HYB_R_LIGHT               0x074u

#define HYB_MAGIC_VALUE           0x4E474831u
#define HYB_VERSION_VALUE         0x00000004u
#define HYB_VERSION_FIQ           2u
#define HYB_VERSION_AUD           3u
#define HYB_VERSION_LIGHT         4u

#define HYB_CTRL_SOC_RUN          0x00000001u

#define HYB_ST_DL_ACTIVE          0x00000001u
#define HYB_ST_ROM_LOADED         0x00000002u
#define HYB_ST_OSD_RESET          0x00000004u
#define HYB_ST_CARD               0x00000008u
#define HYB_ST_FLASH_ARRAY_SHIFT  4
#define HYB_ST_FLASH_ARRAY_MASK   0x00000070u
#define HYB_ST_DMA_PEND_SHIFT     8
#define HYB_ST_DMA_PEND_MASK      0x00007F00u
#define HYB_ST_TURBO              0x00010000u
#define HYB_ST_WX                 0x00020000u

#define HYB_FWD_TIMEOUT_CLK       4096u
#define HYB_FWD_TIMEOUT_DATA      0xDEADBEEFu

#define HYB_DDR_CMD_PEEK          0x00000001u
#define HYB_DDR_CMD_POKE          0x00000002u
#define HYB_DDR_CMD_BUSY          0x00000001u

#define HYB_AUD_CTL_EN            0x00000001u
#define HYB_AUD_CTL_STEREO        0x00000002u
#define HYB_AUD_CTL_PRIME_SHIFT   16
#define HYB_AUD_CTL_PRIME_MASK    0xFFFF0000u
#define HYB_AUD_BASE              0x38E00000u
#define HYB_AUD_SIZE              0x00040000u

#define HYB_RUMBLE_HOLD_MS        2000u
#define HYB_LIGHT_FULL            0x100u

#endif
