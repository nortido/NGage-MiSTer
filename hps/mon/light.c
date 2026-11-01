// SPDX-License-Identifier: GPL-3.0-or-later
#include "light.h"

// exec 0xc9's table slot, its handler loading P, and the store of the tick into [P+24]
static const uint32_t light_vw[][2] = {
    { 0x5032730Cu, 0x50336860u }, { 0x50336860u, 0xE59F3004u }, { 0x50336864u, 0xE5930000u },
    { 0x5033686Cu, 0x80006C1Cu }, { 0x50343884u, 0xE5803018u }, { 0x503438A0u, 0x80000488u },
};

int light_verify(const uint8_t *rom, uint32_t size)
{
    uint32_t i, o;

    for (i = 0; i < sizeof light_vw / sizeof light_vw[0]; i++) {
        o = light_vw[i][0] - LIGHT_ROM_VA;
        if (o > size || size - o < 4u)
            return 0;
        if ((uint32_t)(rom[o] | rom[o + 1u] << 8 | rom[o + 2u] << 16 | (uint32_t)rom[o + 3u] << 24) != light_vw[i][1])
            return 0;
    }
    return 1;
}

void light_start(struct light *l, const struct light_io *io, uint32_t ver, int ok)
{
    struct light z = { 0 };

    *l = z;
    l->on = ver >= HYB_VERSION_LIGHT;
    l->ok = ok != 0;
    l->k = HYB_LIGHT_FULL;
    if (l->on)
        io->wr(io->ctx, HYB_LIGHT_FULL);
}

static void light_put(struct light *l, const struct light_io *io, uint32_t k, uint32_t ticks)
{
    l->k = k;
    l->at = ticks;
    if (l->on) {
        io->wr(io->ctx, k);
        l->writes++;
    }
}

static uint32_t light_read(const struct light_io *io, uint32_t *w)
{
    uint32_t p;

    if (io->rd(io->ctx, LIGHT_P_VA, &p))
        return 1;
    if (!p || (p & 3u))
        return 2;
    return io->rd(io->ctx, p + LIGHT_W_OFF, w) ? 3u : 0u;
}

int light_tick(struct light *l, const struct light_io *io, uint32_t ticks, uint32_t armed)
{
    uint32_t w = 0, k;

    if (!l->ok || ticks % LIGHT_DIV)
        return 0;
    if (armed && !l->armed)
        l->act_at = ticks;
    l->armed = armed != 0;
    l->why = light_read(io, &w);
    // bad data never darkens: a failed read is activity
    if (l->why) {
        l->act_at = ticks;
    } else if (!l->seen) {
        l->seen = 1;
        l->w = w;
    } else if (w != l->w) {
        l->w = w;
        l->acts++;
        l->act_at = ticks;
    }
    l->idle = l->armed ? ticks - l->act_at : 0;
    k = l->idle >= LIGHT_IDLE_MS ? LIGHT_K_OFF : HYB_LIGHT_FULL;
    // while dim the value is written again, so a soc_held reset to full heals
    if (k != l->k || (k != HYB_LIGHT_FULL && ticks - l->at >= LIGHT_EVERY_MS))
        light_put(l, io, k, ticks);
    return 1;
}

uint32_t light_lght(const struct light *l)
{
    return l->writes << 16 | l->k;
}

uint32_t light_lidl(const struct light *l)
{
    uint32_t s = l->idle / 1000u;

    if (!l->ok)
        s = LIGHT_IDLE_OFF;
    else if (s > LIGHT_IDLE_OFF - 1u)
        s = LIGHT_IDLE_OFF - 1u;
    return l->acts << 16 | s;
}
