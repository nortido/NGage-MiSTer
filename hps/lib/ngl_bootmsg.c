// SPDX-License-Identifier: GPL-3.0-or-later
#include "ngl_bootmsg.h"

static int bm_before(uint32_t now, uint32_t end)
{
    return (int32_t)(now - end) < 0;
}

void ngl_bootmsg_begin(struct ngl_bootmsg *b, uint32_t now, uint32_t fps_idx)
{
    // a live hold ends at most held_ms ahead; anything else is a past one the 32-bit clock wrapped around
    if (b->held && b->held_end - now > b->held_ms)
        b->held = 0;
    if (!b->held)
        b->info_at = now;
    b->active = 1;
    b->t0 = now;
    b->up = 0;
    b->n = 0;
    b->ms = 0;
    b->start_n = 0;
    b->start_at = 0;
    b->stall_n = 0;
    b->stall_at = 0;
    b->last_idx = fps_idx;
    b->good = 0;
    b->vis = 0;
}

void ngl_bootmsg_stop(struct ngl_bootmsg *b)
{
    b->active = 0;
}

void ngl_bootmsg_defer(struct ngl_bootmsg *b, uint32_t now, uint32_t ms)
{
    uint32_t end = now + ms;

    if (!b->held)
        b->held_ms = 0;
    if (!b->held || bm_before(b->held_end, end))
        b->held_end = end;
    if (ms > b->held_ms)
        b->held_ms = ms;
    b->held = 1;
    b->info_at = now;
}

int ngl_bootmsg_step(struct ngl_bootmsg *b, uint32_t now, uint32_t state, uint32_t gproc, uint32_t fps_idx,
                     uint32_t fps)
{
    uint32_t rel;

    if (!b->active)
        return NGL_BM_NONE;
    rel = now - b->t0;
    b->ms = rel;
    if (gproc != 0 && !b->up) {
        b->up = 1;
        b->n = 1;
        b->last_idx = fps_idx;
        b->good = 0;
        return NGL_BM_UP;
    }
    // the LCD counter also runs on the black frames of the OS init; the first frame after the game process is the
    // phone screen or the splash, which already tells the user the start went on
    if (fps_idx != b->last_idx) {
        b->last_idx = fps_idx;
        b->good = fps >= NGL_BM_VIS_FPS ? b->good + 1 : 0;
        if (!b->vis && b->up && b->good >= 1) {
            b->vis = 1;
            if (b->start_n && now - b->start_at < NGL_BM_START_SHOW_MS &&
                !bm_before(b->start_at, b->info_at))
                return NGL_BM_HIDE;
        }
    }
    if (b->held) {
        if (bm_before(now, b->held_end))
            return NGL_BM_NONE;
        b->held = 0;
    }
    if (!b->vis && rel < NGL_BM_START_END_MS) {
        if (b->start_n && now - b->start_at < NGL_BM_START_AGAIN_MS)
            return NGL_BM_NONE;
        b->start_n++;
        b->start_at = now;
        b->n = b->start_n;
        return NGL_BM_START;
    }
    if (!b->up && rel >= NGL_BM_STALL_MS && state == NGST_STATE_RUN) {
        if (b->stall_n && now - b->stall_at < NGL_BM_STALL_AGAIN_MS)
            return NGL_BM_NONE;
        b->stall_n++;
        b->stall_at = now;
        b->n = b->stall_n;
        return NGL_BM_STALL;
    }
    return NGL_BM_NONE;
}
