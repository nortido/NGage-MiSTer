// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/platform.h"
#include "../common/hyb_regs.h"
#include "guest_census.h"
#include "cp15.h"
#include "pamap.h"
#include "shadow.h"
#include "cache.h"

#define VA_OF(pa)       (MON_VA_BASE + ((pa) - PL_MON_BASE))
#define ALIAS_LO        PL_SRAM_BASE
#define ALIAS_HI        0x3C000000u
#define DEV_VA          (MON_VA_BASE + 0x04000000u)
#define DEV(pa)         (DEV_VA + ((pa) & 0xFFFFFu))
#define RST_VA          (MON_VA_BASE + 0x04100000u)
#define RST(pa)         (RST_VA + ((pa) & 0xFFFFFu))
#define RST_SEC         0xFFD00000u
#define WIN_VA          (MON_VA_BASE + 0x08000000u)
#define WIN_FWD_SPAN    0x18000000u
#define SEC             0x00100000u

#define D_SH_L1         (PL_MONDATA_BASE + 0x0000u)
#define D_DEV_L2        (PL_MONDATA_BASE + 0x8000u)
#define D_POOL          (PL_MONDATA_BASE + 0x10000u)
#define POOL_N          256u
#define GTIMER          DEV(0xFFFEC200u)
#define GT_PER_MS       200000u

#define DOM_BITS        (MON_DOMAIN << 5)
#define SEC_WB          (0x4406u | DOM_BITS)
#define SEC_OWB         (0x5406u | DOM_BITS)
#define SEC_XN          0x10u
#define SEC_SO          (0x0402u | DOM_BITS)
#define SEC_NC          (0x1402u | DOM_BITS)
#define PG_DEV          0x17u
#define TTBR_WALK       0x40u
#define DACR_BOOT       ((1u << (2u * MON_DOMAIN)) | 1u)
#define DACR_IDENT      0x55555555u
#define CTRL_M          0x1u

#define MCR15(op1, crn, crm, op2, v) \
    __asm__ volatile("mcr p15, " #op1 ", %0, c" #crn ", c" #crm ", " #op2 :: "r"(v) : "memory")
#define MRC15(op1, crn, crm, op2) ({ uint32_t v_; \
    __asm__ volatile("mrc p15, " #op1 ", %0, c" #crn ", c" #crm ", " #op2 : "=r"(v_) :: "memory"); v_; })

extern void dcache_clean_all(void);
extern void dcache_clean_inv_all(void);

static struct sh g_sh;
static struct cp15_state g_cp;
static int g_ident;
static uint32_t g_vbase;
static uint32_t g_align;
static uint32_t g_fills;
static uint32_t g_prefills;
static uint32_t g_open;
static uint32_t g_fwdn;
static uint32_t g_win_va;
static uint32_t g_win_l1e;
static uint32_t g_wtim;
static uint32_t g_wfi_cyc;
static uint32_t g_wgcy;
static uint32_t g_wkcy, g_wkcn;
static int g_wkon;

_Static_assert(POOL_N <= SH_POOL_MAX, "one section flag per pool table");
_Static_assert(SEC_OWB == (SEC_WB | 0x1000u), "TEX 101 C0 B1: inner and outer write-back write-allocate");

extern uint32_t fwd_und, fwd_svc, fwd_pabt_t, fwd_dabt, fwd_irq, fwd_fiq;

static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t pmu_cyc(void) { return MRC15(0, 9, 13, 0); }
static inline uint32_t pmu_ins(void) { return MRC15(0, 9, 13, 2); }
#if NGH_PMU_EV
static const uint32_t pmu_evs[PMUC_N] = { NGH_PMUC_EVS };

static void __attribute__((noinline)) pmu_evinit(void)
{
    uint32_t i;

    for (i = 0; i < PMUC_N; i++) {
        MCR15(0, 9, 12, 5, i + 1u);
        isb();
        MCR15(0, 9, 13, 1, pmu_evs[i]);
        MCR15(0, 9, 13, 2, 0);
    }
    MCR15(0, 9, 12, 1, PMUC_CNTEN);
    // R-FW8: pmu_ins reads PMXEVCNTR, so counter 0 must be selected again before any return
    MCR15(0, 9, 12, 5, 0);
    isb();
}

static void __attribute__((noinline)) pmu_evrd(uint32_t *v)
{
    uint32_t i;

    for (i = 0; i < PMUC_N; i++) {
        MCR15(0, 9, 12, 5, i + 1u);
        isb();
        v[i] = MRC15(0, 9, 13, 2);
    }
    MCR15(0, 9, 12, 5, 0);
    isb();
}
#endif

static void tlb_flush(void)
{
    dsb();
    MCR15(0, 8, 7, 0, 0);
    MCR15(0, 7, 5, 6, 0);
    dsb();
    isb();
}

static void imb_all(void)
{
    dsb();
    MCR15(0, 7, 5, 0, 0);
    MCR15(0, 7, 5, 6, 0);
    dsb();
    isb();
}

static int alias_ok(uint32_t host)
{
    return host >= ALIAS_LO && host < ALIAS_HI;
}

static void dc_op(uint32_t kind, uint32_t va)
{
    va &= ~31u;
    if (kind == CP15_ACT_CLEAN_D_MVA)
        MCR15(0, 7, 10, 1, va);
    else if (kind == CP15_ACT_INV_D_MVA)
        MCR15(0, 7, 6, 1, va);
    else
        MCR15(0, 7, 14, 1, va);
}

// the L1 steps on the monitor alias of the host PA; the L2 steps by PA when CACHE_OUTER is set
static void al_mva(void *c, uint32_t kind, uint32_t va)
{
    (void)c;
    dc_op(kind, va);
}

static void al_dsb(void *c)
{
    (void)c;
    dsb();
}

static void al_l2(void *c, uint32_t off, uint32_t pa)
{
    (void)c;
    wr32(DEV(PL_L2_BASE) + off, pa);
}

static void al_sync(void *c)
{
    (void)c;
    // the board's L2C-310 is r3p3 (CACHE_ID 0x410030C9): erratum 753970 (r3p0, sync at 0x740) and 588369 (< r2p0,
    // CLEAN_INV_PA) do not apply
    wr32(DEV(PL_L2_BASE + L2C_CACHE_SYNC), 0);
    dsb();
}

static uint32_t al_abt(void *c)
{
    (void)c;
    // ISR.A: CPSR.A stays set in the monitor, so an external abort (PL310 SLVERR) waits here instead of being taken
    return MRC15(0, 12, 1, 0) >> 8 & 1u;
}

static const struct cache_ops g_dcops = { 0, al_mva, al_l2, al_dsb, al_sync, al_abt };
static struct cache g_dc;
// fatal and the FK_ kinds follow in main.c, which includes this file; main.c asserts the values
#define L2_ABT_FK 0x110u
static void __attribute__((noreturn)) fatal(uint32_t kind, uint32_t a, uint32_t b);
// ROM slot bytes in the sweep set: the whole slot until guest_boot knows the image size
static uint32_t g_l2rom;
static uint32_t g_btms, g_pkms, g_flms;
// a sweep left a PL310 abort pending: sticky, the abort is never taken
static uint32_t g_l2abt;
// the guard pass lines whose L2 step waits for m_barrier
static uint32_t g_l2va[sizeof g_sh.gline / sizeof g_sh.gline[0]];
static uint32_t g_l2n;

static void alias_l2(uint32_t va, uint32_t len)
{
    // the alias is outer write-back while a guest view may be outer non-cacheable (C=0, guard page, MMU or
    // caches off) and bypass the L2, so a monitor access drops the L2 line too after its L1 clean+invalidate
    if (g_dc.flags & CACHE_OUTER)
        cache_rd_va(&g_dc, va, len);
}

// guest stores bypass this cacheable alias (guest C=0, code the guest copied): drop any older line first
static void alias_fresh(uint32_t a)
{
    MCR15(0, 7, 14, 1, a & ~31u);
    dsb();
    alias_l2(a, 1u);
}

static inline __attribute__((always_inline)) uint32_t l2_sweep(int (*op)(struct cache *, uint32_t, uint32_t))
{
    uint32_t b[SH_L2_NSET], l[SH_L2_NSET], n, i, t0 = rd32(GTIMER);

    n = sh_l2_set(g_sh.memcfg, g_l2rom, b, l);
    for (i = 0; i < n; i++)
        if (op(&g_dc, b[i], l[i]))
            g_l2abt = 1;
    return (rd32(GTIMER) - t0 + GT_PER_MS - 1u) / GT_PER_MS;
}

static __attribute__((noinline)) uint32_t l2_inv(void)
{
    return (g_dc.flags & CACHE_OUTER) ? l2_sweep(cache_sweep_inv) : 0;
}

static __attribute__((noinline)) uint32_t l2_ci(void)
{
    return (g_dc.flags & CACHE_OUTER) ? l2_sweep(cache_sweep_ci) : 0;
}

static __attribute__((noinline)) uint32_t l2_clean(void)
{
    return (g_dc.flags & CACHE_OUTER) ? l2_sweep(cache_sweep_clean) : 0;
}

static void fl_ms(uint32_t ms)
{
    if (ms > g_flms)
        g_flms = ms;
}

static __attribute__((noinline)) void l2_chk(uint32_t site)
{
    // fatal parks, and park stores reason 6 for the pending abort
    if (g_l2abt)
        fatal(L2_ABT_FK, site, 0);
}

// a page joins the guard set with no dirty line left; len 0 turns every page write-through
static void m_guard(void *c, uint32_t host, uint32_t len)
{
    (void)c;
    if (!len) {
        dcache_clean_inv_all();
        // every guest page turns outer non-cacheable too
        fl_ms(l2_ci());
        l2_chk(4u);
    } else if (alias_ok(host)) {
        cache_in_pre(&g_dc, host, len);
    }
}

static void m_clean(void *c, const void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u, e = (uint32_t)p + len;

    (void)c;
    for (; a < e; a += 32u)
        MCR15(0, 7, 10, 1, a);
    dsb();
}

static void m_barrier(void *c)
{
    (void)c;
    dsb();
    // the guard pass lines take one L2 sequence with one SYNC, as Linux l2c210_flush_range does
    if ((g_dc.flags & CACHE_OUTER) && g_l2n) {
        cache_rd_list(&g_dc, g_l2va, g_l2n);
        g_l2n = 0;
    }
}

static int m_rd_at(uint32_t pa, uint32_t *v)
{
    struct pam_res r;
    uint32_t a;

    if (g_ident) {
        *v = trap_ident_desc(pa);
        return 0;
    }
    r = pamap_page(pa, 0, &g_sh.pam);
    if (r.kind == PAM_FWD || !alias_ok(r.host_pa) || (pa & 3u))
        return -1;
    a = VA_OF(r.host_pa);
    // guest table lines may be dirty (cacheable guest writes) or stale (non-cacheable guest writes)
    MCR15(0, 7, 14, 1, a & ~31u);
    dsb();
    alias_l2(a, 1u);
    *v = rd32(a);
    return 0;
}

static int m_rd_line_at(uint32_t pa, uint32_t *v)
{
    struct pam_res r;
    uint32_t a, i;

    pa &= ~31u;
    if (g_ident) {
        for (i = 0; i < 8u; i++)
            v[i] = trap_ident_desc(pa + i * 4u);
        return 0;
    }
    r = pamap_page(pa, 0, &g_sh.pam);
    if (r.kind == PAM_FWD || !alias_ok(r.host_pa))
        return -1;
    a = VA_OF(r.host_pa);
    // as in m_rd, once per 32-byte line
    MCR15(0, 7, 14, 1, a);
    dsb();
    alias_l2(a, 1u);
    for (i = 0; i < 8u; i++)
        v[i] = rd32(a + i * 4u);
    return 0;
}

static int m_page_va(void *c, uint32_t pa, uint32_t *va)
{
    struct pam_res r;

    (void)c;
    // without the MMU m_rd_raw synthesizes the line from its PA
    if (g_ident) {
        *va = pa;
        return 0;
    }
    r = pamap_page(pa, 0, &g_sh.pam);
    if (r.kind == PAM_FWD || !alias_ok(r.host_pa))
        return -1;
    *va = VA_OF(r.host_pa);
    return 0;
}

static void m_inv_line(void *c, uint32_t va)
{
    (void)c;
    if (g_ident)
        return;
    // as in m_rd_line; the caller issues one dsb after the last line
    MCR15(0, 7, 14, 1, va);
    if (g_dc.flags & CACHE_OUTER) {
        if (g_l2n < sizeof g_l2va / sizeof g_l2va[0])
            g_l2va[g_l2n++] = va;
        else
            alias_l2(va, 1u);
    }
}

static void m_rd_raw(void *c, uint32_t va, uint32_t *v)
{
    uint32_t i;

    (void)c;
    if (g_ident) {
        for (i = 0; i < 8u; i++)
            v[i] = trap_ident_desc(va + i * 4u);
        return;
    }
    for (i = 0; i < 8u; i++)
        v[i] = rd32(va + i * 4u);
}

static int m_rd(void *c, uint32_t pa, uint32_t *v)
{
    uint32_t c0;
    int rc;

    (void)c;
    if (!g_wkon)
        return m_rd_at(pa, v);
    c0 = pmu_cyc();
    rc = m_rd_at(pa, v);
    g_wkcy += pmu_cyc() - c0;
    g_wkcn++;
    return rc;
}

static int m_rd_line(void *c, uint32_t pa, uint32_t *v)
{
    uint32_t c0;
    int rc;

    (void)c;
    if (!g_wkon)
        return m_rd_line_at(pa, v);
    c0 = pmu_cyc();
    rc = m_rd_line_at(pa, v);
    g_wkcy += pmu_cyc() - c0;
    g_wkcn++;
    return rc;
}

static void m_tail(void *c, uint32_t host_pa, uint32_t len)
{
    uint32_t a, i;

    if (!alias_ok(host_pa) || !alias_ok(host_pa + len - 1u))
        return;
    a = VA_OF(host_pa);
    for (i = 0; i < len; i++)
        ((volatile uint8_t *)a)[i] = 0xFF;
    m_clean(c, (const void *)a, len);
    alias_l2(a, len);
}

static void m_code_sync(void *c, uint32_t host, uint32_t len)
{
    (void)c;
    // cache_sweep_clean steps the L2 only; cache_out cleans the L1 lines by alias too
    if (alias_ok(host))
        cache_out(&g_dc, host, len);
    dsb();
    imb_all();
}

static void m_tlb_mva(void *c, uint32_t va)
{
    (void)c;
    (void)va;
    // as on sh_drop_mva's path: the whole TLB
    tlb_flush();
}

static void mmu_boot(void)
{
    uint32_t *l1 = (uint32_t *)VA_OF(D_SH_L1);
    uint32_t *dl2 = (uint32_t *)VA_OF(D_DEV_L2);
    uint32_t pa, i;

    sh_init(&g_sh, l1, (uint32_t *)VA_OF(D_POOL), D_POOL, POOL_N, MON_VA_BASE, MON_VA_SIZE, MON_DOMAIN);
    g_sh.ops.rd = m_rd;
    g_sh.ops.rd_line = m_rd_line;
    g_sh.ops.page_va = m_page_va;
    g_sh.ops.inv_line = m_inv_line;
    g_sh.ops.rd_raw = m_rd_raw;
    g_sh.ops.tail = m_tail;
    g_sh.ops.clean = m_clean;
    g_sh.ops.barrier = m_barrier;
    g_sh.ops.guard = m_guard;
    // the control arm keeps HEAD's image: no wx op is ever called there
    if (NGH_WX) {
        g_sh.ops.code_sync = m_code_sync;
        g_sh.ops.tlb_mva = m_tlb_mva;
    }
    sh_window_set(&g_sh, VA_OF(PL_MON_BASE), PL_MON_BASE | SEC_WB);
    sh_window_set(&g_sh, VA_OF(PL_MONDATA_BASE), PL_MONDATA_BASE | SEC_WB | SEC_XN);
    for (pa = PL_STATUS_BASE; pa < ALIAS_LO; pa += SEC)
        sh_window_set(&g_sh, VA_OF(pa), pa | SEC_NC | SEC_XN);
    // the audio ring and the checker log take the event ring's attributes: publish() alone makes their bytes visible
    // to the FPGA reader and to board memory tools
    for (pa = ALIAS_LO; pa < ALIAS_HI; pa += SEC)
        sh_window_set(&g_sh, VA_OF(pa), pa | (pa == PL_AUDIO_BASE || pa == PL_SCHK_BASE ? SEC_NC : SEC_WB) | SEC_XN);
    // strongly-ordered: the PAHI write must reach the FPGA before the forward that depends on it
    for (pa = 0; pa < WIN_FWD_SPAN; pa += SEC)
        sh_window_set(&g_sh, WIN_VA + pa, (HYB_WIN_PHYS + pa) | SEC_SO | SEC_XN);
    sh_window_set(&g_sh, WIN_VA + HYB_WIN_REGS, (HYB_WIN_PHYS + HYB_WIN_REGS) | SEC_SO | SEC_XN);
    for (i = 0; i < 256; i++)
        dl2[i] = 0;
    dl2[0xEC] = 0xFFFEC000u | PG_DEV;
    dl2[0xED] = 0xFFFED000u | PG_DEV;
    dl2[(PL_L2_BASE >> 12) & 0xFFu] = PL_L2_BASE | PG_DEV;
    m_clean(0, dl2, 1024);
    sh_window_set(&g_sh, DEV_VA, D_DEV_L2 | DOM_BITS | 1u);
    // reset manager page: the tick reads BRGMODRST before any window access
    sh_window_set(&g_sh, RST_VA, RST_SEC | SEC_SO | SEC_XN);
    m_clean(0, l1, 16384);
    MCR15(0, 3, 0, 0, DACR_BOOT);
    isb();
    dsb();
    MCR15(0, 2, 0, 0, D_SH_L1 | TTBR_WALK);
    isb();
    tlb_flush();
}

static __attribute__((noinline)) void alias_outer(void)
{
    uint32_t pa;

    // lines an earlier session left in the slots go first, by PA and L2 before L1; then the guest slots take the
    // guest's outer write-back type before the first guest_boot touches them
    g_l2rom = PL_RAM16_BASE - PL_ROM_BASE;
    g_btms = l2_inv();
    l2_chk(1u);
    dcache_clean_inv_all();
    for (pa = ALIAS_LO; pa < ALIAS_HI; pa += SEC)
        if (sh_l2_host(NGST_MC_OUTER_WB, PL_RAM16_BASE - PL_ROM_BASE, pa))
            sh_window_set(&g_sh, VA_OF(pa), pa | SEC_OWB | SEC_XN);
    tlb_flush();
}

static void real_dacr(void)
{
    MCR15(0, 3, 0, 0, (g_cp.control & CTRL_M) ? sh_real_dacr(&g_sh, g_cp.dacr) : DACR_IDENT);
    isb();
}

static void set_vbase(uint32_t b)
{
    g_vbase = b;
    fwd_und = b + 0x04u;
    fwd_svc = b + 0x08u;
    fwd_dabt = b + 0x10u;
    fwd_irq = b + 0x18u;
    fwd_fiq = b + 0x1Cu;
    dsb();
}

#ifdef SH_DROP_VERIFY
#define DROP_VERIFY_FK 0x104u
#endif

static void drop_verify(void)
{
#ifdef SH_DROP_VERIFY
    int32_t i = sh_check_marked(&g_sh);

    if (i >= 0)
        fatal(DROP_VERIFY_FK, (uint32_t)i, g_sh.l1[i]);
#endif
}

static void window_guard(void)
{
    uint32_t va = 0, e = 0, c0;
    int rc;

    if (!(g_cp.control & CTRL_M) || g_win_va)
        return;
    c0 = pmu_cyc();
    rc = sh_window_guest(&g_sh, &va, &e);
    g_wgcy += pmu_cyc() - c0;
#ifdef SH_DROP_VERIFY
    {
        uint32_t va2 = 0, e2 = 0;
        int rc2;

        g_sh.ops.rd_line = 0;
        g_sh.ops.inv_line = 0;
        rc2 = sh_window_guest(&g_sh, &va2, &e2);
        g_sh.ops.rd_line = m_rd_line;
        g_sh.ops.inv_line = m_inv_line;
        if (rc2 != rc || (rc == 1 && (va2 != va || e2 != e)))
            fatal(DROP_VERIFY_FK, 0xB5200000u | (((uint32_t)rc & 0xFFu) << 8) | ((uint32_t)rc2 & 0xFFu), va2);
    }
#endif
    if (rc == 1) {
        g_win_va = va;
        g_win_l1e = e;
    } else if (rc != 0) {
        g_win_va = MON_VA_BASE;
        g_win_l1e = 0xFFFFFFFFu;
    }
}

static void hw_mmu_mode(void *c, uint32_t m)
{
    (void)c;
    g_ident = !m;
    // monitor aliases are cacheable while an MMU-off guest wrote memory non-cacheable
    dcache_clean_inv_all();
    // an MMU-off guest bypasses the L2 as well
    fl_ms(l2_ci());
    l2_chk(5u);
    sh_drop_all(&g_sh);
    drop_verify();
    real_dacr();
    tlb_flush();
    window_guard();
}

static void hw_drop_all(void *c)
{
    (void)c;
    sh_drop_all(&g_sh);
    drop_verify();
    tlb_flush();
    window_guard();
}

static void hw_drop_mva(void *c, uint32_t va)
{
    (void)c;
    sh_drop_mva(&g_sh, va);
    tlb_flush();
    if (va - MON_VA_BASE < MON_VA_SIZE)
        window_guard();
}

static void hw_set_dacr(void *c, uint32_t v)
{
    (void)c;
    (void)v;
    real_dacr();
}

static void hw_vbase(void *c, uint32_t b)
{
    (void)c;
    set_vbase(b);
}

static void hw_align(void *c, uint32_t a)
{
    (void)c;
    g_align = a;
}

static int host_fill(uint32_t va, uint32_t *host)
{
    uint32_t fsr;
    int rc;

    if (va - MON_VA_BASE < MON_VA_SIZE)
        return 0;
    if (trap_host_of(&g_sh, va, host))
        return 1;
    rc = sh_fill(&g_sh, va, &fsr);
    if (rc != SH_OK && rc != SH_SUBPAGE)
        return 0;
    g_fills++;
    return trap_host_of(&g_sh, va, host);
}

static void hw_dmva(void *c, uint32_t kind, uint32_t va)
{
    uint32_t h;

    (void)c;
    if (!host_fill(va, &h) || !alias_ok(h))
        return;
    cache_mva(&g_dc, kind, h);
}

static void hw_dall(void *c)
{
    (void)c;
    dcache_clean_inv_all();
}

static void hw_dcall(void *c)
{
    (void)c;
    dcache_clean_all();
}

static void hw_imb(void *c)
{
    (void)c;
    imb_all();
}

static void hw_drain(void *c)
{
    (void)c;
    cache_drain(&g_dc);
}

static void hw_wfi(void *c)
{
    uint32_t c0 = pmu_cyc(), t0 = rd32(GTIMER);

    (void)c;
    dsb();
    __asm__ volatile("wfi" ::: "memory");
    g_wtim += rd32(GTIMER) - t0;
    g_wfi_cyc += pmu_cyc() - c0;
}

static const struct trap_hw g_hw = {
    0, hw_mmu_mode, hw_drop_all, hw_drop_mva, hw_set_dacr, hw_vbase, hw_align,
    hw_dmva, hw_dall, hw_dcall, hw_imb, hw_drain, hw_wfi
};

static int guest_word(uint32_t va, uint32_t *w)
{
    uint32_t h;

    if (!trap_host_of(&g_sh, va, &h) || !alias_ok(h) || (va & 3u))
        return 0;
    alias_fresh(VA_OF(h));
    *w = rd32(VA_OF(h));
    return 1;
}

static void mmu_prefill(uint32_t vec_va, uint32_t sp)
{
    uint32_t pages[TRAP_PF_MAX], n, i, word = 0, lit_va, lit = 0;
    int have_lit = 0, rc;

    trap_pin_begin(&g_sh);
    (void)trap_pin(&g_sh, vec_va);
    (void)guest_word(vec_va, &word);
    if (trap_vec_literal(vec_va, word, &lit_va)) {
        (void)trap_pin(&g_sh, lit_va);
        have_lit = guest_word(lit_va, &lit);
    }
    n = trap_prefill_set(vec_va, word, have_lit, lit, sp, pages);
    for (i = 0; i < n; i++) {
        rc = trap_pin(&g_sh, pages[i]);
        if (rc == SH_OK || rc == SH_SUBPAGE)
            g_prefills++;
    }
    dsb();
    isb();
}
