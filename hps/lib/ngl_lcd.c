// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/hyb_regs.h"
#include "ngl_lcd.h"
#include "ngl_mon.h"

#define NGL_LCD_PA      0x0C121200u
#define NGL_LCD_PORT(o) (HYB_WIN_PHYS + HYB_FWD_OFF(NGL_LCD_PA + (o), HYB_ACC_WORD))

// the guest's own words: rows 0..207, columns 2..177 (frame column 0), wptr 0, a 12 bpp burst
static const uint32_t lcd_cmd[11][2] = {
    { 0, 0x09200000u }, { 8, 0x75u }, { 0, 0x09240003u }, { 8, 0x0CF00000u },
    { 0, 0x09200000u }, { 8, 0x15u }, { 0, 0x09240003u }, { 8, 0x0B100002u },
    { 0, 0x09200000u }, { 8, 0x5Cu }, { 0, 0x8924D67Fu }
};

int ngl_lcd_black(const struct ngl_io *io)
{
    uint32_t i;
    int rc = ngl_window_ok(io);

    if (rc)
        return rc;
    if (io->rd32(io->ctx, HYB_WIN_PHYS + HYB_REG_OFF(HYB_R_MAGIC)) != HYB_MAGIC_VALUE)
        return NGL_E_MAGIC;
    if (io->wr32(io->ctx, HYB_WIN_PHYS + HYB_REG_OFF(HYB_R_PAHI), HYB_PAHI_OF(NGL_LCD_PA)))
        return NGL_E_MEM;
    for (i = 0; i < 11; i++)
        if (io->wr32(io->ctx, NGL_LCD_PORT(lcd_cmd[i][0]), lcd_cmd[i][1]))
            return NGL_E_MEM;
    for (i = 0; i < NGL_LCD_FB_WORDS; i++)
        if (io->wr32(io->ctx, NGL_LCD_PORT(8u), 0))
            return NGL_E_MEM;
    return (int)NGL_LCD_FB_WORDS;
}
