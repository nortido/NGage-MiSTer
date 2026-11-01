// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_MON_H
#define NGL_MON_H

#include <stdint.h>

#include "ngl_cpu1.h"

#define NGL_FW_CARD 0x1u

enum { NGL_W_IDLE = 0, NGL_W_RUN = 1 };

struct ngl_watch {
    int phase;
    uint32_t reason;
    uint32_t fatal_kind;
    int rc;
};

struct ngl_start {
    const void *mon;
    uint32_t mon_len;
    const uint32_t *patch;
    uint32_t patch_nw;
    uint32_t memcfg;
    uint32_t mile;
    uint32_t log_mode;
    uint32_t cdrv;
};

int ngl_mon_status_init(const struct ngl_io *io, uint32_t memcfg, uint32_t mile, uint32_t log_mode);
int ngl_mon_load(const struct ngl_io *io, const void *mon, uint32_t len);
int ngl_patch_write(const struct ngl_io *io, const uint32_t *words, uint32_t nw);
int ngl_mon_start(const struct ngl_io *io, const struct ngl_start *s, int force);
int ngl_window_ok(const struct ngl_io *io);
int ngl_fpga_wait(const struct ngl_io *io, uint32_t flags, uint32_t timeout_ms);
int ngl_watch_step(const struct ngl_io *io, struct ngl_watch *st);

#endif
