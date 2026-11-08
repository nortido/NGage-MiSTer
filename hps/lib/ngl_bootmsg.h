// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_BOOTMSG_H
#define NGL_BOOTMSG_H

#include <stdint.h>

#include "../common/status.h"

#define NGL_BM_START_TEXT      "N-Gage: starting OS..."
#define NGL_BM_STALL_TEXT      "N-Gage: OS didn't start\nsee /tmp/ngl/fork.log"
#define NGL_BM_UP_TEXT         "N-Gage: OS started"
#define NGL_BM_START_SHOW_MS   4000u
#define NGL_BM_START_AGAIN_MS  3000u
#define NGL_BM_START_END_MS    12000u
#define NGL_BM_STALL_MS        30000u
#define NGL_BM_STALL_SHOW_MS   12000u
#define NGL_BM_STALL_AGAIN_MS  10000u
#define NGL_BM_UP_SHOW_MS      2000u
#define NGL_BM_VIS_FPS         1u

enum { NGL_BM_NONE = 0, NGL_BM_START = 1, NGL_BM_STALL = 2, NGL_BM_UP = 3, NGL_BM_HIDE = 4 };

struct ngl_bootmsg {
    uint32_t active;
    uint32_t t0;
    uint32_t up;
    uint32_t n;
    uint32_t ms;
    uint32_t start_n;
    uint32_t start_at;
    uint32_t stall_n;
    uint32_t stall_at;
    uint32_t held;
    uint32_t held_end;
    uint32_t held_ms;
    uint32_t info_at;
    uint32_t last_idx;
    uint32_t good;
    uint32_t vis;
};

void ngl_bootmsg_begin(struct ngl_bootmsg *b, uint32_t now, uint32_t fps_idx);
void ngl_bootmsg_stop(struct ngl_bootmsg *b);
void ngl_bootmsg_defer(struct ngl_bootmsg *b, uint32_t now, uint32_t ms);
int ngl_bootmsg_step(struct ngl_bootmsg *b, uint32_t now, uint32_t state, uint32_t gproc, uint32_t fps_idx,
                     uint32_t fps);

#endif
