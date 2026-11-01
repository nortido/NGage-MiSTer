// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_LCD_H
#define NGL_LCD_H

#include "ngl_types.h"

// ngage_lcd.sv FB_WORDS: 176x208 pixels, two per word
#define NGL_LCD_FB_WORDS 18304u

int ngl_lcd_black(const struct ngl_io *io);

#endif
