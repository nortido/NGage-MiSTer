// SPDX-License-Identifier: GPL-3.0-or-later
#include "rumble.h"

_Static_assert(NOS_VIB_OFF_MAGIC == 0u && NOS_VIB_OFF_VIB == 4u, "rd gets magic, then vib");

// 0..100 is the phone's own intensity scale, one phone motor drives both pad motors
uint32_t rumble_map(uint32_t vib)
{
    uint32_t l = NOS_VIB_LEVEL(vib);

    if (!NOS_VIB_IS_ON(vib))
        return 0;
    if (l > 100u)
        l = 100u;
    return (l * 255u + 50u) / 100u;
}

void rumble_start(struct rumble *r, const struct rumble_io *io, uint32_t version)
{
    struct rumble z = { 0 };

    *r = z;
    r->on = version >= RUMBLE_VERSION;
    if (r->on)
        io->wr(io->ctx, 0);
}

static void rumble_put(struct rumble *r, const struct rumble_io *io, uint32_t b, uint32_t ticks)
{
    io->wr(io->ctx, b * 0x0101u);
    r->last = b;
    r->at = ticks;
    r->writes++;
    if (b > r->max)
        r->max = b;
}

int rumble_tick(struct rumble *r, const struct rumble_io *io, uint32_t ticks)
{
    uint32_t w[2], b;

    if (!r->on || ticks % RUMBLE_DIV)
        return 0;
    if (io->rd(io->ctx, NOS_VIB_VA, w) || w[0] != NOS_VIB_MAGIC)
        w[1] = 0;
    r->vib = w[1];
    b = rumble_map(w[1]);
    // the RTL drops a value after HYB_RUMBLE_HOLD_MS, so a nonzero one is written again before that
    if (b != r->last || (b && ticks - r->at >= RUMBLE_EVERY_MS))
        rumble_put(r, io, b, ticks);
    return 1;
}

uint32_t rumble_row(const struct rumble *r)
{
    return r->writes << 16 | r->max << 8 | r->last;
}
