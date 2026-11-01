// SPDX-License-Identifier: GPL-3.0-or-later
#include "awp.h"

static uint32_t awp_word(const struct awp_io *io, uint32_t va)
{
    uint32_t w;

    return io->rd(io->ctx, va, &w) ? AWP_BAD : w;
}

static void awp_out(struct awp *a, const struct awp_io *io, uint32_t fl, uint32_t pc, uint32_t x, uint32_t y,
                    uint32_t z)
{
    if (a->wchg)
        fl |= AWP_F_WCHG;
    a->wchg = 0;
    io->put(io->ctx, fl, pc, x, y, z);
}

static void awp_setw(struct awp *a, uint32_t w, uint32_t own)
{
    if (w != a->w)
        a->wchg = 1;
    a->w = w;
    a->wown = own;
}

// the owner is read again only when the effective thread changes, so a requester change under a
// constant Supervisor is still seen
static void awp_track(struct awp *a, const struct awp_io *io)
{
    uint32_t s, e = 0, o;

    a->have = 1;
    a->rq = 0;
    a->req = 0;
    if (!a->sup && !io->rd(io->ctx, AWP_SUP, &s))
        a->sup = s;
    a->cur = awp_word(io, AWP_CUR);
    if (a->cur != AWP_BAD) {
        e = a->cur;
        if (a->sup && a->cur == a->sup) {
            a->rq = 1;
            a->req = awp_word(io, AWP_REQ);
            if (a->req != AWP_BAD && a->req != AWP_REQ_IDLE && a->req)
                e = a->req;
        }
    }
    a->th = e;
    if (e == a->eff)
        return;
    a->eff = e;
    a->own = 0;
    if (e && io->rd(io->ctx, e + AWP_TH_OWN, &o))
        a->eff = 0;
    else if (e)
        a->own = o;
}

static int awp_game(const struct awp *a, uint32_t gp)
{
    return gp && a->th && a->eff == a->th && a->own == gp;
}

static int awp_wgp(const struct awp *a, uint32_t gp)
{
    return gp && a->w && a->wown == gp;
}

static void awp_grp(struct awp *a, const struct awp_io *io, uint32_t f, uint32_t pc, uint32_t cur, uint32_t req)
{
    uint32_t w = a->w, d = a->d, st, cp, o1 = 0;

    if (awp_wgp(a, io->gp(io->ctx)))
        f |= AWP_F_WGP;
    awp_out(a, io, f | AWP_T_T, pc, cur, req, w);
    if (!w) {
        awp_out(a, io, f | AWP_T_S, AWP_BAD, AWP_BAD, AWP_BAD, AWP_BAD);
    } else {
        st = awp_word(io, w + AWP_TH_STATE);
        cp = awp_word(io, w + AWP_TH_CPSR);
        if (st != AWP_BAD && cp != AWP_BAD)
            st = (st > 0xFFFFu ? 0xFFFFu : st) << 16 | (cp & 0xFFFFu);
        else
            st = AWP_BAD;
        awp_out(a, io, f | AWP_T_S, awp_word(io, w + AWP_TH_PC), awp_word(io, w + AWP_TH_LR), st,
                d ? awp_word(io, d + AWP_D_POS) : AWP_BAD);
    }
    if (d && io->rd(io->ctx, d + AWP_D_O1, &o1))
        o1 = 0;
    if (!o1)
        awp_out(a, io, f | AWP_T_A, AWP_BAD, AWP_BAD, AWP_BAD, AWP_BAD);
    else
        awp_out(a, io, f | AWP_T_A, awp_word(io, o1 + AWP_O_CHUNK), awp_word(io, o1 + AWP_O_BPS),
                awp_word(io, o1 + AWP_O_PEND), awp_word(io, o1 + AWP_O_CREDIT));
}

// trap.c's xc_stk_ea (the kernel's 0x50333a28) with a range check for a chunk at its run address too; the
// stack word i then sits at the chunk's current base + off + 4 i
static uint32_t awp_ea(const struct awp_io *io, uint32_t w, uint32_t sp, uint32_t *ch, uint32_t *cb, uint32_t *off,
                       uint32_t *sz)
{
    uint32_t p, n, t, i, c, b;

    if (io->rd(io->ctx, w + AWP_THR_STK, ch)) {
        *ch = AWP_BAD;
        return AWP_TF_NOCH;
    }
    if (!*ch)
        return AWP_TF_NOCH;
    if (io->rd(io->ctx, *ch + AWP_CH_BASE, cb)) {
        *cb = AWP_BAD;
        return AWP_TF_NOCH;
    }
    if (*cb < AWP_CH_HOME) {
        if (io->rd(io->ctx, *ch + AWP_CH_SIZE, sz))
            return AWP_TF_NOCH;
        *off = sp - *cb;
        return *off < *sz ? 0u : AWP_TF_RANGE;
    }
    if (io->rd(io->ctx, w + AWP_TH_OWN, &p) || io->rd(io->ctx, p + AWP_P_NCH, &n) || io->rd(io->ctx, p + AWP_P_CHS, &t))
        return AWP_TF_HOME | AWP_TF_NOENT;
    for (i = 0; i < n && i < AWP_P_NCH_MAX; i++) {
        if (io->rd(io->ctx, t + AWP_P_ENT * i + 4u, &c))
            return AWP_TF_HOME | AWP_TF_NOENT;
        if (c != *ch)
            continue;
        if (io->rd(io->ctx, t + AWP_P_ENT * i, &b) || io->rd(io->ctx, *ch + AWP_CH_SIZE, sz))
            return AWP_TF_HOME | AWP_TF_NOENT;
        *off = sp - b;
        return *off < *sz ? AWP_TF_HOME : AWP_TF_HOME | AWP_TF_RANGE;
    }
    return AWP_TF_HOME | AWP_TF_NOENT;
}

static void awp_stk(struct awp *a, const struct awp_io *io, uint32_t f)
{
    uint32_t sp = AWP_BAD, ch = AWP_BAD, cb = AWP_BAD, off = 0, sz = 0, fl = AWP_TF_BLK, s[3u * AWP_NK], i, n = 0, x;
    int bad;

    if (a->w && !io->rd(io->ctx, a->w + AWP_TH_SP, &sp))
        fl = awp_ea(io, a->w, sp, &ch, &cb, &off, &sz);
    else
        sp = AWP_BAD;
    for (i = 0; i < 3u * AWP_NK; i++)
        s[i] = (fl & ~AWP_TF_HOME) || off + 4u * i >= sz ? AWP_BAD : awp_word(io, cb + off + 4u * i);
    awp_out(a, io, f | AWP_S_STK | AWP_T_T, sp, fl, ch, cb);
    for (i = 0; i < AWP_NK; i++)
        awp_out(a, io, f | AWP_S_STK | AWP_T_K | AWP_RIX(i), sp == AWP_BAD ? AWP_BAD : sp + 12u * i, s[3u * i],
                s[3u * i + 1u], s[3u * i + 2u]);
    for (i = 0; i < 3u * AWP_NK && n < AWP_NBL; i++) {
        if (s[i] < AWP_RAMCODE_LO || s[i] >= AWP_RAMCODE_HI)
            continue;
        // a Thumb return has bit 0 set: the aligned word before it holds the bl pair (trap.c xc_scan)
        bad = io->rd(io->ctx, (s[i] - 4u) & ~3u, &x);
        awp_out(a, io, f | AWP_S_STK | AWP_T_S | AWP_RIX(n), s[i], bad ? AWP_BAD : x, i, bad ? 1u : 0u);
        n++;
    }
}

// once per game process; whether its chunks hold the DLL code is unproven, so no verdict needs these
static void awp_proc(struct awp *a, const struct awp_io *io, uint32_t gp)
{
    uint32_t n, t, i, f, e, ch;

    if (!gp || gp == a->pgp) {
        a->pgp = gp;
        return;
    }
    a->pgp = gp;
    if (io->rd(io->ctx, gp + AWP_P_NCH, &n) || io->rd(io->ctx, gp + AWP_P_CHS, &t))
        return;
    f = n > AWP_P_NCH_MAX ? AWP_F_TRUNC : 0u;
    for (i = 0; i < n && i < AWP_P_NCH_MAX; i++) {
        e = t + AWP_P_ENT * i;
        ch = awp_word(io, e + 4u);
        if (!ch || ch == AWP_BAD)
            awp_out(a, io, AWP_S_PROC | AWP_T_T | AWP_RIX(i) | f, awp_word(io, e), ch, AWP_BAD, AWP_BAD);
        else
            awp_out(a, io, AWP_S_PROC | AWP_T_T | AWP_RIX(i) | f, awp_word(io, e), ch, awp_word(io, ch + AWP_CH_BASE),
                    awp_word(io, ch + AWP_CH_SIZE));
    }
}

void awp_m3(struct awp *a, const struct awp_io *io, uint32_t pc, uint32_t d)
{
    uint32_t cur = awp_word(io, AWP_CUR);

    a->st = 1;
    a->nm = 0;
    a->idle = 0;
    a->d = d;
    awp_grp(a, io, AWP_S_M3, pc, cur, awp_word(io, AWP_REQ));
    awp_proc(a, io, io->gp(io->ctx));
}

void awp_irq(struct awp *a, const struct awp_io *io, uint32_t pc)
{
    uint32_t cur;

    if (a->st != 1)
        return;
    a->st = 2;
    cur = awp_word(io, AWP_CUR);
    awp_grp(a, io, AWP_S_IRQ, pc, cur, awp_word(io, AWP_REQ));
    awp_stk(a, io, awp_wgp(a, io->gp(io->ctx)) ? AWP_F_WGP : 0u);
}

void awp_kick(struct awp *a, const struct awp_io *io, uint32_t pc)
{
    uint32_t gp = io->gp(io->ctx);

    awp_track(a, io);
    if (!gp && a->th)
        awp_setw(a, a->th, a->eff == a->th ? a->own : 0u);
    awp_out(a, io, AWP_S_KICK | AWP_T_T | (awp_game(a, gp) ? AWP_F_WGP : 0u), pc, a->cur, a->rq ? a->req : 0u, a->th);
}

void awp_mmio(struct awp *a, const struct awp_io *io, uint32_t pc, uint32_t gm)
{
    uint32_t gp;

    // an IRQ or FIQ handler's access is not the thread's
    if (gm == 0x11u || gm == 0x12u) {
        a->have = 0;
        return;
    }
    if (!a->have)
        awp_track(a, io);
    a->have = 0;
    gp = io->gp(io->ctx);
    if (awp_game(a, gp))
        awp_setw(a, a->th, a->own);
    if (!a->st)
        return;
    awp_grp(a, io, AWP_S_MMIO | AWP_IX(a->nm), pc, a->cur, a->rq ? a->req : awp_word(io, AWP_REQ));
    if (++a->nm == AWP_NMMIO)
        a->st = 0;
}

void awp_idle(struct awp *a, const struct awp_io *io, uint32_t pc)
{
    uint32_t w = a->w;

    if (a->st != 1 || a->idle)
        return;
    a->idle = 1;
    awp_out(a, io, AWP_S_IDLE | AWP_T_T | (awp_wgp(a, io->gp(io->ctx)) ? AWP_F_WGP : 0u), pc, awp_word(io, AWP_CUR),
            w ? awp_word(io, w + AWP_TH_PC) : AWP_BAD, w ? awp_word(io, w + AWP_TH_LR) : AWP_BAD);
}
