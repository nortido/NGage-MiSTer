// SPDX-License-Identifier: GPL-3.0-or-later
#include <string.h>

#include "../common/hyb_regs.h"
#include "ngl_mon.h"

enum { NGL_FPGA_POLL_MS = 50 };

#define NGL_HYB_REG(r) (HYB_WIN_PHYS + HYB_REG_OFF(r))

static uint32_t ngl_le32(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0, i;
    for (i = 0; i < n && i < 4; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int status_args_ok(uint32_t memcfg, uint32_t mile, uint32_t log_mode)
{
    return memcfg <= 63u && !(mile & ~(NGST_MILE_MMU | NGST_MILE_IRQ)) && log_mode <= NGST_LOG_RING;
}

static int mon_args_ok(const void *mon, uint32_t len)
{
    return mon && len && len <= PL_MON_SIZE;
}

static int patch_args_ok(const uint32_t *words, uint32_t nw)
{
    return words && nw && nw <= PL_PATCH_SIZE / 4u && words[0] <= (nw - 1u) / 2u;
}

static int status_write(const struct ngl_io *io, uint32_t memcfg, uint32_t mile, uint32_t log_mode, uint32_t cdrv)
{
    // the board scripts set the log mode first, then the rest, in this order
    const uint32_t pa[14] = {
        NGL_ST(log_mode), NGL_ST(state), NGL_ST(park_req), NGL_ST(flush_req), NGL_ST(flush_ack),
        NGL_ST(profile), NGL_ST(prog_size), NGL_ST(mile_mask), PL_EVRING_BASE, NGL_ST(memcfg),
        NGL_ST(cdrv), NGL_ST(cdrv_gen), NGL_ST(exit_seen), NGL_ST(gproc)
    };
    const uint32_t v[14] = {
        log_mode, NGST_STATE_BOOT, 0, 0, 0, NGST_PROFILE_NGAGE, 0, mile, 0, memcfg, cdrv, 0, 0, 0
    };
    int i;

    for (i = 0; i < 14; i++)
        if (io->wr32(io->ctx, pa[i], v[i]))
            return NGL_E_MEM;
    return NGL_OK;
}

static int mon_write(const struct ngl_io *io, const void *mon, uint32_t len)
{
    const uint8_t *b = mon;
    uint32_t body = len & ~3u, last = ((len + 3u) & ~3u) - 4u;

    if (body && io->mem_write(io->ctx, PL_MON_BASE, b, body))
        return NGL_E_MEM;
    if (len & 3u && io->wr32(io->ctx, PL_MON_BASE + body, ngl_le32(b + body, len & 3u)))
        return NGL_E_MEM;
    if (io->rd32(io->ctx, PL_MON_BASE) != ngl_le32(b, len) ||
        io->rd32(io->ctx, PL_MON_BASE + last) != ngl_le32(b + last, len - last))
        return NGL_E_MON_READBACK;
    return NGL_OK;
}

static int patch_put(const struct ngl_io *io, const uint32_t *words, uint32_t nw)
{
    if (io->mem_write(io->ctx, PL_PATCH_BASE, words, nw * 4u))
        return NGL_E_MEM;
    // L|watch:hyb-patchlist-load: one run saw PCNT 0
    if (io->rd32(io->ctx, PL_PATCH_BASE) != words[0])
        return NGL_E_PATCH_READBACK;
    return NGL_OK;
}

int ngl_mon_status_init(const struct ngl_io *io, uint32_t memcfg, uint32_t mile, uint32_t log_mode)
{
    if (!status_args_ok(memcfg, mile, log_mode))
        return NGL_E_ARG;
    if (ngl_mon_live(io))
        return NGL_E_LIVE;
    return status_write(io, memcfg, mile, log_mode, 0);
}

int ngl_mon_load(const struct ngl_io *io, const void *mon, uint32_t len)
{
    if (!mon_args_ok(mon, len))
        return NGL_E_ARG;
    if (ngl_mon_live(io))
        return NGL_E_LIVE;
    return mon_write(io, mon, len);
}

int ngl_patch_write(const struct ngl_io *io, const uint32_t *words, uint32_t nw)
{
    if (!patch_args_ok(words, nw))
        return NGL_E_ARG;
    if (ngl_mon_live(io))
        return NGL_E_LIVE;
    return patch_put(io, words, nw);
}

int ngl_mon_start(const struct ngl_io *io, const struct ngl_start *s, int force)
{
    int rc;

    if (!mon_args_ok(s->mon, s->mon_len) || !patch_args_ok(s->patch, s->patch_nw) ||
        !status_args_ok(s->memcfg, s->mile, s->log_mode))
        return NGL_E_ARG;
    if (!force && ngl_mon_live(io))
        return NGL_E_LIVE;
    rc = mon_write(io, s->mon, s->mon_len);
    if (!rc)
        rc = patch_put(io, s->patch, s->patch_nw);
    if (!rc)
        rc = status_write(io, s->memcfg, s->mile, s->log_mode, s->cdrv);
    if (!rc)
        rc = ngl_cpu1_take(io, NGL_MON_PA, 1);
    return rc;
}

static int core_is_hyb(const char *b)
{
    size_t l = strlen(PL_CORENAME_HYB);
    return !strncmp(b, PL_CORENAME_HYB, l) && (b[l] == 0 || b[l] == '\n');
}

static int fpga_user_mode(const struct ngl_io *io)
{
    // Main is_fpga_ready(0): init_done read twice against glitches, then STAT mode == user
    if (!(io->rd32(io->ctx, PL_FPGAMGR_GPIO_EXT) & PL_FPGAMGR_INITDONE) ||
        !(io->rd32(io->ctx, PL_FPGAMGR_GPIO_EXT) & PL_FPGAMGR_INITDONE))
        return 0;
    return (io->rd32(io->ctx, PL_FPGAMGR_STAT) & PL_FPGAMGR_MODE_MASK) == PL_FPGAMGR_MODE_USER;
}

// CORENAME lags the real FPGA state across a core reload, so the fpgamgr is read before any bridge or window access
int ngl_window_ok(const struct ngl_io *io)
{
    char b[16];

    if (io->sys_read(io->ctx, NGL_CORENAME, b, sizeof b) < 0 || !core_is_hyb(b))
        return NGL_E_CORE;
    if (!fpga_user_mode(io))
        return NGL_E_FPGA;
    if (io->rd32(io->ctx, NGL_BRGMODRST) & NGL_BRG_RESET)
        return NGL_E_BRIDGES;
    return NGL_OK;
}

static int fpga_ready(const struct ngl_io *io, uint32_t flags)
{
    uint32_t st;
    int rc = ngl_window_ok(io);

    if (rc)
        return rc;
    if (io->rd32(io->ctx, NGL_HYB_REG(HYB_R_MAGIC)) != HYB_MAGIC_VALUE)
        return NGL_E_MAGIC;
    st = io->rd32(io->ctx, NGL_HYB_REG(HYB_R_STATUS));
    if ((st & (HYB_ST_ROM_LOADED | HYB_ST_DL_ACTIVE)) != HYB_ST_ROM_LOADED)
        return NGL_E_NOT_READY;
    if ((flags & NGL_FW_CARD) && !(st & HYB_ST_CARD))
        return NGL_E_NO_CARD;
    return NGL_OK;
}

int ngl_fpga_wait(const struct ngl_io *io, uint32_t flags, uint32_t timeout_ms)
{
    uint32_t t = 0, step;
    int rc;

    for (;;) {
        rc = fpga_ready(io, flags);
        if (rc == NGL_OK || t >= timeout_ms)
            return rc;
        step = timeout_ms - t < NGL_FPGA_POLL_MS ? timeout_ms - t : NGL_FPGA_POLL_MS;
        io->sleep_ms(io->ctx, step);
        t += step;
    }
}

int ngl_watch_step(const struct ngl_io *io, struct ngl_watch *st)
{
    if (st->phase != NGL_W_RUN || io->rd32(io->ctx, NGL_ST(state)) != NGST_STATE_PARKED)
        return 0;
    st->reason = io->rd32(io->ctx, NGL_ST(park_reason));
    st->fatal_kind = st->reason == NGST_PARK_FATAL ? io->rd32(io->ctx, NGL_ST(fatal_rec.kind)) : 0;
    st->rc = ngl_cpu1_return(io);
    st->phase = NGL_W_IDLE;
    return 1;
}
