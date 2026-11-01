// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_RUMBLE_H
#define NGH_RUMBLE_H

#include <stdint.h>
#include "../common/hyb_regs.h"
#include "../guest/nos_vib.h"

#define RUMBLE_DIV      16u
#define RUMBLE_EVERY_MS 500u
#define RUMBLE_VERSION  3u

_Static_assert(RUMBLE_EVERY_MS + RUMBLE_DIV < HYB_RUMBLE_HOLD_MS, "a rewrite lands inside the RTL hold");

struct rumble_io {
    void *ctx;
    // reads the magic and vib words at va (one 32-byte line); nonzero when va maps to no guest RAM
    int (*rd)(void *ctx, uint32_t va, uint32_t w[2]);
    void (*wr)(void *ctx, uint32_t v);
};

struct rumble {
    uint32_t on;
    uint32_t vib;
    uint32_t last;
    uint32_t max;
    uint32_t writes;
    uint32_t at;
};

uint32_t rumble_map(uint32_t vib);
void rumble_start(struct rumble *r, const struct rumble_io *io, uint32_t version);
int rumble_tick(struct rumble *r, const struct rumble_io *io, uint32_t ticks);
uint32_t rumble_row(const struct rumble *r);

#endif
