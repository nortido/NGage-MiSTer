// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/hyb_regs.h"
#include "../common/platform.h"
#include "../common/status.h"
#include "guest_census.h"

#define IRQ_ID_MASK     0x3FFu
#define IRQ_ID_SPURIOUS 1023u
#define TICK_FPS_DIV    1000u
#define TICK_DBG_DIV    64u
#define BRG_RESET_MASK  0x7u
#define PSR_MODE_MASK   0x1Fu
#define PSR_MODE_ABT    0x17u
#define PSR_MODE_UND    0x1Bu
#define GICC_CTLR_FIQ   0xFu
#define GICD_CTLR_GRP1  0x2u
#define GIC_PRI_FIQ     0xA0u
#define GIC_TGT_CPU1    0x02u
#define GIC_FIQ_NWR     10u
#define GIC_PA_DCTLR    (PL_GICD_BASE + 0x000u)
#define GIC_PA_CCTLR    (PL_GICC_BASE + 0x000u)
#define GIC_PA_IGRP(id) (PL_GICD_BASE + 0x080u + ((id) >> 5) * 4u)
#define GIC_PA_ISEN(id) (PL_GICD_BASE + 0x100u + ((id) >> 5) * 4u)
#define GIC_PA_ICEN(id) (PL_GICD_BASE + 0x180u + ((id) >> 5) * 4u)
#define GIC_PA_ISPD(id) (PL_GICD_BASE + 0x200u + ((id) >> 5) * 4u)
#define GIC_PA_ICPD(id) (PL_GICD_BASE + 0x280u + ((id) >> 5) * 4u)
#define GIC_PA_PRI(id)  (PL_GICD_BASE + 0x400u + (id))
#define GIC_PA_TGT(id)  (PL_GICD_BASE + 0x800u + (id))
#define GIC_PA_ICFG(id) (PL_GICD_BASE + 0xC00u + ((id) >> 4) * 4u)
#define GIC_ID_BIT(id)  (1u << ((id) & 31u))
#define GIC_CFG_EDGE(id) (2u << (((id) & 15u) * 2u))
#define GIC_ID_PWDT     30u

enum { IK_SPURIOUS = 0, IK_GUEST = 1, IK_TICK = 2, IK_OTHER = 3, IK_RCK = 4 };
enum { TA_LOST = 0x1, TA_PARK = 0x2, TA_FLUSH = 0x4, TA_RESTART = 0x8, TA_FPS = 0x10, TA_DBG = 0x20 };

struct irq_tick {
    uint32_t ticks;
};

struct gic_fiq {
    uint32_t dctl, cctl, gr0, gr2, cf4, en, pend, pri, tgt;
};

struct gic_wr {
    uint32_t pa;
    uint32_t val;
    uint32_t byte;
};

// core 1's banked state a park-less return leaves from gic_fiq_cfg; start.S c1rst stores each val as a word
const struct gic_wr gic_c1rst[] = {
    { GIC_PA_CCTLR, 0u, 0u },
    { GIC_PA_IGRP(0), 0u, 0u },
};
const uint32_t gic_c1rst_n = sizeof gic_c1rst / sizeof gic_c1rst[0];

int irq_kind(uint32_t iar)
{
    uint32_t id = iar & IRQ_ID_MASK;

    if (id == IRQ_ID_SPURIOUS)
        return IK_SPURIOUS;
    if (id == PL_GIC_ID_HYB_IRQ)
        return IK_GUEST;
    if (id == PL_GIC_ID_PTIMER)
        return IK_TICK;
    if (id == GIC_ID_PWDT)
        return IK_RCK;
    return IK_OTHER;
}

// any of the three bridges in reset: the window may hang, never touch it
int irq_bridge_ok(uint32_t brgmodrst)
{
    return (brgmodrst & BRG_RESET_MASK) == 0;
}

void irq_tick_init(struct irq_tick *t)
{
    t->ticks = 0;
}

uint32_t irq_tick(struct irq_tick *t, uint32_t magic, uint32_t status, uint32_t park_req, uint32_t flush_req,
                  uint32_t flush_ack)
{
    uint32_t a = 0;

    t->ticks++;
    if (magic != HYB_MAGIC_VALUE)
        return TA_LOST;
    if (park_req == NGST_PARK_REQUEST)
        return TA_PARK;
    if (flush_req != flush_ack)
        a |= TA_FLUSH;
    if (status & (HYB_ST_DL_ACTIVE | HYB_ST_OSD_RESET))
        a |= TA_RESTART;
    if (t->ticks % TICK_FPS_DIV == 0)
        a |= TA_FPS;
    if (t->ticks % TICK_DBG_DIV == 0)
        a |= TA_DBG;
    return a;
}

#ifndef NGH_FIQ_UNDFWD
#define NGH_FIQ_UNDFWD  1
#endif

enum { FC_MON = 0, FC_G_ABT = 1, FC_G_UND = 2, FC_G_OTHER = 3 };

struct fiqd {
    uint32_t dg_und, dg_abt, d_mon, carry, last_glr;
};

uint32_t fiq_class(uint32_t lr, uint32_t spsr)
{
    uint32_t m = spsr & PSR_MODE_MASK;

    if (lr - MON_VA_BASE < PL_MON_SIZE)
        return FC_MON;
    if (m == PSR_MODE_ABT)
        return FC_G_ABT;
    return m == PSR_MODE_UND ? FC_G_UND : FC_G_OTHER;
}

// a monitor trap, a guest ABT context and a live forwarded UND handler would lose lr and SPSR to a nested
// abort; a guest in UND mode by its own switch has none, and the ARM920T takes the FIQ there
int fiq_defer(uint32_t lr, uint32_t spsr, uint32_t live_und)
{
    uint32_t c = fiq_class(lr, spsr);

    return c == FC_MON || c == FC_G_ABT || (c == FC_G_UND && (live_und || !NGH_FIQ_UNDFWD));
}

void fiq_count(struct fiqd *d, uint32_t cls, uint32_t pc)
{
    if (cls == FC_MON) {
        d->d_mon++;
        return;
    }
    if (cls == FC_G_UND)
        d->dg_und++;
    else if (cls == FC_G_ABT)
        d->dg_abt++;
    d->last_glr = pc;
}

static uint32_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : v;
}

uint32_t fiq_row(uint32_t fwd, uint32_t def)
{
    return sat16(fwd) << 16 | sat16(def);
}

static uint32_t gw(struct gic_wr *w, uint32_t n, uint32_t pa, uint32_t val, uint32_t byte)
{
    w[n].pa = pa;
    w[n].val = val;
    w[n].byte = byte;
    return n + 1u;
}

// 75 Group 0 to CPU1, 74 and core 1's banked SGIs/PPIs Group 1; 75 is enabled last
uint32_t gic_fiq_cfg(const struct gic_fiq *s, struct gic_wr *w)
{
    uint32_t id = PL_GIC_ID_HYB_FIQ, n = 0;

    n = gw(w, n, GIC_PA_ICEN(id), GIC_ID_BIT(id), 0);
    if (s->cf4 & GIC_CFG_EDGE(id))
        n = gw(w, n, GIC_PA_ICFG(id), s->cf4 & ~GIC_CFG_EDGE(id), 0);
    n = gw(w, n, GIC_PA_TGT(id), GIC_TGT_CPU1, 1);
    n = gw(w, n, GIC_PA_PRI(id), GIC_PRI_FIQ, 1);
    n = gw(w, n, GIC_PA_IGRP(id), (s->gr2 | GIC_ID_BIT(PL_GIC_ID_HYB_IRQ)) & ~GIC_ID_BIT(id), 0);
    n = gw(w, n, GIC_PA_IGRP(0), 0xFFFFFFFFu, 0);
    n = gw(w, n, GIC_PA_DCTLR, s->dctl | GICD_CTLR_GRP1, 0);
    n = gw(w, n, GIC_PA_CCTLR, GICC_CTLR_FIQ, 0);
    return gw(w, n, GIC_PA_ISEN(id), GIC_ID_BIT(id), 0);
}

uint32_t gic_fiq_restore(const struct gic_fiq *s, struct gic_wr *w)
{
    uint32_t id = PL_GIC_ID_HYB_FIQ, n = 0;

    n = gw(w, n, GIC_PA_ICEN(id), GIC_ID_BIT(id), 0);
    if (!s->pend)
        n = gw(w, n, GIC_PA_ICPD(id), GIC_ID_BIT(id), 0);
    n = gw(w, n, GIC_PA_CCTLR, s->cctl, 0);
    n = gw(w, n, GIC_PA_IGRP(0), s->gr0, 0);
    n = gw(w, n, GIC_PA_IGRP(id), s->gr2, 0);
    n = gw(w, n, GIC_PA_DCTLR, s->dctl, 0);
    if (s->cf4 & GIC_CFG_EDGE(id))
        n = gw(w, n, GIC_PA_ICFG(id), s->cf4, 0);
    n = gw(w, n, GIC_PA_PRI(id), s->pri, 1);
    n = gw(w, n, GIC_PA_TGT(id), s->tgt, 1);
    if (s->en)
        n = gw(w, n, GIC_PA_ISEN(id), GIC_ID_BIT(id), 0);
    return n;
}

#ifndef NGH_LIM_CAP_MS
#define NGH_LIM_CAP_MS  16
#endif
#define LIM_FLOOR_MS    16
#ifndef NGH_LIM_FRAME_US
#define NGH_LIM_FRAME_US 0
#endif
#if NGH_LIM_FRAME_US
_Static_assert(NGH_LIM_FRAME_US <= 100000u, "frame cost up to 100 ms");
#define LIM_FC_NFL      2
// T x ipm / 1000 split at 1 ms, as the monitor has no libgcc for a 64-bit divide (exact for ipm < 4.29M)
#define LIM_FC_DEBIT(t, ipm) ((t) / 1000u * (ipm) + (t) % 1000u * (ipm) / 1000u)
#endif
#ifndef NGH_LIM_TALIGN
#define NGH_LIM_TALIGN  0
#endif
#ifndef NGH_LIM_PREW
#define NGH_LIM_PREW    0
#endif
#ifndef NGH_LIM_PREA
#define NGH_LIM_PREA    0
#endif
#ifndef NGH_LIM_XHOLD
#define NGH_LIM_XHOLD   0
#endif
#if NGH_LIM_XHOLD
#ifndef LIM_FC_DEBIT
#error "the exact hold splits the frame debit"
#endif
// the null loop proper less its literal pool vouches for the object; S1 arms only at
// the idle pcs (DisableAll and Restore, the null thread's function, its clear path, the stub vcall)
#define LIM_XH_NULL     0x80406C48u
#define LIM_XH_LOOP_LO  0x50331A60u
#define LIM_XH_LOOP_HI  0x50331BD4u
#define LIM_XH_EXIT_LO  0x503274E4u
#define LIM_XH_EXIT_HI  0x50327504u
#define LIM_XH_DFC_LO   0x50327F38u
#define LIM_XH_DFC_HI   0x50327FA8u
#define LIM_XH_LIT_LO   0x50331BB4u
#define LIM_XH_LIT_HI   0x50331BCCu
#define LIM_XH_RST_LO   0x503222CCu
#define LIM_XH_RST_HI   0x5032230Cu
#define LIM_XH_NUL_LO   0x50331A30u
#define LIM_XH_NUL_HI   0x50331BD4u
#define LIM_XH_CLR      0x50323C74u
#define LIM_XH_STB_LO   0x50320948u
#define LIM_XH_STB_HI   0x50320960u
#define LIM_XH_M_FIQ    0x11u
#define LIM_XH_M_IRQ    0x12u
#endif
#if NGH_LIM_TALIGN
_Static_assert(NGH_LIM_FRAME_US != 0, "only frame debits are aligned");
#if NGH_PMU_CENSUS
#error "the align rows need a build without the census"
#endif
// the guest clears timer 0's expired flag in its own ISR, so this write means its 64 Hz tick was taken
#define LIM_TA_ACK_PA   0x0C120A08u
// one guest tick of 15.625 ms plus the 1 ms quantum
#define LIM_TA_MAX_MS   17u
#define LIM_TA_IDLE     0u
#define LIM_TA_DEBT     1u
#define LIM_TA_WAIT     2u
#ifndef NGH_LIM_PRE_US
#define NGH_LIM_PRE_US  0
#endif
#if NGH_LIM_PRE_US
_Static_assert(NGH_LIM_PRE_US >= 1000u && NGH_LIM_PRE_US <= 8000u, "a release 1 to 8 ms before the edge");
// the edge is predicted at the tick ack + 15.625 ms; the release falls on the ms tick nearest edge - d
#define LIM_PRE_N       ((15625u - NGH_LIM_PRE_US + 500u) / 1000u)
#define LIM_PRE_MAX_MS  (LIM_TA_MAX_MS + LIM_PRE_N)
#if NGH_LIM_PREA && NGH_LIM_PREW
#error "the pre-point arm is the first half of the window release"
#endif
#if NGH_LIM_PCARRY
#if !NGH_LIM_PREA
#error "the pre-point carry is a release of the pre-point arm"
#endif
#ifndef NGH_LIM_PCARRY_M_US
#define NGH_LIM_PCARRY_M_US 125u
#endif
_Static_assert(NGH_LIM_PCARRY_M_US <= 500u, "a margin of 0 to 500 us before the edge");
#define LIM_SR_GT       200u
#define LIM_SR_PER      3125000u
#define LIM_SR_GAP      1600000000u
#define LIM_PC_M        ((uint64_t)NGH_LIM_PCARRY_M_US * LIM_SR_GT)
#define LIM_PC_LEAD     (2000u * LIM_SR_GT)
#endif
#elif NGH_LIM_PREW
#error "the window release is an option of the pre-edge release"
#elif NGH_LIM_PREA
#error "the pre-point arm is an option of the pre-edge release"
#elif NGH_LIM_PCARRY
#error "the pre-point carry is an option of the pre-edge release"
#endif
#elif NGH_LIM_PRE_US
#error "the pre-edge release is an option of the tick-aligned release"
#elif NGH_LIM_PREW
#error "the window release is an option of the pre-edge release"
#elif NGH_LIM_PREA
#error "the pre-point arm is an option of the pre-edge release"
#elif NGH_LIM_PCARRY
#error "the pre-point carry is an option of the pre-edge release"
#endif
#define LIM_ARM_NW      128u
#define LIM_ARM_MS      60000u
#define LIM_EV_INST     0x08u
#define LIM_EV_RENAME   0x68u
#define LIM_PM_LO       950u
#define LIM_PM_HI       1100u
#define LIM_SCU_CPU1(s) ((s) >> 8 & 3u)
#ifndef NGH_LIM_IPM
#define NGH_LIM_IPM     65000u
#endif
#ifndef NGH_LIM_FORCE_TURBO
#define NGH_LIM_FORCE_TURBO 0
#endif
#ifndef NGH_LIM_RCK_US
#define NGH_LIM_RCK_US  50u
#endif
#define LIM_RCK_US      NGH_LIM_RCK_US
#define LIM_RCK_LOAD(us) ((us) * 200u - 1u)
#define LIM_WDT_RUN     0x5u
#define LIM_WDT_MODE    0x8u
#ifndef NGH_LIM_WLOOP
#define NGH_LIM_WLOOP   1
#endif
#ifndef NGH_LIM_SLB_US
#define NGH_LIM_SLB_US  50u
#endif
#define LIM_SLB_US      NGH_LIM_SLB_US
#ifndef NGH_LIM_RCK_MIN_US
#define NGH_LIM_RCK_MIN_US 2u
#endif
#define LIM_RCK_MIN_US  NGH_LIM_RCK_MIN_US
// a minimal slice of 0 would wrap LIM_RCK_LOAD; the re-check off build (r 0) never loads one
#define LIM_MIN_OK(r, m) ((r) == 0 || ((m) >= 1 && (m) <= (r)))
_Static_assert(LIM_MIN_OK(LIM_RCK_US, LIM_RCK_MIN_US), "re-check minimal slice");
#define LIM_HOLD_US     2000u
#define PSR_F           0x40u

enum { WK_TICK = 0, WK_IRQ = 1, WK_FIQ = 2, WK_RCK = 3, WK_MASK = 4, WK_NONE = 5 };

struct lim {
    uint32_t ent, mon, ticks, ipm, turbo;
    int64_t bal;
    uint32_t armed, cause, kick, tarm, nwmax, nwarm;
    uint32_t run, smx, sgi, sst, gip, stl;
    uint32_t ev, mev, flr, wak, ilm, sgt, wkp, zrun, zmx;
    uint64_t wkt;
    uint32_t rck, rck_n, rst_n, wdt0;
    uint32_t wloop, slb, slm, wki, wkf, wkm, wkn, hld, fms, fmp, flk;
#if NGH_LIM_FRAME_US
    uint32_t fnew;
#endif
#if NGH_LIM_XHOLD
    uint32_t xrd, xvf, xvd, xdn, xarm, xf1, xfn, xks, xke, xkm, xkf, xp1, xp2, xan, xam;
#endif
#if NGH_LIM_TALIGN
    uint32_t tas, tak, tmk, taw, tan, tar, tat, tmr, tam, tah, tans, tars, tahs;
#if NGH_LIM_PRE_US
    uint32_t tack, tav, tpa, tpr, tprs;
#if NGH_LIM_PREW
    uint32_t tpl;
#endif
#endif
#endif
#if NGH_LIM_PCARRY
    uint64_t snow, sle, sla;
    int64_t pcar;
    uint32_t slv;
#endif
};

void lim_reset(struct lim *l, uint32_t ent, uint32_t mon)
{
    l->ent = ent;
    l->mon = mon;
    l->ticks = 0;
    l->ipm = 0;
    l->turbo = 0;
    l->bal = 0;
    l->armed = 0;
    l->cause = 0;
    l->kick = 0;
    l->tarm = 0;
    l->nwmax = 0;
    l->nwarm = 0;
    l->run = 0;
    l->smx = 0;
    l->sgi = 0;
    l->sst = 0;
    l->gip = 0;
    l->stl = 0;
    l->wkp = 0;
    l->zrun = 0;
    l->zmx = 0;
#if NGH_LIM_FRAME_US
    l->fnew = 0;
#endif
#if NGH_LIM_XHOLD
    l->xrd = 0;
    l->xvf = 0;
    l->xvd = 0;
    l->xdn = 0;
    l->xarm = 0;
    l->xf1 = 0;
    l->xfn = 0;
    l->xks = 0;
    l->xke = 0;
    l->xkm = 0;
    l->xkf = 0;
    l->xp1 = 0;
    l->xp2 = 0;
    l->xan = 0;
    l->xam = 0;
#endif
#if NGH_LIM_TALIGN
    l->tas = LIM_TA_IDLE;
    l->tak = 0;
    l->tmk = 0;
    l->taw = 0;
    l->tan = 0;
    l->tar = 0;
    l->tat = 0;
    l->tmr = 0;
    l->tam = 0;
    l->tah = 0;
    l->tans = 0;
    l->tars = 0;
    l->tahs = 0;
#if NGH_LIM_PRE_US
    l->tack = 0;
    l->tav = 0;
    l->tpa = 0;
    l->tpr = 0;
    l->tprs = 0;
#if NGH_LIM_PREW
    l->tpl = 0;
#endif
#endif
#endif
#if NGH_LIM_PCARRY
    l->snow = 0;
    l->sle = 0;
    l->sla = 0;
    l->pcar = 0;
    l->slv = 0;
#endif
}

void lim_frame(struct lim *l, uint32_t nw, uint32_t kick)
{
    if (l->armed)
        return;
    if (nw < LIM_ARM_NW) {
        if (nw > l->nwmax)
            l->nwmax = nw;
        return;
    }
    l->armed = 1;
    l->cause = 1;
    l->kick = kick;
    l->tarm = l->ticks;
    l->nwarm = nw;
}

#if NGH_LIM_XHOLD
static uint32_t lim_xin(uint32_t pc, uint32_t lo, uint32_t hi)
{
    return pc - lo <= hi - lo;
}

static uint32_t lim_xown(uint32_t pc)
{
    return lim_xin(pc, LIM_XH_LOOP_LO, LIM_XH_LIT_LO - 4u) || lim_xin(pc, LIM_XH_LIT_HI + 4u, LIM_XH_LOOP_HI);
}

static uint32_t lim_xidle(uint32_t pc)
{
    return lim_xin(pc, LIM_XH_RST_LO, LIM_XH_RST_HI) || lim_xin(pc, LIM_XH_NUL_LO, LIM_XH_NUL_HI) || pc == LIM_XH_CLR ||
           lim_xin(pc, LIM_XH_STB_LO, LIM_XH_STB_HI);
}

void lim_xcur(struct lim *l, uint32_t ok, uint32_t w, uint32_t pc, uint32_t mode)
{
    uint32_t nul = ok && w == LIM_XH_NULL;

    l->xrd = 0;
    // only a tick in the null loop's own body vouches for the object; a failed read says nothing
    if (ok && !l->xvd && lim_xown(pc)) {
        if (nul) {
            l->xvf = 1;
        } else if (l->xvf) {
            l->xvf = 0;
            l->xvd = 1;
            l->xdn++;
            l->xarm = 0;
            l->xks = 0;
        }
    }
    if (!l->xvf || !nul)
        return;
    l->xrd = 1;
    if (mode != LIM_XH_M_IRQ && mode != LIM_XH_M_FIQ && lim_xidle(pc) && !lim_xin(pc, LIM_XH_EXIT_LO, LIM_XH_EXIT_HI) &&
        !lim_xin(pc, LIM_XH_DFC_LO, LIM_XH_DFC_HI))
        l->xrd = 3;
}

void lim_xfwd(struct lim *l, uint32_t has, uint32_t ent, uint32_t mon)
{
    l->xfn++;
    if (l->xarm != 1u)
        return;
    l->xarm = 2;
    l->xf1 = has ? (ent - l->ent) - (mon - l->mon) : 0u;
}

void lim_xkick(struct lim *l, uint32_t ok, uint32_t ent, uint32_t mon)
{
    l->xks = ok;
    l->xke = ent;
    l->xkm = mon;
    l->xkf = l->xfn;
}

static uint32_t lim_xfree(struct lim *l, uint32_t g, uint32_t pre)
{
    uint32_t f1 = 0, f2 = 0;

    if (l->xarm) {
        l->xam++;
        if (l->xarm == 2u)
            f1 = l->xf1 < g ? l->xf1 : g;
        else if (l->xrd)
            f1 = g;
        else
            l->xan++;
    }
    if (l->xks && l->xrd && l->xkf == l->xfn)
        f2 = g - (pre < g ? pre : g);
    l->xp1 = f1;
    l->xp2 = f2;
    l->xarm = l->xrd == 3u;
    l->xf1 = 0;
    l->xks = 0;
    l->xrd = 0;
    // a forward splits the two parts; a segment with no forward already holds the whole ms
    return f2 > g - f1 ? g : f1 + f2;
}
#endif

#if NGH_LIM_TALIGN
uint32_t lim_tack(struct lim *l, uint32_t pa)
{
    if ((pa & ~3u) != LIM_TA_ACK_PA)
        return 0;
    l->tan++;
#if NGH_LIM_PRE_US
    // an ack before the armed pre-point means the prediction ran late; release at it as TA does
    l->tack = l->ticks;
    l->tav = 1;
    if (l->tas == LIM_TA_WAIT) {
#if NGH_LIM_PREW
        // paid past the pre-point, so this ack releases as in TA, not the pre-point after it
        if (l->tpa || l->tpl)
#else
        if (l->tpa)
#endif
            l->tak = 1;
        else
            l->tpa = 1;
    }
#else
    if (l->tas == LIM_TA_WAIT)
        l->tak = 1;
#endif
    return 1;
}

static void lim_trel(struct lim *l)
{
    l->tas = LIM_TA_IDLE;
#if NGH_LIM_PREW
    l->tpl = 0;
#endif
    if (l->tak)
        l->tar++;
#if NGH_LIM_PRE_US
    else if (!l->tmk)
        l->tpr++;
#endif
    else
        l->tmr++;
}

#if NGH_LIM_PRE_US
#if NGH_LIM_PCARRY
void lim_snow(struct lim *l, uint64_t t)
{
    l->snow = t;
}

void lim_sack(struct lim *l, uint64_t t)
{
    uint64_t n;
    uint32_t k;
    int32_t off;

    if (!l->slv || t - l->sla > LIM_SR_GAP) {
        l->slv = 1;
        l->sle = t;
        l->sla = t;
        return;
    }
    l->sla = t;
    k = ((uint32_t)(t - l->sle) + LIM_SR_PER / 2u) / LIM_SR_PER;
    n = l->sle + (uint64_t)k * LIM_SR_PER;
    off = (int32_t)(t - n);
    // an ack before the lattice bounds the edge's latency, so the lattice moves to it; a later one
    // drifts it up by a sixteenth, enough to follow the clocks' drift, not one late ack
    l->sle = off < 0 ? t : n + (uint32_t)off / 16u;
}

static uint32_t lim_slat(const struct lim *l, uint64_t t)
{
    return l->slv && (t < l->sla || t - l->sla <= LIM_SR_GAP);
}

static uint64_t lim_sedge(const struct lim *l, uint64_t x)
{
    if (x <= l->sle)
        return l->sle - (uint64_t)((uint32_t)(l->sle - x) / LIM_SR_PER) * LIM_SR_PER;
    return l->sle + (uint64_t)(((uint32_t)(x - l->sle) + LIM_SR_PER - 1u) / LIM_SR_PER) * LIM_SR_PER;
}

static void lim_pcar(struct lim *l, int64_t b)
{
    uint64_t e, r;

    if (l->tas != LIM_TA_DEBT || l->bal >= 0 || !l->tav || l->ticks - l->tack != LIM_PRE_N || !lim_slat(l, l->snow))
        return;
    e = lim_sedge(l, l->snow) - l->snow;
    r = (uint64_t)-l->bal;
    // paid at the stall rate by E* - M; M is checked first so e - M cannot wrap, products only (no libgcc)
    if (e <= LIM_PC_M || e > LIM_PC_LEAD || r > 2u * (uint64_t)b || r * LIM_SR_GT * 1000u > (e - LIM_PC_M) * (uint64_t)b)
        return;
    l->pcar = (int64_t)r;
    l->bal = 0;
}
#endif
#endif
#endif

uint32_t lim_tick(struct lim *l, uint32_t ent, uint32_t mon, uint32_t status, uint32_t ipm)
{
    // ent and mon are free-running counters, so unsigned deltas survive a wrap
    uint32_t g = (ent - l->ent) - (mon - l->mon);
    int64_t b = ipm, cap = NGH_LIM_CAP_MS * b;
#if NGH_LIM_FRAME_US
    uint32_t n = l->fnew;
    int64_t d = LIM_FC_DEBIT(NGH_LIM_FRAME_US, ipm);
#if NGH_LIM_XHOLD
    uint32_t xpre = (l->xke - l->ent) - (l->xkm - l->mon), fr;
    int64_t xa;
#endif

    l->fnew = 0;
#endif

    l->ent = ent;
    l->mon = mon;
    l->ticks++;
    l->wkp = 0;
    l->slb = LIM_SLB_US;
    l->sgi += g;
    l->ipm = ipm;
    l->turbo = (status & HYB_ST_TURBO) || NGH_LIM_FORCE_TURBO;
    if (!l->armed && l->ticks >= LIM_ARM_MS) {
        l->armed = 1;
        l->cause = 2;
        l->kick = 0;
        l->tarm = l->ticks;
    }
    if (l->turbo || !l->armed || !ipm) {
        l->bal = 0;
        l->run = 0;
        l->zrun = 0;
#if NGH_LIM_TALIGN
        l->tas = LIM_TA_IDLE;
        l->tak = 0;
        l->tmk = 0;
        l->taw = 0;
#if NGH_LIM_PRE_US
        l->tpa = 0;
#if NGH_LIM_PREW
        l->tpl = 0;
#endif
#endif
#endif
#if NGH_LIM_XHOLD
        l->xarm = 0;
        l->xf1 = 0;
        l->xks = 0;
        l->xrd = 0;
#endif
#if NGH_LIM_PCARRY
        l->pcar = 0;
        l->slv = 0;
#endif
        return 0;
    }
    if (g < ipm / 64u) {
        if (++l->zrun > l->zmx)
            l->zmx = l->zrun;
    } else {
        l->zrun = 0;
    }
#if NGH_LIM_FRAME_US
    // the debit stays out of g and the sgi count; the floor keps two debits on top of the old one
    l->bal = (l->bal + b < cap ? l->bal + b : cap) - g - n * d;
#if NGH_LIM_PCARRY
    if (n) {
        l->bal -= l->pcar;
        l->pcar = 0;
    }
#endif
#if NGH_LIM_XHOLD
    // bal = max(today, min(today + fr, 0)), so the free part never banks credit
    fr = lim_xfree(l, g, xpre);
    if (fr) {
        xa = l->bal + (int64_t)fr;
        if (xa > 0)
            xa = 0;
        if (xa > l->bal)
            l->bal = xa;
    }
#endif
    if (l->bal < -(LIM_FLOOR_MS * b + LIM_FC_NFL * d)) {
        l->bal = -(LIM_FLOOR_MS * b + LIM_FC_NFL * d);
        l->flr++;
    }
#if NGH_LIM_TALIGN
    // a paid debt waits for the guest's next tick ack; the wait is not banked, or later frames would shrink
    if (n)
        l->tas = LIM_TA_DEBT;
#if NGH_LIM_PCARRY
    lim_pcar(l, b);
#endif
    if (l->tas == LIM_TA_DEBT && l->bal >= 0) {
        l->tas = LIM_TA_WAIT;
        l->tak = 0;
        l->tmk = 0;
        l->taw = 0;
#if NGH_LIM_PRE_US
        // with PREW or PREA a payment at the pre-point tick arms it and is released in it
        l->tpa = l->tav && l->ticks - l->tack < LIM_PRE_N + (NGH_LIM_PREW || NGH_LIM_PREA);
#if NGH_LIM_PREW
        l->tpl = l->tav && !l->tpa;
#endif
#endif
    }
    if (l->tas == LIM_TA_WAIT) {
        if (l->tak || l->tmk) {
            lim_trel(l);
#if NGH_LIM_PRE_US
        } else if (l->tpa && l->ticks - l->tack >= LIM_PRE_N) {
            lim_trel(l);
        } else if (l->taw >= LIM_PRE_MAX_MS) {
#else
        } else if (l->taw >= LIM_TA_MAX_MS) {
#endif
            l->tas = LIM_TA_IDLE;
            l->tat++;
        } else {
            if (++l->taw > l->tam)
                l->tam = l->taw;
            l->tah++;
            if (l->bal > b)
                l->bal = b;
            l->sst++;
            if (++l->run > l->smx)
                l->smx = l->run;
            return 1;
        }
    }
#endif
#else
    l->bal = (l->bal + b < cap ? l->bal + b : cap) - g;
    if (l->bal < -LIM_FLOOR_MS * b) {
        l->bal = -LIM_FLOOR_MS * b;
        l->flr++;
    }
#endif
    if (l->bal >= 0) {
        l->run = 0;
        return 0;
    }
    l->sst++;
    if (++l->run > l->smx)
        l->smx = l->run;
    return 1;
}

#if NGH_LIM_COST
// Q8 phone cycles per guest 0x68 instruction and per event of PMUC_EVS; FW-A's centre (M1, s = 1) until
// a later fit
#ifndef LIM_CW0
#define LIM_CW0         307
#endif
#ifndef LIM_CW1
#define LIM_CW1         (-2458)
#endif
#ifndef LIM_CW2
#define LIM_CW2         77
#endif
#ifndef LIM_CW3
#define LIM_CW3         59290
#endif
#ifndef LIM_CW4
#define LIM_CW4         6420
#endif
#ifndef LIM_CW5
#define LIM_CW5         0
#endif
#ifndef NGH_LIM_CPM
#define NGH_LIM_CPM     104000u
#endif

struct lcost {
    uint32_t ent, mon, cum, sgi, gip;
    uint32_t ev[PMUC_N], mev[PMUC_N];
};

static const int32_t lim_cw[1u + PMUC_N] = { LIM_CW0, LIM_CW1, LIM_CW2, LIM_CW3, LIM_CW4, LIM_CW5 };

uint32_t lim_cost(const int32_t *w, uint32_t di, const uint32_t *de)
{
    int64_t s = (int64_t)w[0] * di;
    uint32_t k;

    for (k = 0; k < PMUC_N; k++)
        s += (int64_t)w[k + 1u] * de[k];
    // a negative weight (the mispredict overcount of 0x68) may outweigh a quiet tick; a tick never refunds
    return s > 0 ? (uint32_t)(s >> 8) : 0u;
}

static void lcost_base(struct lcost *c, uint32_t ent, uint32_t mon, const uint32_t *tot, const uint32_t *mev)
{
    uint32_t k;

    c->ent = ent;
    c->mon = mon;
    for (k = 0; k < PMUC_N; k++) {
        c->ev[k] = tot[k];
        c->mev[k] = mev[k];
    }
}

void lcost_reset(struct lcost *c, uint32_t ent, uint32_t mon, const uint32_t *tot, const uint32_t *mev)
{
    lcost_base(c, ent, mon, tot, mev);
    // cum stays as lim_reset's base; the instruction second restarts as lim_reset's does
    c->sgi = 0;
    c->gip = 0;
}

uint32_t lim_ccum(struct lcost *c, const int32_t *w, uint32_t ent, uint32_t mon, const uint32_t *tot,
                  const uint32_t *mev, uint32_t commit)
{
    // as lim_tick: totals at a trap's entry less the monitor's completed traps, unsigned deltas survive a wrap
    uint32_t di = (ent - c->ent) - (mon - c->mon), de[PMUC_N], cum, k;

    for (k = 0; k < PMUC_N; k++)
        de[k] = (tot[k] - c->ev[k]) - (mev[k] - c->mev[k]);
    cum = c->cum + lim_cost(w, di, de);
    if (commit) {
        lcost_base(c, ent, mon, tot, mev);
        c->cum = cum;
        c->sgi += di;
    }
    return cum;
}

void lim_csecond(struct lcost *c)
{
    c->gip = c->sgi / 1000u;
    c->sgi = 0;
}
#endif

uint32_t lim_wake(struct lim *l, uint32_t isr, uint32_t hppir, uint64_t t)
{
    uint32_t id = hppir & IRQ_ID_MASK;

    // the re-check's own PPI 30 wake arms nothing and is no wake by the guest
    if ((isr & 1u) || id == GIC_ID_PWDT)
        return 0;
    l->wak++;
    // LILM times only a wake by the guest IRQ; a FIQ, another ID or a held 74 (disabled, so not reported)
    // leaves no stamp
    if (id == PL_GIC_ID_HYB_IRQ) {
        l->wkt = t;
        l->wkp = 1;
    }
    return l->rck;
}

uint32_t lim_rck_ok(uint32_t ctl, uint32_t us)
{
    // a watchdog-mode timer belongs to someone else and is never touched
    return us != 0 && !(ctl & LIM_WDT_MODE);
}

uint32_t lim_recheck(struct lim *l, uint32_t ent, uint32_t mon)
{
    // the tick base stays, so the next lim_tick still charges everything since the tick
    uint32_t g = (ent - l->ent) - (mon - l->mon);

    l->rck_n++;
#if NGH_LIM_TALIGN
    // the wait goes on after the slice until the guest's tick ack or a masked wake
    if (l->armed && !l->turbo && l->ipm && l->tas == LIM_TA_WAIT) {
        if (!l->tak && !l->tmk) {
            l->rst_n++;
            return 1;
        }
        lim_trel(l);
    }
#endif
    if (l->turbo || !l->armed || !l->ipm || l->bal - (int64_t)g >= 0)
        return 0;
    l->rst_n++;
    return 1;
}

_Static_assert(PL_GIC_ID_HYB_IRQ >> 5 == PL_GIC_ID_HYB_FIQ >> 5, "74 and 75 in one ISPENDR word");

uint32_t lim_wcls(uint32_t isr, uint32_t hppir, uint32_t pend, uint32_t held, uint32_t gpc, uint32_t gpsr,
                  uint32_t live)
{
    if (isr & 1u)
        return WK_TICK;
    // the pending bits, not HPPIR, so a takeable 74 behind a masked 75 still ends the stall
    if ((pend & GIC_ID_BIT(PL_GIC_ID_HYB_IRQ)) && !held && !live)
        return WK_IRQ;
    // the guest takes a FIQ with its F clear wherever fiq_defer forwards it; live is the gate of its mode
    if ((pend & GIC_ID_BIT(PL_GIC_ID_HYB_FIQ)) && !(gpsr & PSR_F) && !fiq_defer(gpc, gpsr, live))
        return WK_FIQ;
    if ((hppir & IRQ_ID_MASK) == GIC_ID_PWDT)
        return WK_RCK;
    if (pend & (GIC_ID_BIT(PL_GIC_ID_HYB_IRQ) | GIC_ID_BIT(PL_GIC_ID_HYB_FIQ)))
        return WK_MASK;
    return WK_NONE;
}

uint32_t lim_wexit(const struct lim *l, uint32_t cls)
{
    return !l->wloop || cls == WK_TICK || cls == WK_IRQ || cls == WK_FIQ || cls == WK_RCK;
}

uint32_t lim_slice(struct lim *l, uint32_t cls)
{
    // a variable, so the NGH_LIM_RCK_US=0 build does not see an always-true compare (-Wtype-limits)
    uint32_t full = LIM_RCK_US;

    if (!l->wloop)
        return full;
    // the per-tick budget pays full slices to taken interrupts only; the rest re-check after the ISR
    if ((cls == WK_IRQ || cls == WK_FIQ) && l->slb >= full) {
        l->slb -= full;
        return full;
    }
    l->slm++;
    return LIM_RCK_MIN_US;
}

void lim_wdone(struct lim *l, uint32_t cls, uint32_t seen, uint32_t hold_us)
{
    seen |= 1u << cls;
#if NGH_LIM_TALIGN
    // an interrupt pending that the guest cannot take now; it must run before it can take its tick
    if (l->tas == LIM_TA_WAIT && (seen & 1u << WK_MASK))
        l->tmk = 1;
#endif
    if (cls == WK_IRQ)
        l->wki++;
    if (cls == WK_FIQ)
        l->wkf++;
    if (seen & 1u << WK_MASK)
        l->wkm++;
    if (seen & 1u << WK_NONE)
        l->wkn++;
    l->hld += hold_us;
}

void lim_fmask(struct lim *l, uint32_t gpsr, uint32_t pend)
{
    if (!(gpsr & PSR_F))
        return;
    l->fms++;
    if (pend & GIC_ID_BIT(PL_GIC_ID_HYB_FIQ))
        l->fmp++;
}

void lim_fleak(struct lim *l, uint32_t rc, uint32_t cpsr, uint32_t gpsr)
{
    // a FIQ deferred inside irq_run left F set, and the IRQ forward carries it into the guest's handler
    if (rc && (cpsr & PSR_F) && !(gpsr & PSR_F))
        l->flk++;
}

void fiq_carry(struct fiqd *d, uint32_t cpsr, uint32_t gpsr)
{
    // a FIQ deferred in the trap window left F set, and the forward carries it into the guest handler
    if ((cpsr & PSR_F) && !(gpsr & PSR_F))
        d->carry++;
}

uint32_t lim_pk16(uint32_t hi, uint32_t lo)
{
    return (hi & 0xFFFFu) << 16 | (lo & 0xFFFFu);
}

void lim_second(struct lim *l)
{
    l->gip = l->sgi / 1000u;
    l->stl = l->sst;
    l->sgi = 0;
    l->sst = 0;
#if NGH_LIM_TALIGN
    l->tans = l->tan;
    l->tars = l->tar;
    l->tahs = l->tah;
    l->tan = 0;
    l->tar = 0;
    l->tah = 0;
#if NGH_LIM_PRE_US
    l->tprs = l->tpr;
    l->tpr = 0;
#endif
#endif
}

static int lim_pm_ok(uint32_t r)
{
    return r >= LIM_PM_LO && r <= LIM_PM_HI;
}

uint32_t lim_pick(uint32_t r08, uint32_t r68, uint32_t scu)
{
    // a CPU1 field other than 00 (dormant or off) could turn the stall wfi into a power-down
    if (LIM_SCU_CPU1(scu))
        return 0;
    if (lim_pm_ok(r08))
        return LIM_EV_INST;
    if (lim_pm_ok(r68))
        return LIM_EV_RENAME;
    return 0;
}
