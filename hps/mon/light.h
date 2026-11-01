// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_LIGHT_H
#define NGH_LIGHT_H

#include <stdint.h>
#include "../common/hyb_regs.h"

// policy (changes only these two)
#define LIGHT_IDLE_MS   15000u
#define LIGHT_K_OFF     154u

#define LIGHT_DIV       16u
#define LIGHT_EVERY_MS  1000u
#define LIGHT_P_VA      0x80006C1Cu
#define LIGHT_W_OFF     24u
#define LIGHT_ROM_VA    0x50000000u
#define LIGHT_IDLE_OFF  0xFFFFu

_Static_assert(LIGHT_K_OFF < HYB_LIGHT_FULL, "the off level is below full");

struct light_io {
    void *ctx;
    // one guest word at va; nonzero when va maps to no guest RAM
    int (*rd)(void *ctx, uint32_t va, uint32_t *w);
    void (*wr)(void *ctx, uint32_t v);
};

struct light {
    uint32_t on;
    uint32_t ok;
    uint32_t armed;
    uint32_t seen;
    uint32_t w;
    uint32_t act_at;
    uint32_t idle;
    uint32_t k;
    uint32_t at;
    uint32_t writes;
    uint32_t acts;
    uint32_t why;
};

int light_verify(const uint8_t *rom, uint32_t size);
void light_start(struct light *l, const struct light_io *io, uint32_t ver, int ok);
int light_tick(struct light *l, const struct light_io *io, uint32_t ticks, uint32_t armed);
uint32_t light_lght(const struct light *l);
uint32_t light_lidl(const struct light *l);

#endif
