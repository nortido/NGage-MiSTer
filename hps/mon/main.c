// SPDX-License-Identifier: GPL-3.0-or-later
#include <stddef.h>
#include <stdint.h>
#include "../common/platform.h"
#include "../common/status.h"
#include "../common/evlog.h"
#include "guest_census.h"
#include "cp15.h"
#include "patch.h"
#include "ldst.h"
#include "pamap.h"
#include "shadow.h"
#include "nor.h"
#include "audio.h"
#include "rumble.h"
#include "light.h"
#include "pmuc.h"

#include "trap.c"
#include "mmu.c"
#include "irq.c"
#include "dma.c"
#include "log.c"
#include "fwd.c"

#if NGH_LIM_COST
#if NGH_LIM_XHOLD
#error "the exact hold charges instructions, not the cost"
#endif
#endif

#define M_USR   0x10u
#define M_FIQ   0x11u
#define M_IRQ   0x12u
#define M_SVC   0x13u
#define M_ABT   0x17u
#define M_UND   0x1Bu
#define M_SYS   0x1Fu
#define PSR_T   0x20u
#define CTRL_C  0x4u
#define GUEST_CPSR 0xD3u
#define FILL_REPEAT_MAX 8u
#define FILL_RESERVE 64u
#define EXIT_CAP_REGS 0xE11C0001u
#define EXIT_CAP_THR 0xE11C0002u
// the current thread, the running exec's requester (a sentinel when idle), the Supervisor (owned by the kernel
// process)
#define EXIT_CUR_THREAD 0x80006740u
#define EXIT_REQ_THREAD 0x800003E0u
#define EXIT_SUP_THREAD 0x800066A8u
#define GICD_ISEN(id)   DEV(PL_GICD_BASE + 0x100u + ((id) >> 5) * 4u)
#define GICD_ICEN(id)   DEV(PL_GICD_BASE + 0x180u + ((id) >> 5) * 4u)
#define GICD_ICPD(id)   DEV(PL_GICD_BASE + 0x280u + ((id) >> 5) * 4u)
#define GICD_ISPD(id)   DEV(PL_GICD_BASE + 0x200u + ((id) >> 5) * 4u)
#define GICD_PRI8(id)   DEV(PL_GICD_BASE + 0x400u + (id))
#define GICD_TGT8(id)   DEV(PL_GICD_BASE + 0x800u + (id))
#define GIC_BIT(id)     (1u << ((id) & 31u))
#define GICC_CTLR       DEV(PL_GICC_BASE + 0x00u)
#define GICC_PMR        DEV(PL_GICC_BASE + 0x04u)
#define GICC_IAR        DEV(PL_GICC_BASE + 0x0Cu)
#define GICC_EOIR       DEV(PL_GICC_BASE + 0x10u)
#define GICC_HPPIR      DEV(PL_GICC_BASE + 0x18u)
#define GIC_PRI         0x80u
#define GIC_PMR         0xF8u
// an expired re-check one-shot waits behind a pending 74 or tick 29, so the guest enters its ISR first
#define GIC_PRI_RCK     0x88u
_Static_assert(GIC_PRI < GIC_PRI_RCK && GIC_PRI_RCK < GIC_PMR, "PPI 30 below 74 and 29, above the mask");
#define LIM_PRI_RCK     (NGH_LIM_WLOOP ? GIC_PRI_RCK : GIC_PRI)
#define GIC_CPU1        0x02u
#define PTM_LOAD        DEV(0xFFFEC600u)
#define PTM_CTRL        DEV(0xFFFEC608u)
#define PTM_ISR         DEV(0xFFFEC60Cu)
// PERIPHCLK 200 MHz (global timer 124.7e6 counts in 0.62 s)
#define PTM_1KHZ        199999u
#define PTM_RUN         0x7u
// core 1's private watchdog in timer mode, the re-check one-shot
#define WDT_LOAD        DEV(0xFFFEC620u)
#define WDT_CTRL        DEV(0xFFFEC628u)
#define WDT_ISR         DEV(0xFFFEC62Cu)
// SCU CPU power status, read only: CPU1 in [9:8]
#define LIM_SCU_PWR     0xFFFEC008u
#define BRGMODRST       RST(0xFFD0501Cu)
#define FS_DEBUG        0x02u
#define WIN_VA_SPAN     (HYB_WIN_REGS + SEC)
// an arm's memory policy when the launcher asks for none (the fork always writes 0)
#ifndef NGH_MEMCFG_DEF
#define NGH_MEMCFG_DEF  0u
#endif
// inner write-back only until the limiter arms, write-through for the rest of the guest boot
#ifndef NGH_BOOT_WB
#define NGH_BOOT_WB     0
#endif
_Static_assert(!NGH_PLAY_WB || NGH_BOOT_WB, "NGH_PLAY_WB needs NGH_BOOT_WB");
_Static_assert(!NGH_WX_OSD || NGH_PLAY_WB, "NGH_WX_OSD needs NGH_PLAY_WB");
// an evidence arm keeps the guest running past the gproc Die; the fork never sees the exit
#ifndef NGH_XC_HOLD
#define NGH_XC_HOLD     0
#endif
// the evidence arm's exit trace; the card monitor carries its rows, zero, and none of its reads
#ifndef NGH_XC_TRACE
#define NGH_XC_TRACE    0
#endif
#ifndef NGH_MK_CAP
#define NGH_MK_CAP      0
#endif
// the audio-wait probe's ring records, an evidence arm only
#ifndef NGH_AW_PROBE
#define NGH_AW_PROBE    0
#endif
#if NGH_AW_PROBE
#include "awp.c"
#endif

_Static_assert(PL_CDRV_BASE >= ALIAS_LO && PL_CDRV_BASE + PL_CDRV_SIZE <= ALIAS_HI, "C: shadow in the monitor alias");
_Static_assert(PL_CDRV_SIZE == NOR_SIZE, "C: shadow size");
_Static_assert(PL_AUDIO_BASE >= ALIAS_LO && PL_AUDIO_BASE + PL_AUDIO_SIZE <= ALIAS_HI, "audio ring in the monitor alias");
_Static_assert((PL_AUDIO_BASE & (SEC - 1u)) == 0 && PL_AUDIO_SIZE <= SEC, "audio ring is one section");

enum {
    FK_INTERNAL = 0x101, FK_REENTRY, FK_CP15, FK_SHADOW, FK_DABT, FK_PABT, FK_EMUL, FK_UNSUP,
    FK_IRQ, FK_FILL_LOOP, FK_PATCH, FK_PROFILE, FK_FETCH, FK_WINDOW, FK_MEMCFG, FK_L2
};
_Static_assert(L2_ABT_FK == FK_L2, "L2 abort fatal kind");
#ifdef SH_DROP_VERIFY
_Static_assert(DROP_VERIFY_FK == FK_SHADOW, "drop verify fatal kind");
#endif

struct frame {
    uint32_t spsr;
    uint32_t pad;
    uint32_t r[13];
    uint32_t lr;
};

extern void bank_get(uint32_t mode, uint32_t *r8_r14);
extern void bank_put(uint32_t mode, const uint32_t *r8_r14);
extern uint32_t bank_spsr(uint32_t mode);
extern void bank_spsr_put(uint32_t mode, uint32_t v);
extern void __attribute__((noreturn)) guest_enter(uint32_t pc, uint32_t cpsr);

static struct frame *g_f;
static uint32_t g_emode;
static struct trap_live g_live;
static struct trap_gp g_gp;
static struct xc g_xc;
#if NGH_XC_TRACE
static struct xc_tk g_xtk;
#endif
static uint32_t g_memcfg;
static int g_test;
static int g_cplog;
static uint32_t g_hb, g_trap[NGST_NTRAP];
static int g_cdrv;
static uint32_t g_bad;
static uint32_t g_rent;
static uint32_t g_rep_pc, g_rep_va, g_rep_n;
static uint32_t g_pc;
static uint32_t g_emx;
static uint64_t g_t0;
static uint64_t g_tb, g_tdt;
static struct fwd g_fwd;
static struct {
    uint32_t saved;
    uint32_t en;
    uint32_t pri;
    uint32_t tgt;
} g_g74;
static struct gic_fiq g_g75;
static struct aud g_aud;
static uint32_t g_fiqf, g_fiqd;
#if NGH_LIM_XHOLD
// the abort trap's entry count for the kick snapshot, valid only while that trap runs
static uint32_t g_dent, g_dok;
#endif
static struct {
    uint32_t held;
    uint32_t n;
    uint32_t max;
    uint64_t t0;
} g_irqh;

static inline uint32_t rd8(uint32_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uint32_t a, uint32_t v) { *(volatile uint8_t *)a = (uint8_t)v; }
static inline uint32_t cpsr_rd(void) { uint32_t v; __asm__ volatile("mrs %0, cpsr" : "=r"(v) : : "memory"); return v; }

static uint32_t win_rd(void *c, uint32_t off)
{
    (void)c;
    return rd32(WIN_VA + off);
}

static void win_wr(void *c, uint32_t off, uint32_t v)
{
    (void)c;
    wr32(WIN_VA + off, v);
}

static const struct fwd_io g_fio = { 0, win_rd, win_wr };

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}

static uint32_t bank_mode(uint32_t m)
{
    return m == M_USR ? M_SYS : m;
}

static uint32_t scratch_sp(void)
{
    if (g_emode == M_UND)
        return MRC15(0, 13, 0, 4);
    if (g_emode == M_IRQ)
        return MRC15(0, 13, 0, 2);
    return MRC15(0, 13, 0, 3);
}

static void scratch_sp_set(uint32_t v)
{
    if (g_emode == M_UND)
        MCR15(0, 13, 0, 4, v);
    else if (g_emode == M_IRQ)
        MCR15(0, 13, 0, 2, v);
    else
        MCR15(0, 13, 0, 3, v);
}

static void __attribute__((noinline)) snap_fill(void)
{
    static const uint32_t bm[6] = { M_SYS, M_SVC, M_ABT, M_UND, M_IRQ, M_FIQ };
    volatile struct ngst_fatal *fr = &ST->fatal_rec;
    uint32_t i, r[7];

    fr->mon_pc = g_f ? g_f->lr : 0;
    for (i = 0; i < 13; i++)
        fr->r[i] = g_f ? g_f->r[i] : 0;
    for (i = 0; i < 6; i++) {
        bank_get(bm[i], r);
        fr->sp[i] = r[5];
        fr->lr[i] = r[6];
        if (i)
            fr->spsr[i - 1] = bank_spsr(bm[i]);
        if (g_f && bm[i] == g_emode) {
            fr->sp[i] = scratch_sp();
            fr->lr[i] = g_f->lr;
            fr->spsr[i - 1] = g_f->spsr;
        }
    }
    fr->cpsr = g_f ? g_f->spsr : 0;
    for (i = 0; i < NGST_NCP15; i++)
        fr->cp15[i] = 0;
    fr->cp15[NGST_CP15_SCTLR] = g_cp.control;
    fr->cp15[NGST_CP15_TTBR0] = g_cp.ttbr;
    fr->cp15[NGST_CP15_DACR] = g_cp.dacr;
    fr->cp15[NGST_CP15_CONTEXTIDR] = g_cp.ctxid;
    fr->cp15[8] = g_cp.fsr;
    fr->cp15[9] = g_cp.far;
    fr->dfsr = MRC15(0, 5, 0, 0);
    fr->dfar = MRC15(0, 6, 0, 0);
    fr->ifsr = MRC15(0, 5, 0, 1);
    fr->ifar = MRC15(0, 6, 0, 2);
}

static void __attribute__((noreturn)) fatal(uint32_t kind, uint32_t a, uint32_t b)
{
    snap_fill();
    ST->fatal_rec.kind = kind;
    prow(PR_FA, a);
    prow(PR_FB, b);
    ST->fatal = 1;
    park(NGST_PARK_FATAL);
}

static uint32_t gr_get(void *c, unsigned mode, unsigned n)
{
    uint32_t r[7];

    (void)c;
    if (n < 8 || (n < 13 && mode != M_FIQ))
        return g_f->r[n];
    if (n > 14)
        return 0;
    if (n >= 13 && bank_mode(mode) == g_emode) {
        if (n == 13)
            return scratch_sp();
        g_bad = 1;
        return g_f->lr;
    }
    bank_get(bank_mode(mode), r);
    return r[n - 8];
}

static void gr_set(void *c, unsigned mode, unsigned n, uint32_t v)
{
    uint32_t r[7];

    (void)c;
    if (n < 8 || (n < 13 && mode != M_FIQ)) {
        g_f->r[n] = v;
        return;
    }
    if (n > 14) {
        g_bad = 2;
        return;
    }
    if (n >= 13 && bank_mode(mode) == g_emode) {
        if (n == 13)
            scratch_sp_set(v);
        else
            g_bad = 3;
        return;
    }
    bank_get(bank_mode(mode), r);
    r[n - 8] = v;
    bank_put(bank_mode(mode), r);
}

static uint32_t gr_lr(uint32_t gm)
{
    // the entry exception overwrote the LR of the monitor's own bank: 0 then, and no g_bad for the caller's emulation
    uint32_t b = g_bad, v;

    g_bad = 0;
    v = gr_get(0, gm, 14);
    if (g_bad)
        v = 0;
    g_bad = b;
    return v;
}

static void gic_apply(const struct gic_wr *w, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (w[i].byte)
            wr8(DEV(w[i].pa), w[i].val);
        else
            wr32(DEV(w[i].pa), w[i].val);
        dsb();
    }
}

static void irq_hw_park(void)
{
    struct gic_wr w[GIC_FIQ_NWR];

    if (!g_g74.saved)
        return;
    wr32(PTM_CTRL, 0);
    wr32(PTM_ISR, 1);
    wr32(GICD_ICEN(PL_GIC_ID_PTIMER), GIC_BIT(PL_GIC_ID_PTIMER));
    if (g_lim.rck) {
        wr32(WDT_CTRL, 0);
        wr32(WDT_ISR, 1);
        wr32(GICD_ICEN(GIC_ID_PWDT), GIC_BIT(GIC_ID_PWDT));
    }
    wr32(g_g74.en ? GICD_ISEN(PL_GIC_ID_HYB_IRQ) : GICD_ICEN(PL_GIC_ID_HYB_IRQ), GIC_BIT(PL_GIC_ID_HYB_IRQ));
    wr8(GICD_PRI8(PL_GIC_ID_HYB_IRQ), g_g74.pri);
    wr8(GICD_TGT8(PL_GIC_ID_HYB_IRQ), g_g74.tgt);
    dsb();
    gic_apply(w, gic_fiq_restore(&g_g75, w));
    g_g74.saved = 0;
}

static void irq_hw_init(void)
{
    struct gic_wr w[GIC_FIQ_NWR];
    uint32_t id = PL_GIC_ID_HYB_FIQ;

    if (!g_g74.saved) {
        g_g75.dctl = rd32(DEV(GIC_PA_DCTLR));
        g_g75.cctl = rd32(DEV(GIC_PA_CCTLR));
        g_g75.gr0 = rd32(DEV(GIC_PA_IGRP(0)));
        g_g75.gr2 = rd32(DEV(GIC_PA_IGRP(id)));
        g_g75.cf4 = rd32(DEV(GIC_PA_ICFG(id)));
        g_g75.en = rd32(DEV(GIC_PA_ISEN(id))) >> (id & 31u) & 1u;
        g_g75.pend = rd32(DEV(GIC_PA_ISPD(id))) >> (id & 31u) & 1u;
        g_g75.pri = rd8(DEV(GIC_PA_PRI(id)));
        g_g75.tgt = rd8(DEV(GIC_PA_TGT(id)));
        g_g74.tgt = rd8(GICD_TGT8(PL_GIC_ID_HYB_IRQ));
        g_g74.pri = rd8(GICD_PRI8(PL_GIC_ID_HYB_IRQ));
        g_g74.en = rd32(GICD_ISEN(PL_GIC_ID_HYB_IRQ)) & GIC_BIT(PL_GIC_ID_HYB_IRQ);
        g_g74.saved = 1;
        prow(PR_G74S, g_g74.tgt | g_g74.pri << 8 | (g_g74.en ? 1u : 0u) << 16);
    }
    wr32(PTM_CTRL, 0);
    wr32(PTM_ISR, 1);
    // one read per boot; no WDT or ID-30 write unless the watchdog is in timer mode and the re-check is on
    g_lim.wdt0 = rd32(WDT_CTRL);
    g_lim.rck = lim_rck_ok(g_lim.wdt0, LIM_RCK_US);
    g_lim.wloop = NGH_LIM_WLOOP;
    if (g_lim.rck) {
        wr32(WDT_CTRL, 0);
        wr32(WDT_ISR, 1);
        wr8(GICD_PRI8(GIC_ID_PWDT), LIM_PRI_RCK);
        // SL4r A1: Linux clears the PPI enables at CPU online but not a pending edge a left one-shot latched
        wr32(GICD_ICPD(GIC_ID_PWDT), GIC_BIT(GIC_ID_PWDT));
        wr32(GICD_ISEN(GIC_ID_PWDT), GIC_BIT(GIC_ID_PWDT));
    }
    wr8(GICD_PRI8(PL_GIC_ID_PTIMER), GIC_PRI);
    wr32(GICD_ISEN(PL_GIC_ID_PTIMER), GIC_BIT(PL_GIC_ID_PTIMER));
    wr8(GICD_TGT8(PL_GIC_ID_HYB_IRQ), GIC_CPU1);
    wr8(GICD_PRI8(PL_GIC_ID_HYB_IRQ), GIC_PRI);
    wr32(GICD_ISEN(PL_GIC_ID_HYB_IRQ), GIC_BIT(PL_GIC_ID_HYB_IRQ));
    wr32(GICC_PMR, GIC_PMR);
    gic_apply(w, gic_fiq_cfg(&g_g75, w));
    wr32(PTM_LOAD, PTM_1KHZ);
    wr32(PTM_CTRL, PTM_RUN);
    dsb();
}

static void dma_flush(void)
{
    dsb();
    dcache_clean_inv_all();
    // the channel may be inbound
    fl_ms(l2_ci());
    l2_chk(7u);
    publish();
}

// the kick's exact ranges, the whole L1 only for a channel with no known range
static void dma_mt_kick(void)
{
    uint32_t c0 = pmu_cyc();

    dsb();
    if (dma_kick_mt(&g_dma, &g_dc, &g_sh.pam) == DM_ALL)
        dma_flush();
    else
        publish();
    g_dma.dmoc += pmu_cyc() - c0;
}

static int dma_wx_in(uint32_t m)
{
    uint32_t k, pa, len, step;
    struct pam_res r;

    for (k = 0; k < DMA_NCH; k++) {
        if (!(m & (1u << k)) || g_dma.snap[k].kind != DK_IN)
            continue;
        pa = g_dma.snap[k].pa;
        len = g_dma.snap[k].len < DMA_RANGE_MAX ? g_dma.snap[k].len : DMA_RANGE_MAX;
        for (; len; pa += step, len -= step) {
            step = 0x1000u - (pa & 0xFFFu);
            if (step > len)
                step = len;
            r = pamap_page(pa, 0, &g_sh.pam);
            if (r.kind == PAM_RAM && sh_wx_exec_range(&g_sh, r.host_pa, step))
                return 1;
        }
    }
    return 0;
}

static void dma_mt_in(void)
{
    uint32_t c0 = pmu_cyc(), m = g_dma.done_in;

    dsb();
    dma_in_post(&g_dma, &g_dc, &g_sh.pam);
    // a card-in write over an exec page leaves the I side stale
    if (sh_wx_on(&g_sh) && dma_wx_in(m))
        imb_all();
    publish();
    g_dma.dmoc += pmu_cyc() - c0;
}

static void dma_mt_cancel(uint32_t chans)
{
    uint32_t c0 = pmu_cyc();

    dsb();
    dma_cpost(&g_dma, &g_dc, &g_sh.pam, chans);
    publish();
    g_dma.dmoc += pmu_cyc() - c0;
}

#ifdef NGH_SECCHK
#define SCHK_HDR    ((volatile struct schk_hdr *)VA_OF(PL_SCHK_BASE))
#define SCHK_REC    ((volatile struct schk_rec *)(VA_OF(PL_SCHK_BASE) + sizeof(struct schk_hdr)))

static void schk_init(void)
{
    SCHK_HDR->magic = 0;
    dsb();
    SCHK_HDR->ver = SCHK_VER;
    SCHK_HDR->n = 0;
    SCHK_HDR->ovf = 0;
    dsb();
    SCHK_HDR->magic = SCHK_MAGIC;
    publish();
}

// out of line so a test finds it in this build and not in the default one
static void __attribute__((noinline)) schk_put(uint32_t pos, uint32_t info, uint32_t dst, uint32_t sum)
{
    volatile struct schk_rec *r;

    if (g_slog >= SCHK_MAX_REC) {
        SCHK_HDR->ovf = ++g_slov;
        return;
    }
    r = &SCHK_REC[g_slog++];
    r->pos = pos;
    r->info = info;
    r->dst = dst;
    r->sum = sum;
}

static void schk_pub(void)
{
    dsb();
    SCHK_HDR->n = g_slog;
    publish();
}

static int schk_sum(uint32_t pa, uint32_t n, uint32_t *sum)
{
    struct pam_res a = pamap_page(pa, 0, &g_sh.pam), b = pamap_page(pa + n - 1u, 0, &g_sh.pam);
    uint32_t s, va;

    if (a.kind != PAM_RAM || b.kind != PAM_RAM || b.host_pa - a.host_pa != n - 1u || !alias_ok(a.host_pa) ||
        !alias_ok(b.host_pa))
        return 0;
    s = VA_OF(a.host_pa);
    // a card-out source gets only a clean and guest stores bypass this alias, so its lines are dropped first
    for (va = s & ~31u; va < s + n; va += 32u)
        MCR15(0, 7, 14, 1, va);
    dsb();
    alias_l2(s, n);
    *sum = schk_fnv((const void *)s, n);
    return 1;
}

// one record per dma_piece, read after the maintenance
static void schk_xfer(uint32_t kind, uint32_t chans)
{
    uint32_t c0 = pmu_cyc(), k, off, sum;
    const struct dma_snap *s;
    struct dma_piece p;

    for (k = 0; k < DMA_NCH; k++) {
        if (!(chans & (1u << k)))
            continue;
        s = &g_dma.snap[k];
        if (s->kind != (kind == SCHK_R ? DK_IN : DK_OUT))
            continue;
        for (off = 0; dma_piece(s, off, &p); off += p.n + p.pad)
            if (schk_sum(p.pa, p.n, &sum))
                schk_put(p.pos, schk_info(kind, p.n + p.pad), p.pa, dma_fnv_pad(sum, p.pad));
        schk_pub();
    }
    g_sccy += pmu_cyc() - c0;
}

static void schk_cancel(uint32_t k)
{
    const struct dma_snap *s = &g_dma.snap[k];

    schk_put(s->pos, schk_info(SCHK_C, s->len), s->pa, 0);
    schk_pub();
}
#endif

static uint32_t dma_check(uint32_t status)
{
    uint32_t in = g_dma.out, fin = dma_done(&g_dma, status);

    if (!fin)
        return 0;
    dma_fin(&g_dma, fin, (uint32_t)gtime());
    if (fin & in)
        dma_mt_in();
#ifdef NGH_SECCHK
    schk_xfer(SCHK_R, fin & in);
#endif
    return fin;
}

#if NGH_GF_DIAG
static uint32_t g_gfev;
#endif
#if NGH_PMU_FRAME
static struct pmf g_pmf;

#if NGH_LIM_FRAME_US
// the entry snapshot less the completed traps' sums, as lim_tick takes g_ient: the open one is in no sum yet
static void pmf_put(void)
{
    struct pmf_rec r[2];

    pmf_frame(r, &g_pmf, g_mcyc, g_lwcy, g_minst, g_pmc.mon, g_gf.frames);
    ev_put(EVK_PMF, r[0].flags, r[0].pc, r[0].a, r[0].b, r[0].c);
    ev_put(EVK_PMF, r[1].flags, r[1].pc, r[1].a, r[1].b, r[1].c);
}
#endif
#endif

static void gf_refresh(void)
{
#if NGH_GF_FULL
    uint32_t bytes = g_dma.lcd_words * 4u, va, fs;
#else
    uint32_t bytes = g_dma.lcd_words * 4u, va, st, i;
#endif
    struct pam_res a, b;
    const volatile uint32_t *p;
#if NGH_GF_DIAG && !NGH_GF_FULL
    uint32_t fs;
#endif
#if NGH_GF_DIAG
    uint32_t cyc, fr = g_gf.frames;
#endif

    if (!bytes || g_dma.lcd_words > 0x40000u)
        return;
    a = pamap_page(g_dma.lcd_src, 0, &g_sh.pam);
    b = pamap_page(g_dma.lcd_src + bytes - 1u, 0, &g_sh.pam);
    if (a.kind != PAM_RAM || b.kind != PAM_RAM || b.host_pa - a.host_pa != bytes - 1u || (a.host_pa & 3u) ||
        !alias_ok(a.host_pa) || !alias_ok(b.host_pa))
        return;
    va = VA_OF(a.host_pa);
#if NGH_GF_FULL || NGH_GF_DIAG
    p = (const volatile uint32_t *)VA_OF(a.host_pa);
#if NGH_GF_DIAG
    cyc = pmu_cyc();
#endif
    gf_fresh(&g_dc, va, bytes);
    fs = gf_sigf(p, g_dma.lcd_words);
#if NGH_GF_DIAG
    cyc = pmu_cyc() - cyc;
#endif
#endif
#if NGH_GF_FULL
#if NGH_LIM_FRAME_US
#if NGH_PMU_FRAME
    if (gf_lcd(&g_gf, fs)) {
        g_lim.fnew++;
#if NGH_LIM_XHOLD
        lim_xkick(&g_lim, g_dok, g_dent, g_minst);
#endif
        pmf_put();
    }
#else
#if NGH_LIM_XHOLD
    if (gf_lcd(&g_gf, fs)) {
        g_lim.fnew++;
        lim_xkick(&g_lim, g_dok, g_dent, g_minst);
    }
#else
    if (gf_lcd(&g_gf, fs))
        g_lim.fnew++;
#endif
#endif
#else
    (void)gf_lcd(&g_gf, fs);
#endif
#else
    // cache_out only cleans and guest stores bypass this alias, so the sampeld lines are dropped first
    for (i = 0, st = gf_step(g_dma.lcd_words) * 4u; i < GF_NSAMP && i < g_dma.lcd_words; i++)
        MCR15(0, 7, 14, 1, (va + i * st) & ~31u);
    dsb();
    if (g_dc.flags & CACHE_OUTER)
        cache_rd_step(&g_dc, va, st, i);
    p = (const volatile uint32_t *)VA_OF(a.host_pa);
#if NGH_LIM_FRAME_US
#if NGH_PMU_FRAME
    if (gf_lcd(&g_gf, gf_sig(p, g_dma.lcd_words))) {
        g_lim.fnew++;
#if NGH_LIM_XHOLD
        lim_xkick(&g_lim, g_dok, g_dent, g_minst);
#endif
        pmf_put();
    }
#else
#if NGH_LIM_XHOLD
    if (gf_lcd(&g_gf, gf_sig(p, g_dma.lcd_words))) {
        g_lim.fnew++;
        lim_xkick(&g_lim, g_dok, g_dent, g_minst);
    }
#else
    if (gf_lcd(&g_gf, gf_sig(p, g_dma.lcd_words)))
        g_lim.fnew++;
#endif
#endif
#else
    (void)gf_lcd(&g_gf, gf_sig(p, g_dma.lcd_words));
#endif
#endif
#if NGH_GF_DIAG
    // a FULL build's sampled signature reads the lines gf_fresh just dropped, so it needs no maintenance of its own
    g_gfev = gf_alt(&g_gf, NGH_GF_FULL ? gf_sig(p, g_dma.lcd_words) : fs, g_gf.frames != fr, cyc);
#endif
    if (!g_lim.armed)
        lim_frame(&g_lim, lim_nonwhite(p, g_dma.lcd_words), g_dma.lcd_kicks);
}

#if NGH_AW_PROBE
static struct awp g_awp;
static uint32_t g_awp_gm;
#endif
#if NGH_AW_PROBE || NGH_TICK_CUR || NGH_LIM_XHOLD
static int aud_va2pa(void *c, uint32_t va, uint32_t *host);

// inside the IRQ and the MMIO trap, so the alias read recipe and no gm_mem fill
static int awp_iord(void *c, uint32_t va, uint32_t *w)
{
    uint32_t h;

    if ((va & 3u) || aud_va2pa(c, va, &h))
        return -1;
    alias_fresh(VA_OF(h));
    *w = rd32(VA_OF(h));
    return 0;
}

#if NGH_LIM_XHOLD
static void xh_cur(const struct frame *f)
{
    uint32_t w = 0;
    int bad = awp_iord(0, EXIT_CUR_THREAD, &w);

    lim_xcur(&g_lim, !bad, w, f->lr - 4u, f->spsr & 0x1Fu);
}
#endif
#endif

#if NGH_AW_PROBE
static void awp_ioput(void *c, uint32_t fl, uint32_t pc, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c;
    ev_put(EVK_AWP, fl, pc, a, b, d);
}

static uint32_t awp_iogp(void *c)
{
    (void)c;
    return g_gp.gproc;
}

static const struct awp_io g_awpio = { 0, awp_iord, awp_ioput, awp_iogp };
#endif

static uint32_t fwd_mem(uint32_t pa, unsigned size, int write, uint32_t *data)
{
    uint32_t chg, lk = g_dma.lcd_kicks, i;
    int dw = DW_NONE;

    if (write) {
        // this write clears dma_pend, so a finished channel is taken from STATUS before it goes out
        if (dma_pre(&g_dma, pa))
            g_dma.dcpr += dma_nbits(dma_check(win_rd(0, HYB_REG_OFF(HYB_R_STATUS))));
        dw = dma_write(&g_dma, pa, fwd_mask(*data, size));
#if NGH_LIM_TALIGN
#if NGH_LIM_PCARRY
        if (lim_tack(&g_lim, pa))
            lim_sack(&g_lim, gtime());
#else
        (void)lim_tack(&g_lim, pa);
#endif
#endif
    }
    // the FPGA reads and writes guest RAM in DDR behind core 1's L1
    if (dw == DW_KICK) {
        dma_mt_kick();
        for (i = 0; i < g_dma.nedge; i++)
            (void)sh_guard_add(&g_sh, g_dma.edge[i]);
        if (g_dma.lcd_kicks != lk)
            gf_refresh();
#ifdef NGH_SECCHK
        if (g_dma.snap[g_dma.last].kind == DK_OUT)
            schk_xfer(SCHK_W, 1u << g_dma.last);
#endif
        dma_stamp(&g_dma, (uint32_t)gtime());
    }
#ifdef NGH_SECCHK
    if (dw == DW_CANCEL)
        schk_cancel(g_dma.last);
#endif
    chg = fwd_access(&g_fwd, &g_fio, pa, size, write, data);
    // INTC reads check card channels only: LCD completions wait for the IRQ delivery, the tick or a +08 write
    if (write)
        dsb();
    else if ((g_dma.out | g_dma.wout) && dma_intc_read(pa))
        dma_check(win_rd(0, HYB_REG_OFF(HYB_R_STATUS)));
    g_fwdn++;
    ST->traps[NGST_TRAP_MMIO] = ++g_trap[NGST_TRAP_MMIO];
#if NGH_GF_DIAG
    ev_put(EVK_MMIO, (size == 1 ? EVF_SZ_B : size == 2 ? EVF_SZ_H : EVF_SZ_W) | (write ? EVF_WRITE : 0u),
           g_pc, pa, *data, g_gfev);
    g_gfev = 0;
#else
    ev_put(EVK_MMIO, (size == 1 ? EVF_SZ_B : size == 2 ? EVF_SZ_H : EVF_SZ_W) | (write ? EVF_WRITE : 0u),
           g_pc, pa, *data, 0);
#endif
#if NGH_AW_PROBE
    if (g_dma.lcd_kicks != lk)
        awp_kick(&g_awp, &g_awpio, g_pc);
    awp_mmio(&g_awp, &g_awpio, g_pc, g_awp_gm);
#endif
    if (chg)
        hw_drop_all(0);
    return 0;
}

static uint32_t cdrv_mem(uint32_t pa, unsigned size, int write, uint32_t *data)
{
    uint32_t base = VA_OF(PL_CDRV_BASE), gen = ST->cdrv_gen, a;
    struct nor_fx fx;
    int rc;

    g_nora++;
    ST->traps[NGST_TRAP_MMIO] = ++g_trap[NGST_TRAP_MMIO];
    // seqlock: a reader accepts a shadow copy only with the same even gen before and after it
    if (write) {
        gen = nor_gen_next(gen, 0, 0);
        ST->cdrv_gen = gen;
        dsb();
    }
    rc = nor_access(&g_nor, (uint16_t *)base, pa - NOR_PA_BASE, size, write, data, &fx);
    if (write) {
        if (fx.hi != fx.lo) {
            for (a = (base + fx.lo) & ~31u; a < base + fx.hi; a += 32u)
                MCR15(0, 7, 14, 1, a);
            dsb();
        }
        ST->cdrv_gen = nor_gen_next(gen, 1, fx.hi != fx.lo);
        dsb();
        if (!rc)
            ev_put(EVK_MMIO, (size == 1 ? EVF_SZ_B : size == 2 ? EVF_SZ_H : EVF_SZ_W) | EVF_WRITE | EVF_CDRV,
                   g_pc, pa, *data, 0);
    }
    if (rc) {
        g_bad = 8;
        return 0x5u;
    }
    return 0;
}

static uint32_t gm_mem(void *c, uint32_t va, unsigned size, int write, int user, uint32_t *data)
{
    uint32_t h, a, fsr = 0, pa = va;
    int32_t chk;
    int rc;

    g_emx = TEM_OT;
    if (va - MON_VA_BASE < MON_VA_SIZE) {
        g_bad = 4;
        return 0x5u;
    }
    if (g_cp.control & CTRL_M) {
        // c: the walk of the fill this emulation follows (dabt_run), else 0
        chk = sh_access_pa(&g_sh, c, va, write, user, &pa);
#ifdef SH_DROP_VERIFY
        {
            uint32_t pa2 = va;
            int32_t chk2 = sh_access_check(&g_sh, va, write, user);

            if (chk2 == 0 && sh_va2pa(&g_sh, va, &pa2) != SH_OK)
                chk2 = -1;
            if ((chk2 < 0 ? -1 : chk2) != (chk < 0 ? -1 : chk) || (chk == 0 && pa2 != pa))
                fatal(DROP_VERIFY_FK, 0xB5300000u | (((uint32_t)chk2 & 0xFFu) << 8) | ((uint32_t)chk & 0xFFu), va);
        }
#endif
        if (chk > 0)
            return (uint32_t)chk;
        if (chk < 0) {
            g_bad = 5;
            return 0x5u;
        }
    }
    if (!g_test && g_cdrv && nor_pa_in(pa))
        return cdrv_mem(pa, size, write, data);
    // a write to an array-mode ROM page is a flash command: never the DDR copy
    if (!g_test && pamap_page(pa, write, &g_sh.pam).kind == PAM_FWD) {
        g_emx = TEM_IO;
        return fwd_mem(pa, size, write, data);
    }
    if (!trap_host_of(&g_sh, va, &h)) {
        rc = sh_fill(&g_sh, va, &fsr);
        if (rc == SH_FAULT)
            return fsr;
        if (rc == SH_FWD) {
            g_open++;
            ST->traps[NGST_TRAP_MMIO] = ++g_trap[NGST_TRAP_MMIO];
            if (!write)
                *data = 0;
            return 0;
        }
        if ((rc != SH_OK && rc != SH_SUBPAGE) || !trap_host_of(&g_sh, va, &h)) {
            g_bad = 6;
            return 0x5u;
        }
        g_fills++;
    }
    if (!alias_ok(h)) {
        g_bad = 7;
        return 0x5u;
    }
    a = VA_OF(h);
    // the guest mapping may be non-cacheable while the alias is write-back: no line stays behind
    MCR15(0, 7, 14, 1, a & ~31u);
    dsb();
    alias_l2(a, 1u);
    if (size == 4) {
        if (write)
            wr32(a, *data);
        else
            *data = rd32(a);
    } else if (size == 2) {
        if (write)
            *(volatile uint16_t *)a = (uint16_t)*data;
        else
            *data = *(volatile uint16_t *)a;
    } else {
        if (write)
            *(volatile uint8_t *)a = (uint8_t)*data;
        else
            *data = *(volatile uint8_t *)a;
    }
    if (write) {
        MCR15(0, 7, 14, 1, a & ~31u);
        dsb();
        alias_l2(a, 1u);
        // the I side may hold the old bytes of an exec page
        if (sh_wx_on(&g_sh) && sh_wx_exec_range(&g_sh, h, size))
            imb_all();
    }
    g_emx = TEM_LS;
    return 0;
}

static int aud_va2pa(void *c, uint32_t va, uint32_t *host)
{
    uint32_t pa = va;
    struct pam_res r;

    (void)c;
    if ((g_cp.control & CTRL_M) && sh_va2pa(&g_sh, va, &pa) != SH_OK)
        return -1;
    r = pamap_page(pa, 0, &g_sh.pam);
    if (r.kind != PAM_RAM || !alias_ok(r.host_pa))
        return -1;
    *host = r.host_pa;
    return 0;
}

static void aud_copy(void *c, uint32_t off, uint32_t host, uint32_t n)
{
    uint32_t s = VA_OF(host), d = VA_OF(PL_AUDIO_BASE) + off, a;

    (void)c;
    // as in gm_mem: a non-cacheable guest buffer leaves no older line in the write-back alias
    for (a = s & ~31u; a < s + n; a += 32u)
        MCR15(0, 7, 14, 1, a);
    dsb();
    alias_l2(s, n);
    if (!((s | d | n) & 3u))
        for (a = 0; a < n; a += 4u)
            wr32(d + a, rd32(s + a));
    else
        for (a = 0; a < n; a++)
            wr8(d + a, rd8(s + a));
}

static void aud_publish(void *c)
{
    (void)c;
    publish();
}

static uint32_t aud_rd(void *c, uint32_t r)
{
    return win_rd(c, HYB_REG_OFF(r));
}

static void aud_wr(void *c, uint32_t r, uint32_t v)
{
    win_wr(c, HYB_REG_OFF(r), v);
}

static const struct aud_io g_aio = { 0, aud_va2pa, aud_copy, aud_publish, aud_rd, aud_wr };

static void aud_st(void)
{
    ST->aud_bytes = g_aud.bytes;
    ST->aud_chunks = g_aud.chunks;
    ST->aud_drops = g_aud.drops;
    ST->aud_skips = g_aud.skips;
    ST->aud_rate = g_aud.rate;
    ST->aud_ch = g_aud.ch;
    ST->aud_cfg = g_aud.cfg;
    ST->aud_trim = (uint32_t)g_aud.trim;
}

static void __attribute__((noinline)) aud_hw_park(uint32_t reason)
{
    // the FPGA is gone or the bridge aborts: no register access
    if (!g_aud.on || reason == NGST_PARK_FPGA_LOST || reason == NGST_PARK_WINDOW_ABORT ||
        !irq_bridge_ok(rd32(BRGMODRST)) || win_rd(0, HYB_REG_OFF(HYB_R_MAGIC)) != HYB_MAGIC_VALUE)
        return;
    aud_park(&g_aud, &g_aio);
}

static struct rumble g_rm;

// IRQ context, so no gm_mem and no sh_fill: a read-only walk and the alias read recipe
static int rm_rd(void *c, uint32_t va, uint32_t w[2])
{
    uint32_t h;

    if (aud_va2pa(c, va, &h))
        return -1;
    alias_fresh(VA_OF(h));
    w[0] = rd32(VA_OF(h));
    w[1] = rd32(VA_OF(h) + 4u);
    return 0;
}

static void rm_wr(void *c, uint32_t v)
{
    (void)c;
    win_wr(0, HYB_REG_OFF(HYB_R_RUMBLE), v);
}

static const struct rumble_io g_rio = { 0, rm_rd, rm_wr };

static void rm_pub(void)
{
    xrow(XR_RMBL, rumble_row(&g_rm));
    xrow(XR_VBGW, g_rm.vib);
}

static struct light g_lt;

// IRQ context, so the alias read recipe for one kernel word
static int lt_rd(void *c, uint32_t va, uint32_t *w)
{
    uint32_t h;

    if (aud_va2pa(c, va, &h))
        return -1;
    alias_fresh(VA_OF(h));
    *w = rd32(VA_OF(h));
    return 0;
}

static void lt_wr(void *c, uint32_t v)
{
    (void)c;
    win_wr(0, HYB_REG_OFF(HYB_R_LIGHT), v);
}

static const struct light_io g_lio = { 0, lt_rd, lt_wr };

static void lt_pub(void)
{
    xrow(XR_LGHT, light_lght(&g_lt));
    xrow(XR_LIDL, light_lidl(&g_lt));
}

static uint32_t guest_fetch(uint32_t pc, int thumb)
{
    uint32_t h, c0 = pmu_cyc(), w;

    if (!trap_host_of(&g_sh, pc, &h) || !alias_ok(h))
        fatal(FK_FETCH, pc, 0);
    if (!trap_rom_site(&g_sh.pam, h))
        alias_fresh(VA_OF(h));
    w = thumb ? *(volatile uint16_t *)VA_OF(h) : rd32(VA_OF(h));
    g_gfcy += pmu_cyc() - c0;
    g_gfcn++;
    return w;
}

static void irq_release(uint32_t gm, uint32_t pc)
{
    uint32_t us;

    if (trap_irq_gate(&g_live, gm, (int)g_irqh.held, 0) != TIG_UNMASK)
        return;
    us = (uint32_t)(gtime() - g_irqh.t0) / GT_PER_US;
    if (us > g_irqh.max)
        g_irqh.max = us;
    g_irqh.held = 0;
    wr32(GICD_ISEN(PL_GIC_ID_HYB_IRQ), GIC_BIT(PL_GIC_ID_HYB_IRQ));
    dsb();
    ev_put(EVK_IRQ, EVF_IRQ_HELD, pc, us, g_irqh.n, g_irqh.max);
}

static void enter(struct frame *f, uint32_t emode, uint32_t pcoff, uint32_t far)
{
    uint32_t gm = f->spsr & 0x1Fu, c0 = pmu_cyc();

    g_f = f;
    g_emode = emode;
    g_bad = 0;
    if (f->lr - MON_VA_BASE < PL_MON_SIZE)
        fatal(FK_INTERNAL, emode, f->lr);
    fwd_invalidate(&g_fwd);
    ST->heartbeat = ++g_hb;
    if (ST->park_req == NGST_PARK_REQUEST)
        park(NGST_PARK_REQUEST);
    if (trap_live_check(&g_live, emode, gm)) {
        g_rent++;
        prow(PR_RENT, g_rent);
        ev_put(EVK_REENTRY, emode == M_UND ? EVRE_UND : EVRE_ABT, f->lr - pcoff, far, 0, 0);
        fatal(FK_REENTRY, emode, gm);
    }
    irq_release(gm, f->lr - pcoff);
    g_ency += pmu_cyc() - c0;
    g_encn++;
}

static void abt_fill(uint32_t vec_va)
{
    struct sh_fill_stats st;
    uint64_t t0 = gtime();
    uint32_t us;

    sh_fill_all(&g_sh, FILL_RESERVE, &st);
    tlb_flush();
    us = (uint32_t)(gtime() - t0) / GT_PER_US;
    ev_put(EVK_ABT, EVABT_FILL | (st.stop ? EVF_FILL_STOP : 0u), vec_va, st.pages, g_sh.l2_used,
           (st.refused < 0xFFFu ? st.refused : 0xFFFu) << 20 | (us < 0xFFFFFu ? us : 0xFFFFFu));
}

// the dirty lines reach DDR while g_dma.wb still holds, imb drops the code of pages left in the W^X data
// state; guard_sync's loop runs the one drop-all that refills under the new policy
__attribute__((noinline)) static void boot_wt(void)
{
#if NGH_PLAY_WB
    // once per guest boot, guest_boot clears the state
    if (g_sh.wx_off)
        return;
#endif
    dcache_clean_inv_all();
    imb_all();
#if NGH_PLAY_WB
    g_sh.wx_off = 1;
#else
    g_sh.memcfg = g_memcfg & ~NGST_MC_INNER_WB;
    trap_sw_memcfg(&g_sw, g_sh.memcfg);
    g_dma.wb = (g_sh.memcfg & SH_MC_WB) != 0;
#endif
    sh_wx_reset(&g_sh);
    g_wxe = g_wxw = 0;
    real_dacr();
    g_sh.gdue = 1;
    g_bwsw = lim_min((g_bwsw >> 24) + 1u, 255u) << 24 | lim_min(g_lim.ticks, 0xFFFFFFu);
}

// the entries that mapped a joined page write-back go before the guest runs again
__attribute__((noinline)) static void guard_sync(void)
{
    if (NGH_BOOT_WB && !sh_wx_osd(&g_sh) && (g_sh.memcfg & NGST_MC_INNER_WB) && g_lim.armed && !g_live.abt && !g_live.und)
        boot_wt();
    while (g_sh.gdue) {
        g_sh.gdue = 0;
        hw_drop_all(0);
    }
}

static uint32_t forward(uint32_t fmode, uint32_t vec_off)
{
    // the prefetch-abort stub jumps here: a data abort raised on that path (mark site) needs the data vector
    fwd_pabt_t = g_vbase + vec_off;
    mmu_prefill(g_vbase + vec_off, fmode == g_emode ? scratch_sp() : gr_get(0, fmode, 13));
    // the guest handler stays in ABT mode, where a shadow miss would overwrite its LR_abt and SPSR_abt
    if (fmode == M_ABT) {
        // after a join too: the drop-all comes first, then the fill the handler runs on
        do {
            guard_sync();
            abt_fill(g_vbase + vec_off);
        } while (g_sh.gdue);
    }
    fiq_carry(&g_fqd, cpsr_rd(), g_f->spsr);
    trap_live_forward(&g_live, fmode);
    if (fmode == M_ABT)
        g_fwda++;
    else
        g_fwdu++;
    ST->forwards = ST->forwards + 1u;
    st_counters();
    return 1;
}

static uint32_t forward_und(uint32_t pc)
{
    uint32_t gm = g_f->spsr & 0x1Fu;

    ST->traps[NGST_TRAP_UNDEF] = ++g_trap[NGST_TRAP_UNDEF];
    ev_put(EVK_ABT, EVABT_U, pc, 0, 0, 0);
    xc_fwd_note(&g_xc, XC_FWD_UND, pc, gr_lr(gm), pc, 0, gm, g_lim.ticks);
    return forward(M_UND, 0x04u);
}

static uint32_t forward_abt(int pabt, uint32_t pc, uint32_t vfsr, uint32_t vfar)
{
    uint32_t gm = g_f->spsr & 0x1Fu;

    if (!pabt) {
        g_cp.fsr = vfsr & 0xFFu;
        g_cp.far = vfar;
    }
    ev_put(EVK_ABT, pabt ? EVABT_P : EVABT_D, pc, vfar, vfsr, 0);
    xc_fwd_note(&g_xc, pabt ? XC_FWD_PABT : XC_FWD_DABT, pc, gr_lr(gm), vfar, vfsr, gm, g_lim.ticks);
    return forward(M_ABT, trap_vec_abt(pabt));
}

static void sync_shadow_view(void)
{
    uint32_t m;

    sh_set_sctlr(&g_sh, g_cp.control);
    g_sh.g_ttb = g_cp.ttbr;
    // a live domain changed its Manager-ness: its entries are re-emitted before the guest runs again, a
    // drop-all only past SH_RE_MAX; with the guest MMU off the shadow is not in use; in the MMU-on trap g_ident
    // holds until hw_mmu_mode, so a refill or drop here would read the identity table (FK_WINDOW)
    m = sh_set_dacr(&g_sh, g_cp.dacr);
    if (m && sh_wx_on(&g_sh) && (g_cp.control & CTRL_M) && !g_ident && sh_reemit(&g_sh, m)) {
        hw_drop_all(0);
        g_sh.drop_dacr++;
    }
}

static void __attribute__((noreturn)) halt(uint32_t pc)
{
    prow(PR_HALT, pc);
    park(NGST_PARK_MILESTONE);
}

static void __attribute__((noreturn)) milestone(uint32_t m)
{
    prow(PR_TMIL, (uint32_t)(gtime() - g_t0));
    prow(PR_TOUT, win_rd(0, HYB_REG_OFF(HYB_R_TIMEOUTS)));
    prow(PR_MILE, m);
    park(NGST_PARK_MILESTONE);
}

static uint32_t mark_site(struct frame *f, uint32_t pc, int thumb, uint32_t slot);
static uint32_t und_ldm(struct frame *f, uint32_t pc, uint32_t word);

static uint32_t cp15_trap(struct frame *f, uint32_t pc, int thumb, int bkpt)
{
    uint32_t gm, word, orig = 0, rd, rdv, nzcv, old, fk = 0, fa = 0, val, rst;
    uint32_t c0 = pmu_cyc(), w0 = g_wfi_cyc, ck = TCK_N, d;
    int kind, td, passed;
    struct cp15_result r;

    g_rep_n = 0;
    gm = f->spsr & 0x1Fu;
    word = guest_fetch(pc, thumb);
    kind = trap_udf_kind(word, thumb, gm, g_test, &orig);
    if (kind == TU_MARK || kind == TU_EXIT) {
        if (bkpt)
            return mark_site(f, pc, thumb, orig);
        kind = TU_FWD;
    }
    if (kind == TU_HALT)
        halt(pc);
    // an UNDEF for the guest cannot leave from the abort stub (EXCCOUNT und 0 over 1G cycles)
    if (bkpt && kind == TU_FWD)
        fatal(FK_UNSUP, word, pc);
    if (kind == TU_FWD)
        return und_ldm(f, pc, word);

    ST->traps[NGST_TRAP_CP15] = ++g_trap[NGST_TRAP_CP15];
    rd = (orig >> 12) & 15u;
    rdv = rd == 15u ? pc + 8u : gr_get(0, gm, rd);
    nzcv = f->spsr & 0xF0000000u;
    passed = trap_cond_pass(orig >> 28, nzcv);
    old = g_cp.control;
    r = cp15_exec(&g_cp, orig, rdv, nzcv);
    sync_shadow_view();
#if NGH_AW_PROBE
    if (r.nact && r.act[0].kind == CP15_ACT_WFI)
        awp_idle(&g_awp, &g_awpio, pc);
#endif
    rst = g_sw.drop_rst;
    td = trap_dispatch(&r, &g_hw, &g_sw, &fk, &fa);
#if NGH_PMU_FRAME
    g_pmf.idle |= g_wfi_cyc != w0;
#endif
    if (g_sw.drop_rst != rst)
        g_drc[trap_drop_cause(orig, CP15_ACT_SHADOW_RESET)]++;
    if (g_win_va)
        fatal(FK_WINDOW, g_win_va, g_win_l1e);
    if (bkpt && td == TD_UNDEF)
        fatal(FK_UNSUP, orig, pc);
    if (td == TD_UNDEF)
        return forward_und(pc);
    if (td == TD_FATAL)
        fatal(FK_CP15, fk, fa);
    if ((old ^ g_cp.control) & CTRL_C) {
        dcache_clean_inv_all();
        // hw_mmu_mode swept already when M changed in the same write
        if (!((old ^ g_cp.control) & CTRL_M))
            fl_ms(l2_ci());
        l2_chk(6u);
    }
    val = rdv;
    if (r.kind == CP15_RES_RD) {
        gr_set(0, gm, rd, r.value);
        val = r.value;
    } else if (r.kind == CP15_RES_FLAGS) {
        f->spsr = (f->spsr & 0x0FFFFFFFu) | r.value;
        val = r.value;
    }
    if (g_bad)
        fatal(FK_EMUL, g_bad, orig);
    if (passed) {
        ck = trap_cp15_kind(orig, &r);
        g_ck[ck]++;
        if (g_cplog)
            ev_put(EVK_CP15, 0, pc, orig, val, gm);
    }
    if (passed && !g_test && (ST->mile_mask & NGST_MILE_MMU) && (g_cp.control & ~old & CTRL_M))
        milestone(NGST_MILE_MMU);
    // only completed CP15 ops: mark sites and forwarded UNDEFs would skew the cycles per CP15 trap
    d = pmu_cyc() - c0 - (g_wfi_cyc - w0);
    g_ccyc += d;
    if (ck == TCK_TL)
        g_cytl += d;
    else if (ck == TCK_DA)
        g_cyda += d;
    return 0;
}

#if NGH_PMU_EV
static void pmc_out(const uint32_t *e0)
{
    uint32_t e1[PMUC_N];

    pmu_evrd(e1);
    pmuc_trap(&g_pmc, e0, e1);
}
#endif

#if NGH_PMU_FRAME
// the alias read as in the audio-wait probe; a refused page is looked up again only for ROM or no map
static uint32_t pmf_iord(void *c, uint32_t va, uint32_t *w)
{
    uint32_t h, pa = va;

    if (!aud_va2pa(c, va, &h)) {
        alias_fresh(VA_OF(h));
        *w = rd32(VA_OF(h));
        return 0;
    }
    if ((g_cp.control & CTRL_M) && sh_va2pa(&g_sh, va, &pa) != SH_OK)
        return PMS_F_NOMAP;
    return pamap_page(pa, 0, &g_sh.pam).kind == PAM_ROM ? PMS_F_ROM : PMS_F_NOMAP;
}

static const struct pmf_io g_pmfio = { 0, pmf_iord };

// lim_tick returns 1 exactly when it leaves run nonzero, so run is the last tick's stall
static void pmf_tick(struct frame *f)
{
    struct pmf_rec r;

    pmf_sample(&r, &g_pmf, &g_pmfio, f->lr, f->spsr, g_ient - g_minst, g_lim.run != 0);
    ev_put(EVK_PMS, r.flags, r.pc, r.a, r.b, r.c);
#if NGH_TICK_CUR
    pmf_cur(&r, r.pc, awp_iord, 0);
#if NGH_LIM_XHOLD
    r.b = g_lim.xam;
    r.c = g_lim.xan;
#endif
    ev_put(EVK_PMF, r.flags, r.pc, r.a, r.b, r.c);
#endif
}

static void pmf_sec(void)
{
    struct pmf_rec r;

    pmf_part2(&r, &g_pmf, pmu_evs);
    ev_put(EVK_PMF, r.flags, r.pc, r.a, r.b, r.c);
}
#endif

#if NGH_PMU_CENSUS
static void pmc_boot(void)
{
    uint32_t e[PMUC_N];

    pmu_evrd(e);
    pmuc_reset(&g_pmc, e, pmu_ins(), g_minst);
    pmc_rows();
}
#endif

#if NGH_LIM_COST
static uint32_t lc_cum(uint32_t commit)
{
    // the counts at this trap's entry, as the flat limiter takes g_ient: the open trap is in neither monitor sum yet
    return lim_ccum(&g_lc, lim_cw, g_ient, g_minst, g_pce, g_pmc.mon, commit);
}

static void lc_boot(void)
{
    uint32_t e[PMUC_N];

    pmu_evrd(e);
    lcost_reset(&g_lc, pmu_ins(), g_minst, e, g_pmc.mon);
    lim_reset(&g_lim, g_lc.cum, 0u);
}
#endif

uint32_t trap_und_c(struct frame *f)
{
    uint32_t c0 = pmu_cyc(), i0 = pmu_ins(), rc;
    int thumb = (f->spsr & PSR_T) != 0;
#if NGH_PMU_EV
    uint32_t e0[PMUC_N];

    pmu_evrd(e0);
#endif
#if NGH_PMU_FRAME
    pmf_ent(&g_pmf, c0, i0, e0);
#endif

    enter(f, M_UND, thumb ? 2u : 4u, 0);
    rc = cp15_trap(f, f->lr - (thumb ? 2u : 4u), thumb, 0);
    guard_sync();
#if NGH_PMU_EV
    pmc_out(e0);
#endif
    g_mcyc += pmu_cyc() - c0;
    g_minst += pmu_ins() - i0;
    return rc;
}

static void ldst_prep(struct ldst_ops *ops, uint32_t pc)
{
    ops->ctx = 0;
    ops->align = (int)g_align;
    ops->get_reg = gr_get;
    ops->set_reg = gr_set;
    ops->mem = gm_mem;
    g_rep_n = 0;
    g_pc = pc;
    g_bad = 0;
}

static uint32_t emulate_end(struct frame *f, uint32_t pc, uint32_t insn, int st, const struct ldst_result *res)
{
    uint32_t gm = f->spsr & 0x1Fu;

    if (g_bad)
        fatal(FK_EMUL, g_bad, insn);
    if (st == LDST_FAULT)
        return forward_abt(0, pc, res->fsr, res->fault_va);
    if (st != LDST_DONE)
        fatal(FK_UNSUP, insn, pc);
    if (res->spsr_restore) {
        if (gm == M_USR || gm == M_SYS || gm == g_emode)
            fatal(FK_UNSUP, insn, pc);
        f->spsr = bank_spsr(gm);
    } else {
        f->spsr = (f->spsr & ~PSR_T) | (res->thumb ? PSR_T : 0u);
    }
    f->lr = res->next_pc;
    return 0;
}

static uint32_t emulate(struct frame *f, uint32_t pc, int thumb, struct sh_hand *hand)
{
    struct ldst_ops ops;
    struct ldst_result res;
    uint32_t insn = guest_fetch(pc, thumb);
    int st;

    ldst_prep(&ops, pc);
#if NGH_AW_PROBE
    g_awp_gm = f->spsr & 0x1Fu;
#endif
    ops.ctx = hand;
    g_emx = TEM_OT;
    st = ldst_exec(insn, thumb, pc, f->spsr, &ops, &res);
    trap_em_end(g_em, g_emx);
    return emulate_end(f, pc, insn, st, &res);
}

// the abort is entered by hand, the UND stub can only resume or forward to the UNDEF vector
static uint32_t und_dabt(struct frame *f, uint32_t pc, uint32_t vfsr, uint32_t vfar)
{
    (void)forward_abt(0, pc, vfsr, vfar);
    gr_set(0, M_ABT, 14, pc + 8u);
    bank_spsr_put(M_ABT, f->spsr);
    f->spsr = trap_und_abt_psr(f->spsr);
    f->lr = g_vbase + trap_vec_abt(0);
    return 0;
}

static uint32_t und_ldm(struct frame *f, uint32_t pc, uint32_t word)
{
    struct ldst_ops ops;
    struct ldst_result res;
    int st;

    // a guest in UND mode lost its LR_und to this trap: a load into lr could not land
    if ((f->spsr & 0x1Fu) == M_UND)
        return forward_und(pc);
    ldst_prep(&ops, pc);
#if NGH_AW_PROBE
    g_awp_gm = f->spsr & 0x1Fu;
#endif
    st = trap_und_ldm(word, pc, f->spsr, &ops, &res);
    if (st == LDST_UNSUP)
        return forward_und(pc);
    if (g_bad)
        fatal(FK_EMUL, g_bad, word);
    if (st == LDST_FAULT)
        return und_dabt(f, pc, res.fsr, res.fault_va);
    f->lr = res.next_pc;
    return 0;
}

// a privileged guest read for the learn rule: a failed one learns nothing and is no emulation error
static uint32_t gp_rd(void *c, uint32_t va, uint32_t *v)
{
    uint32_t b = g_bad, rc;

    (void)c;
    *v = 0;
    rc = (va & 3u) ? 0x1u : gm_mem(0, va, 4, 0, 0, v);
    if (g_bad && !rc)
        rc = 0x5u;
    g_bad = b;
    return rc;
}

static void __attribute__((noinline, noclone)) xc_pub(void)
{
    uint32_t r[XC_NROW], d[XC_NDROW], l[XC_NLROW], c[XC_NCROW], i;

    xc_rows(&g_xc, r);
    for (i = 0; i < XC_NROW; i++)
        xrow(XR_XTYP + i, r[i]);
    xc_drows(&g_xc, d);
    for (i = 0; i < XC_NDROW; i++)
        xrow(XR_XDLR + i, d[i]);
    xc_lrows(&g_xc, l);
    for (i = 0; i < XC_NLROW; i++)
        xrow(XR_XLC0 + i, l[i]);
    xc_crows(&g_xc, c);
    for (i = 0; i < XC_NCROW; i++)
        xrow(XR_KCN + i, c[i]);
}

static uint32_t __attribute__((noinline, noclone)) xc_free_kb(void)
{
    // the kernel page allocator's free count, read only at the learn, a game Leave and the exit
    uint32_t p, pages;

    if (gp_rd(0, XC_FREE_CELL, &p) || !p || gp_rd(0, p + XC_FREE_AVAIL, &pages))
        return XC_KB_UNREAD;
    return xc_kb_pages(pages);
}

static int __attribute__((noinline, noclone)) xc_exit_cap(uint32_t gm, uint32_t cur, uint32_t own, uint32_t rc1,
                                                        uint32_t rc2)
{
    struct xc_die d;
    uint32_t cat[4], fl = 0, thr, s;

    // an RProcess call enters Die again from its main thread's death (a no-op); the first entry's rows stay
    if (g_xc.frozen)
        return 0;
    d.lr = gr_lr(gm);
    d.sreg = gr_get(0, gm, XC_DIE_REG);
    d.rc_cur = rc1 | rc2;
    d.cur = cur;
    d.own = own;
    if (!g_xc.kproc && !gp_rd(0, EXIT_SUP_THREAD, &s) && s)
        (void)gp_rd(0, s + 0x18u, &g_xc.kproc);
    d.rc_req = gp_rd(0, EXIT_REQ_THREAD, &d.req);
    xc_die_who(gp_rd, 0, &d, g_gp.gproc, g_xc.kproc, &thr, &fl);
    xc_cat(gp_rd, 0, gr_get(0, gm, 3), cat, &fl);
    xc_exit_fill(&g_xc, gr_get(0, gm, 1), gr_get(0, gm, 2), cat, fl, thr, g_lim.ticks);
    xc_freeze(&g_xc, fl | (NGH_XC_HOLD ? XC_F_HELD : 0u), d.lr);
    xc_kb_set(&g_xc, XC_KB_EXIT, xc_free_kb());
    xc_pub();
    return 1;
}

#if NGH_XC_TRACE
static void __attribute__((noinline, noclone)) xc_trace_cap(void)
{
    uint32_t r[XC_NTROW], e[XC_NEROW], i;

    xc_trace_hit(gp_rd, 0, &g_xtk, g_xc.row[XC_R_THR], r, e);
    for (i = 0; i < XC_NTROW; i++)
        xrow(XR_TRSP + i, r[i]);
    for (i = 0; i < XC_NEROW; i++)
        xrow(XR_TRCH + i, e[i]);
}

static void __attribute__((noinline, noclone)) xc_tdie_cap(uint32_t pc, uint32_t slot, uint32_t thr)
{
    uint32_t s;

    if (!g_xc.kproc && !gp_rd(0, EXIT_SUP_THREAD, &s) && s)
        (void)gp_rd(0, s + 0x18u, &g_xc.kproc);
    if (!xc_tdie_gate(gp_rd, 0, pc, patch_mark_orig(slot), thr, g_xc.frozen, g_xc.kproc))
        return;
    // rows kept, not published: xc_trace_cap takes them at this thread's DProcess::Die
    g_xtk.thr = thr;
    xc_trace_early(gp_rd, 0, thr, g_xtk.row, g_xtk.x);
}
#endif

static int __attribute__((noinline, noclone)) xc_follow_cap(int first, uint32_t gm)
{
    uint32_t nx = xc_follow(gp_rd, 0, first, g_gp.gproc, gr_get(0, gm, 1), gr_get(0, gm, 2), g_xc.row[XC_R_TYP],
                            g_lim.cause, g_xc.fol, g_xc.t_learn, g_lim.ticks);

    if (!nx)
        return 0;
    xc_follow_step(&g_xc, &g_gp, nx);
    xc_pub();
    ST->gproc = nx;
    prow(PR_GPRC, nx);
    return 1;
}

static void __attribute__((noinline, noclone)) xc_leave_cap(uint32_t pc, uint32_t slot, uint32_t gm)
{
    uint32_t cur = 0, own = 0;

    if (!g_gp.gproc)
        return;
    if (!gp_rd(0, EXIT_CUR_THREAD, &cur))
        (void)gp_rd(0, cur + 0x18u, &own);
    if (!xc_leave_gate(pc, patch_mark_orig(slot), own, g_gp.gproc))
        return;
    // at the entry the code is still r0 and lr names the caller
    xc_leave_push(&g_xc, (int32_t)gr_get(0, gm, 0), gr_get(0, gm, 14), cur, g_lim.ticks);
    xc_kb_set(&g_xc, XC_KB_LEAVE, xc_free_kb());
    xc_pub();
}

static void __attribute__((noinline, noclone)) xc_cid_cap(uint32_t pc, uint32_t slot)
{
    uint32_t cur = 0, own = 0;
    int k;

    if (!g_gp.gproc)
        return;
    if (!gp_rd(0, EXIT_CUR_THREAD, &cur))
        (void)gp_rd(0, cur + 0x18u, &own);
    k = xc_cid_gate(pc, patch_mark_orig(slot), own, g_gp.gproc);
    if (k == XC_CID_NONE)
        return;
    xc_cid_push(&g_xc, k, g_lim.ticks);
    xc_pub();
}

#if NGH_MK_CAP
static void __attribute__((noinline, noclone)) mk_cap(uint32_t pc, uint32_t slot, uint32_t gm, int thumb)
{
    struct mk_in in;
    uint32_t rec[EV_MKCAP_NREC][4], i;

    (void)pc;
    for (i = 0; i < 4u; i++)
        in.r[i] = gr_get(0, gm, i);
    in.rf = 0;
    in.lr = 0;
    // gr_get flags a read of the monitor's own lr as an emulation error
    if (bank_mode(gm) == g_emode)
        in.rf |= MK_RF_LR;
    else
        in.lr = gr_get(0, gm, 14);
    in.sp = gr_get(0, gm, 13);
    in.gm = gm;
    in.thumb = thumb ? 1u : 0u;
    in.own = 0;
    if (gp_rd(0, EXIT_CUR_THREAD, &in.cur)) {
        in.cur = 0;
        in.rf |= MK_RF_CUR | MK_RF_OWN;
    } else if (!mk_kva(in.cur) || gp_rd(0, in.cur + XC_THR_OWNER, &in.own)) {
        in.own = 0;
        in.rf |= MK_RF_OWN;
    }
    in.ulr = gr_get(0, M_USR, 14);
    in.usp = gr_get(0, M_USR, 13);
    in.scan = mk_scan(gp_rd, 0, gm, in.sp, in.w, in.o);
    (void)mk_name(gp_rd, 0, in.r[0], in.nm[0]);
    (void)mk_name(gp_rd, 0, in.own, in.nm[1]);
    mk_pack(&in, slot, rec);
    for (i = 0; i < EV_MKCAP_NREC; i++)
        ev_put(EVK_MARK, EVF_EXIT_CAP, rec[i][0], rec[i][1], rec[i][2], rec[i][3]);
}
#endif

static uint32_t mark_site(struct frame *f, uint32_t pc, int thumb, uint32_t slot)
{
    struct ldst_ops ops;
    struct ldst_result res;
    uint32_t why = 0, gm = f->spsr & 0x1Fu, r0 = gr_get(0, gm, 0), ef = 0, eb = r0, ec = gm, ar[4];
    int a, gp, pcm = patch_mark_kind(slot) == PATCH_K_PCM;

    // the pcm contract is r0-r3 at the site, before the emulated instruction
    if (pcm)
        aud_args(gr_get, 0, gm, ar);
    ldst_prep(&ops, pc);
#if NGH_AW_PROBE
    g_awp_gm = gm;
#endif
#if NGH_MK_CAP
    int k = patch_mark_kind(slot);

    if (!pcm && (k == PATCH_K_MARK || k == PATCH_K_EXITIF))
        mk_cap(pc, slot, gm, thumb);
#endif
    // entered as a prefetch abort: if the access faults, the guest gets a data abort (forward_abt picks the data
    // vector and forward() redirects the stub) with LR_abt = pc + 8
    f->lr = pc + 8u;
    gp = trap_gp_hit(&g_gp, slot, pc, r0);
    if (gp == TGP_EXIT) {
        // the exit cause (r1-r3) and the dying thread are only live at the entry; records only, the park comes later
        uint32_t cur, own = 0, rc2 = 0, rc1 = gp_rd(0, EXIT_CUR_THREAD, &cur);
        int first;

        if (!rc1)
            rc2 = gp_rd(0, cur + 0x18u, &own);
        ev_put(EVK_MARK, EVF_EXIT_CAP, EXIT_CAP_REGS, gr_get(0, gm, 1), gr_get(0, gm, 2), gr_get(0, gm, 3));
        ev_put(EVK_MARK, EVF_EXIT_CAP, EXIT_CAP_THR, cur, own, rc1 | rc2 << 16);
#if NGH_XC_TRACE
        if ((first = xc_exit_cap(gm, cur, own, rc1, rc2)))
            xc_trace_cap();
#else
        first = xc_exit_cap(gm, cur, own, rc1, rc2);
#endif
        // the launcher handed the session to its app, so this Die publishes no exit
        if (xc_follow_cap(first, gm))
            gp = TGP_NONE;
    } else if (pc == XC_LEAVE_VA) {
        xc_leave_cap(pc, slot, gm);
    } else if (pc == XC_CID_VA) {
        xc_cid_cap(pc, slot);
#if NGH_XC_TRACE
    } else if (pc == XC_TDIE_VA) {
        xc_tdie_cap(pc, slot, r0);
#endif
    }
    a = trap_mark_run(slot, pc, thumb, f->spsr, &ops, &res, &why);
    if (a == TMK_BAD)
        fatal(FK_UNSUP, slot, pc);
    if (a == TMK_PARK) {
        prow(PR_EXIT, (pc & ~7u) | slot);
        ev_put(EVK_MARK, EVF_MARK_EXIT, pc, slot, r0, gm);
        park(why);
    }
    if (gp == TGP_LEARN) {
        ST->gproc = g_gp.gproc;
        prow(PR_GPRC, g_gp.gproc);
        g_xc.t_learn = g_lim.ticks;
        xc_kb_set(&g_xc, XC_KB_LEARN, xc_free_kb());
        xc_pub();
        ef = EVF_MARK_LEARN;
        eb = g_gp.gproc;
        ec = g_gp.cur;
    } else if (gp == TGP_EXIT) {
        // no park here, the fork settles on the C: and MMC writes and parks through REQUEST
#if !NGH_XC_HOLD
        ST->exit_seen = g_gp.exit_seen;
        prow(PR_EXIT, g_gp.exit_seen);
#endif
        ef = EVF_MARK_EXIT;
    }
    if (emulate_end(f, pc, patch_mark_orig(slot), a == TMK_DONE ? LDST_DONE : a == TMK_FAULT ? LDST_FAULT : 0, &res))
        return 1;
    if (pcm) {
        (void)aud_chunk(&g_aud, &g_aio, ar);
        aud_st();
    }
    g_mk[slot]++;
    prow(PR_MK0 + slot, g_mk[slot]);
    ev_put(EVK_MARK, ef, pc, slot, eb, ec);
#if NGH_AW_PROBE
    if (pcm)
        awp_m3(&g_awp, &g_awpio, pc, gr_get(0, gm, 6));
#endif
    return 0;
}

static void fill_guard(uint32_t pc, uint32_t va)
{
    if (pc == g_rep_pc && va == g_rep_va) {
        if (++g_rep_n > FILL_REPEAT_MAX)
            fatal(FK_FILL_LOOP, pc, va);
    } else {
        g_rep_pc = pc;
        g_rep_va = va;
        g_rep_n = 0;
    }
}

static __attribute__((noinline)) int dabt_wx(uint32_t pc, uint32_t dfsr, uint32_t dfar, uint32_t spsr)
{
    int rc;

    // a store into its own instruction's page would turn the page back to data before the retried fetch,
    // forever; HEAD's path stores through gm_mem, which runs imb_all on an exec page
    if (sh_wx_own(&g_sh, pc, dfar))
        return 0;
    if (sh_access_check(&g_sh, dfar, (dfsr >> 11) & 1u, (spsr & 0x1Fu) == M_USR) != 0)
        return 0;
    rc = (dfsr & (1u << 11)) ? sh_wx_write(&g_sh, dfar) : SH_WX_NOOP;
    return rc == SH_WX_DONE || (rc == SH_WX_NOOP && sh_wx_widen(&g_sh, dfar) == SH_WX_DONE);
}

static uint32_t dabt_run(struct frame *f)
{
    uint32_t dfsr = MRC15(0, 5, 0, 0), dfar = MRC15(0, 6, 0, 0), pc, vfsr = 0, c0, pr, tw, l1e;
    uint32_t fs = (dfsr & 0xFu) | ((dfsr >> 6) & 0x10u);
    struct sh_hand hand;
    int thumb, rc;

    // the bridge answers with an external abort once Main takes the FPGA away; under OUTER_WB an asynchronous
    // one with the bridges up is the PL310 (memcfg 0 and 1 read no BRGMODRST here and keep WINDOW_ABORT)
    pr = trap_abort_class(fs, dfar - WIN_VA < WIN_VA_SPAN,
                          fs == TFS_EXT_ASYNC && (g_dc.flags & CACHE_OUTER) && irq_bridge_ok(rd32(BRGMODRST)));
    if (pr) {
        prow(PR_FA, dfsr);
        prow(PR_FB, dfar);
        park(pr);
    }
    enter(f, M_ABT, 8u, dfar);
    ST->traps[NGST_TRAP_DABT] = ++g_trap[NGST_TRAP_DABT];
    thumb = (f->spsr & PSR_T) != 0;
    pc = f->lr - 8u;
    tw = trap_win_abort(g_sh.ops.rd, g_sh.ops.ctx, g_sh.g_ttb, (g_cp.control & CTRL_M) != 0, dfar, &l1e);
    if (tw == TW_FATAL)
        fatal(FK_WINDOW, dfsr, l1e);
    if (tw == TW_GUEST)
        return forward_abt(0, pc, TFS_SEC_TRANS, dfar);
    switch (trap_fsr_class(dfsr)) {
    case FC_TRANS:
        fill_guard(pc, dfar);
        c0 = pmu_cyc();
        g_wkon = 1;
        rc = sh_fill_hand(&g_sh, dfar, &vfsr, &hand);
        g_wkon = 0;
        g_fcyc += pmu_cyc() - c0;
        if (rc == SH_OK || rc == SH_SUBPAGE) {
            g_fills++;
            // sh_fill ended in a dsb, and the exception return synchronizes context
            f->lr = pc;
            return 0;
        }
        if (rc == SH_FAULT)
            return forward_abt(0, pc, vfsr, dfar);
        if (rc == SH_FWD)
            return emulate(f, pc, thumb, &hand);
        fatal(FK_SHADOW, (uint32_t)rc, dfar);
    case FC_PERM:
        // a store the guest allows flips an exec page to data; a Manager-only right widens the page once
        if (sh_wx_on(&g_sh) && dabt_wx(pc, dfsr, dfar, f->spsr)) {
            f->lr = pc;
            return 0;
        }
        return emulate(f, pc, thumb, 0);
    case FC_ALIGN:
        if (g_align)
            return forward_abt(0, pc, 0x1u, dfar);
        return emulate(f, pc, thumb, 0);
    default:
        fatal(FK_DABT, dfsr, dfar);
    }
}

static uint32_t pabt_run(struct frame *f)
{
    uint32_t ifsr = MRC15(0, 5, 0, 1), ifar = MRC15(0, 6, 0, 2), pc, vfsr = 0, gm, c0;
    int32_t chk;
    int rc;

    enter(f, M_ABT, 4u, ifar);
    gm = f->spsr & 0x1Fu;
    pc = f->lr - 4u;
    if (((ifsr & 0xFu) | ((ifsr >> 6) & 0x10u)) == FS_DEBUG)
        return cp15_trap(f, pc, (f->spsr & PSR_T) != 0, 1);
    ST->traps[NGST_TRAP_PABT] = ++g_trap[NGST_TRAP_PABT];
    if (ifar - MON_VA_BASE < MON_VA_SIZE)
        fatal(FK_WINDOW, ifsr, ifar);
    switch (trap_fsr_class(ifsr)) {
    case FC_TRANS:
        fill_guard(pc, ifar);
        c0 = pmu_cyc();
        g_wkon = 1;
        rc = sh_fill(&g_sh, ifar, &vfsr);
        g_wkon = 0;
        g_fcyc += pmu_cyc() - c0;
        if (rc == SH_OK || rc == SH_SUBPAGE) {
            g_fills++;
            // sh_fill ended in a dsb, and the exception return synchronizes context
            f->lr = pc;
            return 0;
        }
        if (rc == SH_FAULT)
            return forward_abt(1, pc, vfsr, ifar);
        fatal(FK_SHADOW, (uint32_t)rc, ifar);
    case FC_PERM:
        chk = sh_access_check(&g_sh, ifar, 0, gm == M_USR);
        // a fetch the guest allows flips a data page to exec; a Manager-only right widens the page once
        if (sh_wx_on(&g_sh) && chk == 0) {
            rc = sh_wx_exec(&g_sh, ifar);
            if (rc == SH_WX_DONE || (rc == SH_WX_NOOP && sh_wx_widen(&g_sh, ifar) == SH_WX_DONE)) {
                f->lr = pc;
                return 0;
            }
        }
        if (chk > 0)
            return forward_abt(1, pc, (uint32_t)chk, ifar);
        fatal(FK_PABT, ifsr, ifar);
    default:
        fatal(FK_PABT, ifsr, ifar);
    }
}

uint32_t trap_dabt_c(struct frame *f)
{
    uint32_t c0 = pmu_cyc(), i0 = pmu_ins(), rc;
#if NGH_PMU_EV
    uint32_t e0[PMUC_N];

    pmu_evrd(e0);
#endif
#if NGH_PMU_FRAME
    pmf_ent(&g_pmf, c0, i0, e0);
#endif

#if NGH_LIM_XHOLD
    g_dent = i0;
    g_dok = 1;
#endif
    rc = dabt_run(f);
#if NGH_LIM_XHOLD
    g_dok = 0;
#endif
    guard_sync();
#if NGH_PMU_EV
    pmc_out(e0);
#endif
    g_mcyc += pmu_cyc() - c0;
    g_minst += pmu_ins() - i0;
    return rc;
}

uint32_t trap_pabt_c(struct frame *f)
{
    uint32_t c0 = pmu_cyc(), i0 = pmu_ins(), rc;
#if NGH_PMU_EV
    uint32_t e0[PMUC_N];

    pmu_evrd(e0);
#endif
#if NGH_PMU_FRAME
    pmf_ent(&g_pmf, c0, i0, e0);
#endif

    rc = pabt_run(f);
    guard_sync();
#if NGH_PMU_EV
    pmc_out(e0);
#endif
    g_mcyc += pmu_cyc() - c0;
    g_minst += pmu_ins() - i0;
    return rc;
}

static void __attribute__((noreturn)) cold_restart(void);

static void __attribute__((noinline)) lim_stall(void)
{
    // out of line so a test finds its one wfi. A wake the guest would not take (masked, held or
    // gone) waits again, so it buys no slice; the tick, a takeable 74 or 75, PPI 30 and LIM_HOLD_US end the wait
    uint64_t t0 = gtime(), th = 0, t1;
    uint32_t c0 = pmu_cyc(), cls, seen = 0;

    for (;;) {
        dsb();
        __asm__ volatile("wfi" ::: "memory");
        cls = lim_wcls(rd32(PTM_ISR), rd32(GICC_HPPIR), rd32(GICD_ISPD(PL_GIC_ID_HYB_IRQ)), g_irqh.held, g_f->lr - 4u,
                       g_f->spsr, trap_irq_gate(&g_live, g_f->spsr & 0x1Fu, (int)g_irqh.held, 1) == TIG_DEFER);
        if (lim_wexit(&g_lim, cls) || gtime() - t0 >= LIM_HOLD_US * GT_PER_US)
            break;
        if (!seen)
            th = gtime();
        seen |= 1u << cls;
    }
    g_lwcy += pmu_cyc() - c0;
    t1 = gtime();
    g_lim.sgt += (uint32_t)(t1 - t0);
    lim_wdone(&g_lim, cls, seen, seen ? (uint32_t)(t1 - th) / GT_PER_US : 0u);
    if (lim_wake(&g_lim, rd32(PTM_ISR), rd32(GICC_HPPIR), t1)) {
        wr32(WDT_LOAD, LIM_RCK_LOAD(lim_slice(&g_lim, cls)));
        wr32(WDT_CTRL, LIM_WDT_RUN);
    }
}

static void tick(struct frame *f)
{
    uint32_t st, a, m;

    if (!irq_bridge_ok(rd32(BRGMODRST)))
        park(NGST_PARK_FPGA_LOST);
    st = win_rd(0, HYB_REG_OFF(HYB_R_STATUS));
    a = irq_tick(&g_tk, win_rd(0, HYB_REG_OFF(HYB_R_MAGIC)), st, ST->park_req, ST->flush_req, ST->flush_ack);
    if (a & TA_LOST)
        park(NGST_PARK_FPGA_LOST);
    if (a & TA_PARK)
        park(NGST_PARK_REQUEST);
    if (rumble_tick(&g_rm, &g_rio, g_tk.ticks))
        rm_pub();
    if (light_tick(&g_lt, &g_lio, g_tk.ticks, g_lim.armed))
        lt_pub();
    ST->heartbeat = ++g_hb;
    if (g_dma.out | g_dma.wout | g_dma.lout)
        dma_check(st);
    // a cancelled card transfr may still land, its range gets the completion maintenance here
    m = dma_cflush(&g_dma);
    if (m)
        dma_mt_cancel(m);
    if (a & TA_FLUSH) {
        dcache_clean_all();
        fl_ms(l2_clean());
        // the requester reads DDR: no ack while a sweep op may have been refused
        if (g_l2abt)
            park(NGST_PARK_L2);
        publish();
        ST->flush_ack = ST->flush_req;
        publish();
    }
    prow(PR_PCSM, f->lr - 4u);
    xrow(XR_GPSR, f->spsr);
    if (a & TA_DBG) {
        st_counters();
        prow(PR_TOUT, win_rd(0, HYB_REG_OFF(HYB_R_TIMEOUTS)));
        win_wr(0, HYB_REG_OFF(HYB_R_DBG0), g_tk.ticks);
        win_wr(0, HYB_REG_OFF(HYB_R_DBG1), g_irqn);
        win_wr(0, HYB_REG_OFF(HYB_R_DBG2), g_fills);
        win_wr(0, HYB_REG_OFF(HYB_R_DBG3), f->lr - 4u);
    }
    if (ST->mile_mask & NGST_MILE_SNAP) {
        snap_fill();
        ST->mile_mask = ST->mile_mask & ~NGST_MILE_SNAP;
    }
    if (a & TA_FPS) {
        ST->lcd_fps[ST->lcd_fps_idx % NGST_NFPS] = (uint16_t)win_rd(0, HYB_REG_OFF(HYB_R_LCD_FPS));
        ST->lcd_fps_idx = ST->lcd_fps_idx + 1u;
        gf_second(&g_gf);
        prow(PR_GFRM, g_gf.frames);
        prow(PR_GFPS, g_gf.last);
        prow(PR_GFHI, g_gf.secs);
        prow(PR_GH00 + ((g_gf.secs - 1u) % GF_NHIST) / 4u, gf_row(&g_gf, ((g_gf.secs - 1u) % GF_NHIST) / 4u));
        lim_second(&g_lim);
#if NGH_LIM_COST
        lim_csecond(&g_lc);
#endif
        lim_rows(&g_lim);
#if NGH_PMU_CENSUS
        // the counts at this trap's entry, as the limiter takes g_ient: the open trap is not in g_minst yet
        pmuc_second(&g_pmc, g_pce, g_ient, g_minst);
        pmc_rows();
#endif
#if NGH_PMU_FRAME
        pmf_sec();
#endif
    }
#if NGH_LIM_PCARRY
    lim_snow(&g_lim, gtime());
#endif
    if (a & TA_RESTART)
        cold_restart();
#if NGH_LIM_XHOLD
    xh_cur(f);
#endif
    lim_fmask(&g_lim, f->spsr, rd32(GICD_ISPD(PL_GIC_ID_HYB_FIQ)));
#if NGH_LIM_COST
    if (lim_tick(&g_lim, lc_cum(1u), 0u, st, g_lim.ev ? NGH_LIM_CPM : 0u))
#else
    if (lim_tick(&g_lim, g_ient, g_minst, st, g_lim.ev ? NGH_LIM_IPM : 0u))
#endif
        lim_stall();
}

static uint32_t irq_run(struct frame *f)
{
    uint32_t iar = rd32(GICC_IAR), us;

    g_f = f;
    g_emode = M_IRQ;
    g_bad = 0;
    if (f->lr - MON_VA_BASE < PL_MON_SIZE)
        fatal(FK_INTERNAL, M_IRQ, f->lr);
    fwd_invalidate(&g_fwd);
    // E is IRQ, never a guest ABT/UND: this only clears live[] when the guest left those modes
    (void)trap_live_check(&g_live, M_IRQ, f->spsr & 0x1Fu);
    irq_release(f->spsr & 0x1Fu, f->lr - 4u);
    switch (irq_kind(iar)) {
    case IK_GUEST:
        if (trap_irq_gate(&g_live, f->spsr & 0x1Fu, (int)g_irqh.held, 1) == TIG_DEFER) {
            wr32(GICD_ICEN(PL_GIC_ID_HYB_IRQ), GIC_BIT(PL_GIC_ID_HYB_IRQ));
            dsb();
            wr32(GICC_EOIR, iar);
            g_irqh.held = 1;
            g_irqh.n++;
            g_irqh.t0 = gtime();
            break;
        }
        wr32(GICC_EOIR, iar);
        g_irqn++;
#if NGH_LIM_XHOLD
        lim_xfwd(&g_lim, 1u, g_ient, g_minst);
#endif
        if (g_dma.out | g_dma.wout | g_dma.lout)
            dma_check(win_rd(0, HYB_REG_OFF(HYB_R_STATUS)));
        ev_put(EVK_IRQ, 0, f->lr - 4u, 0, 0, 0);
#if NGH_AW_PROBE
        awp_irq(&g_awp, &g_awpio, f->lr - 4u);
#endif
        if (ST->mile_mask & NGST_MILE_IRQ)
            milestone(NGST_MILE_IRQ);
        if (g_lim.wkp) {
            us = (uint32_t)(gtime() - g_lim.wkt) / GT_PER_US;
            if (us > g_lim.ilm)
                g_lim.ilm = us;
            g_lim.wkp = 0;
        }
        return 1;
    case IK_TICK:
        wr32(PTM_ISR, 1);
        wr32(GICC_EOIR, iar);
#if NGH_PMU_FRAME
        pmf_tick(f);
#endif
        tick(f);
        break;
    case IK_OTHER:
        wr32(GICC_EOIR, iar);
        g_irqo++;
        break;
    case IK_RCK:
        wr32(WDT_ISR, 1);
        wr32(GICC_EOIR, iar);
#if NGH_LIM_COST
        if (lim_recheck(&g_lim, lc_cum(0u), 0u))
#else
        if (lim_recheck(&g_lim, g_ient, g_minst))
#endif
            lim_stall();
        break;
    default:
        break;
    }
    f->lr -= 4u;
    return 0;
}

uint32_t trap_irq_c(struct frame *f)
{
    uint32_t c0 = pmu_cyc(), w0 = g_lwcy, i0 = pmu_ins(), rc;

    g_ient = i0;
#if NGH_PMU_EV
    pmu_evrd(g_pce);
#endif
#if NGH_PMU_FRAME
    pmf_ent(&g_pmf, c0, i0, g_pce);
#endif
    rc = irq_run(f);
    lim_fleak(&g_lim, rc, cpsr_rd(), f->spsr);
#if NGH_PMU_EV
    pmc_out(g_pce);
#endif
    // the stall wfi is not monitor work, and its delta is right whether PMCCNTR halts in it or not
    g_mcyc += pmu_cyc() - c0 - (g_lwcy - w0);
    g_minst += pmu_ins() - i0;
    return rc;
}

uint32_t trap_fiq_c(uint32_t lr, uint32_t spsr)
{
    uint32_t c = fiq_class(lr, spsr), d = (uint32_t)fiq_defer(lr, spsr, g_live.und);

    if (d) {
        g_fiqd++;
        fiq_count(&g_fqd, c, lr - 4u);
    } else {
        g_fiqf++;
#if NGH_LIM_XHOLD
        lim_xfwd(&g_lim, 0u, 0u, 0u);
#endif
    }
    prow(PR_FIQC, fiq_row(g_fiqf, g_fiqd));
    return d;
}

static void slot_fill(uint32_t pa, uint32_t len, uint32_t v)
{
    uint32_t a = VA_OF(pa), e = a + len;

    for (; a < e; a += 4u)
        wr32(a, v);
}

static void fpga_wait(void)
{
    for (;;) {
        if (ST->park_req == NGST_PARK_REQUEST)
            park(NGST_PARK_REQUEST);
        if (!irq_bridge_ok(rd32(BRGMODRST)))
            park(NGST_PARK_FPGA_LOST);
        if (win_rd(0, HYB_REG_OFF(HYB_R_MAGIC)) == HYB_MAGIC_VALUE &&
            (win_rd(0, HYB_REG_OFF(HYB_R_STATUS)) & (HYB_ST_ROM_LOADED | HYB_ST_DL_ACTIVE)) == HYB_ST_ROM_LOADED)
            return;
    }
}

static uint32_t ngage_prepare(uint32_t n, uint32_t *rom)
{
    uint32_t size, tail;

    fpga_wait();
    // the core load raises the OSD flag too: this start consumes it
    win_wr(0, HYB_REG_OFF(HYB_R_STATUS), HYB_ST_OSD_RESET);
    // soc held in reset while its RAM slots are cleared
    win_wr(0, HYB_REG_OFF(HYB_R_CTRL), 0);
    size = win_rd(0, HYB_REG_OFF(HYB_R_ROM_SIZE));
    if (!size || size > 0x02000000u || !n || n > (PL_PATCH_SIZE - 4u) / 8u)
        fatal(FK_PATCH, size, n);
    slot_fill(PL_RAM16_BASE, 0x01000000u, 0);
    slot_fill(PL_RAM2M_BASE, 0x00200000u, 0);
    slot_fill(PL_SRAM_BASE, 0x00040000u, 0);
    tail = fwd_tail_len(size);
    for (; tail; tail--)
        *(volatile uint8_t *)(VA_OF(PL_ROM_BASE) + size + tail - 1u) = 0xFF;
    *rom = PL_ROM_BASE;
    return size;
}

static uint32_t test_prepare(uint32_t n, uint32_t *rom)
{
    uint32_t size = ST->prog_size;

    // a test program always has halt sites: an empty list would let the guest program the real MMU
    if (!size || size > 0x01000000u || !n || n > (PL_PATCH_SIZE - 4u) / 8u)
        fatal(FK_PATCH, size, n);
    slot_fill(PL_RAM16_BASE + ((size + 3u) & ~3u), 0x01000000u - ((size + 3u) & ~3u), 0);
    *rom = PL_RAM16_BASE;
    return size;
}

static void __attribute__((noinline, noclone)) xc_boot(void)
{
    uint32_t i;

    xc_reset(&g_xc);
    for (i = 0; i < XC_NROW; i++)
        xrow(XR_XTYP + i, 0);
    for (i = 0; i < XC_NDROW; i++)
        xrow(XR_XDLR + i, 0);
    for (i = 0; i < XC_NLROW; i++)
        xrow(XR_XLC0 + i, 0);
    for (i = 0; i < XC_NCROW; i++)
        xrow(XR_KCN + i, 0);
#if NGH_XC_TRACE
    for (i = 0; i < XC_NTROW; i++)
        xrow(XR_TRSP + i, 0);
    for (i = 0; i < XC_NEROW; i++)
        xrow(XR_TRCH + i, 0);
    g_xtk.thr = 0;
#endif
}

static void __attribute__((noreturn)) guest_boot(void)
{
    const struct patch_entry *list = (const struct patch_entry *)VA_OF(PL_PATCH_BASE + 4u);
    uint32_t n, bad, size, i, rom, ver;
    uint64_t t, now;

    n = rd32(VA_OF(PL_PATCH_BASE));
    prow(PR_PCNT, n);
    size = g_test ? test_prepare(n, &rom) : ngage_prepare(n, &rom);
    g_l2rom = size;
    // the test profile never touches the window, and g_cdrv only acts outside it
    ver = g_test ? 0 : win_rd(0, HYB_REG_OFF(HYB_R_VERSION));
    g_cdrv = !g_test && nor_route_on(ST->cdrv, ver);
    // the soc is still in reset: the stream stops before the guest runs
    aud_start(&g_aud, &g_aio, ver);
    aud_st();
    rumble_start(&g_rm, &g_rio, ver);
    rm_pub();
    t = gtime();
    if (!g_rstr)
        g_tb = t;
    now = (t - g_tb) / GT_PER_US;
    bad = patch_apply_tt((uint8_t *)VA_OF(rom), size, list, n, g_cdrv, g_tdt, now);
    prow(PR_PBAD, bad);
    if (bad)
        fatal(FK_PATCH, bad, n);
    light_start(&g_lt, &g_lio, ver, light_verify((const uint8_t *)VA_OF(rom), size));
    lt_pub();
    g_tdt = now;
    for (i = 0; i < n; i++)
        MCR15(0, 7, 10, 1, VA_OF(rom + (patch_off(list[i].off) & ~31u)));
    dsb();
    // the zeroed guest RAM must reach DDR and leave no alias line for the MMU-off guest
    dcache_clean_inv_all();
    // in the L2 too, as the zeroing and the patches went through the outer write-back alias
    g_btms += l2_ci();
    l2_chk(2u);
    imb_all();

    cp15_reset(&g_cp, MON_DOMAIN);
    nor_reset(&g_nor);
    g_sh.pam.rom_size = size;
    g_sh.pam.profile = g_test ? PAM_PROFILE_TEST : PAM_PROFILE_NORMAL;
    if (!g_test) {
        win_wr(0, HYB_REG_OFF(HYB_R_CTRL), HYB_CTRL_SOC_RUN);
        (void)fwd_flash_sync(&g_sh.pam, win_rd(0, HYB_REG_OFF(HYB_R_STATUS)));
    }
    sync_shadow_view();
    g_ident = 1;
    // a cold restart leaves the kernel shadow, its pins and live[] behind
    sh_unpin_all(&g_sh);
    sh_drop_all(&g_sh);
    sh_wx_reset(&g_sh);
    g_wxe = g_wxw = 0;
    g_live.abt = 0;
    g_live.und = 0;
    g_irqh.held = 0;
    // the game process is learned again in every guest boot
    g_gp.rd = gp_rd;
    trap_gp_reset(&g_gp);
    ST->gproc = 0;
    ST->exit_seen = 0;
    prow(PR_GPRC, 0);
    xc_boot();
    dma_init(&g_dma);
    if (NGH_BOOT_WB) {
        g_sh.memcfg = g_memcfg;
        trap_sw_memcfg(&g_sw, g_memcfg);
    }
    g_dma.wb = (g_memcfg & SH_MC_WB) != 0;
#if NGH_PLAY_WB
    g_sh.wx_off = 0;
#endif
#if NGH_WX_OSD
    g_sh.wx_osd = !g_test && (win_rd(0, HYB_REG_OFF(HYB_R_STATUS)) & HYB_ST_WX);
#endif
    cache_init(&g_dc, &g_dcops, trap_l2_flags(g_memcfg), VA_OF(0u));
    gf_init(&g_gf);
#if NGH_LIM_COST
    lc_boot();
#else
    lim_reset(&g_lim, pmu_ins(), g_minst);
#endif
#if NGH_PMU_CENSUS
    pmc_boot();
#endif
    fwd_invalidate(&g_fwd);
    g_align = (g_cp.control >> 1) & 1u;
    set_vbase(0);
    real_dacr();
    tlb_flush();
    if (!g_test)
        irq_hw_init();
    ST->state = NGST_STATE_RUN;
    publish();
    guest_enter(0, GUEST_CPSR);
}

// L1 D clean+invalidate, ICIALLU, BPIALL, then the start sequence; ROM lines are clean after patching
static void __attribute__((noreturn)) cold_restart(void)
{
    g_rstr++;
    dcache_clean_inv_all();
    imb_all();
    g_btms = l2_ci();
    l2_chk(3u);
    guest_boot();
}

static uint32_t read_cost(void)
{
    uint32_t c0, i, nc, gt;

    c0 = pmu_cyc();
    for (i = 0; i < 16u; i++)
        (void)ST->park_req;
    nc = (pmu_cyc() - c0) / 16u;
    c0 = pmu_cyc();
    for (i = 0; i < 16u; i++)
        (void)rd32(GTIMER);
    gt = (pmu_cyc() - c0) / 16u;
    return (nc < 0xFFFFu ? nc : 0xFFFFu) << 16 | (gt < 0xFFFFu ? gt : 0xFFFFu);
}

static uint32_t pmu_cost(void)
{
    uint32_t c0 = pmu_cyc(), i;

    for (i = 0; i < 16u; i++)
        (void)pmu_cyc();
    return (pmu_cyc() - c0) / 16u;
}

static uint32_t __attribute__((noinline)) lim_probe(uint32_t ev)
{
    // PMSELR stays 0: pmu_ins reads counter 0 through PMXEVCNTR
    uint32_t n = 4096u;

    MCR15(0, 9, 12, 5, 0);
    isb();
    MCR15(0, 9, 13, 1, ev);
    MCR15(0, 9, 13, 2, 0);
    isb();
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
    return (uint32_t)(((uint64_t)MRC15(0, 9, 13, 2) * 1000u) >> 13);
}

static void lim_init(void)
{
    uint32_t scu = rd32(DEV(LIM_SCU_PWR)), r08, r68, ev;

    r08 = lim_probe(LIM_EV_INST);
    r68 = lim_probe(LIM_EV_RENAME);
    ev = lim_pick(r08, r68, scu);
    if (ev)
        (void)lim_probe(ev);
    g_lim.ev = ev;
    g_lim.mev = ev << 24 | LIM_SCU_CPU1(scu) << 22 | lim_min(r08, 2047u) << 11 | lim_min(r68, 2047u);
#if NGH_PMU_EV
    pmu_evinit();
#endif
}

void mon_main(void)
{
    uint32_t mc;

    mmu_boot();
    // PMCR E and C with D clear (one count per cycle), cycle counter and counter 0 on, no overflow IRQ; they run on
    // at park
    MCR15(0, 9, 12, 0, 0x5u);
    MCR15(0, 9, 14, 2, 1u << 31 | 1u);
    MCR15(0, 9, 12, 1, 1u << 31 | 1u);
    isb();
    g_t0 = gtime();
    st_init();
    g_ucyc = read_cost();
    g_pmcy = pmu_cost();
    lim_init();
    ev_init();
#ifdef NGH_SECCHK
    schk_init();
#endif
    fwd_init(&g_fwd, &g_sh.pam);
    irq_tick_init(&g_tk);
    mc = ST->memcfg;
    ST->memcfg = 0;
    publish();
    if (!mc)
        mc = NGH_MEMCFG_DEF;
    if (!trap_memcfg_ok(mc))
        fatal(FK_MEMCFG, mc, 0);
    g_memcfg = mc;
    g_sh.memcfg = mc;
    if (sh_wx_on(&g_sh))
        real_dacr();
    trap_sw_memcfg(&g_sw, g_memcfg);
    cache_init(&g_dc, &g_dcops, trap_l2_flags(g_memcfg), VA_OF(0u));
    if (g_memcfg & NGST_MC_OUTER_WB)
        alias_outer();
    prow(PR_MCFG, g_memcfg);
    if (mc)
        st_version(mc);
    g_test = ST->profile == NGST_PROFILE_TEST;
    if (!g_test && ST->profile != NGST_PROFILE_NGAGE)
        fatal(FK_PROFILE, ST->profile, 0);
    g_cplog = trap_cp15_log(ST->log_mode, ST->profile);
    guest_boot();
}
