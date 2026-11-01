// SPDX-License-Identifier: GPL-3.0-or-later
#include "shadow.h"
#include "../common/platform.h"
#include "../common/status.h"

#define SCTLR_M 0x1u
#define SCTLR_C 0x4u
#define SCTLR_S 0x100u
#define SCTLR_R 0x200u

enum { SH_L2_FAULT = 0, SH_L2_LARGE, SH_L2_SMALL, SH_L2_TINY };
enum { W_SEC = 1, W_LARGE, W_SMALL };

struct sh_l2 {
    uint32_t kind;
    uint32_t ap[4];
    uint32_t c;
    uint32_t b;
    uint32_t base;
};

// perm code = priv * 3 + user, each 0 none, 1 read, 2 write; value is the v7 APX:AP bits
static const uint8_t v7_ap[9] = { 0, 0, 0, 5, 6, 0, 1, 2, 3 };

static int in_window(const struct sh *s, uint32_t va)
{
    return va - s->win_base < s->win_size;
}

static int sh_rd(struct sh *s, uint32_t pa, uint32_t *v)
{
    if (!s->ops.rd || s->ops.rd(s->ops.ctx, pa, v) != 0)
        return SH_E_READ;
    return SH_OK;
}

// E25: with SMP=0 and TTBR IRGN WB the A9 walk sees a stored shadow word without a clean once a dsb
// completes it, at both walk levels and for zeroing stores too; sh_fill ends in ops.barrier, other
// publishers in the caller dsb before the TLB flush
static void sh_put(struct sh *s, uint32_t *p, uint32_t w)
{
    (void)s;
    *p = w;
}

static void sh_barrier(struct sh *s)
{
    if (s->ops.barrier)
        s->ops.barrier(s->ops.ctx);
}

static void sh_put_l1(struct sh *s, uint32_t i, uint32_t w)
{
    s->touched[i >> 5] |= 1u << (i & 31u);
    sh_put(s, &s->l1[i], w);
}

static void sh_put_l2(struct sh *s, uint32_t *p, uint32_t w)
{
    uint32_t i = (uint32_t)(p - s->l2);

    s->l2_set[i >> 5] |= 1u << (i & 31u);
    sh_put(s, p, w);
}

static uint32_t sh_perm(uint32_t ap, uint32_t sctlr)
{
    switch (ap & 3u) {
    case 0:
        if (sctlr & SCTLR_R)
            return 4;
        return (sctlr & SCTLR_S) ? 3 : 0;
    case 1:
        return 6;
    case 2:
        return 7;
    default:
        return 8;
    }
}

static uint32_t perm_min(uint32_t a, uint32_t b)
{
    uint32_t pa = a / 3u, ua = a % 3u, pb = b / 3u, ub = b % 3u;
    return (pa < pb ? pa : pb) * 3u + (ua < ub ? ua : ub);
}

static uint32_t perm_ro(uint32_t p)
{
    return perm_min(p, 4);
}

_Static_assert(SH_HPAGES == (0x1000000u + PL_RAM2M_BASE + 0x200000u - PL_SRAM_BASE) >> 12, "dual map pages");

static int32_t sh_hpi(uint32_t host)
{
    if (host - PL_RAM16_BASE < 0x1000000u)
        return (int32_t)((host - PL_RAM16_BASE) >> 12);
    if (host - PL_SRAM_BASE < PL_RAM2M_BASE + 0x200000u - PL_SRAM_BASE)
        return (int32_t)(4096u + ((host - PL_SRAM_BASE) >> 12));
    return -1;
}

static int sh_wb(const struct sh *s, uint32_t c, uint32_t b)
{
    return (s->memcfg & NGST_MC_INNER_WB) && (s->g_sctlr & SCTLR_C) && c && b && !s->wt_all;
}

int sh_l2_host(uint32_t mc, uint32_t rom_len, uint32_t host)
{
    if (!(mc & NGST_MC_OUTER_WB))
        return 0;
    return host - PL_SRAM_BASE < PL_RAM2M_BASE + 0x200000u - PL_SRAM_BASE || host - PL_ROM_BASE < rom_len ||
           host - PL_RAM16_BASE < 0x1000000u;
}

uint32_t sh_l2_set(uint32_t mc, uint32_t rom_len, uint32_t *base, uint32_t *len)
{
    uint32_t n = 1;

    if (!(mc & NGST_MC_OUTER_WB))
        return 0;
    base[0] = PL_SRAM_BASE;
    len[0] = PL_RAM2M_BASE + 0x200000u - PL_SRAM_BASE;
    if (rom_len) {
        base[n] = PL_ROM_BASE;
        len[n++] = rom_len < PL_RAM16_BASE - PL_ROM_BASE ? (rom_len + 0xFFFu) & ~0xFFFu : PL_RAM16_BASE - PL_ROM_BASE;
    }
    base[n] = PL_RAM16_BASE;
    len[n++] = 0x1000000u;
    return n;
}

static int sh_outer(const struct sh *s, uint32_t c, uint32_t host)
{
    return (s->g_sctlr & SCTLR_C) && c && !s->wt_all &&
           sh_l2_host(s->memcfg, (s->pam.rom_size + 0xFFFu) & ~0xFFFu, host);
}

static int sh_dirty(const struct sh *s, uint32_t c, uint32_t b, uint32_t host)
{
    return sh_wb(s, c, b) || sh_outer(s, c, host);
}

static int sh_guarded(const struct sh *s, uint32_t host, uint32_t span)
{
    uint32_t i;

    for (i = 0; i < s->nguard; i++)
        if (s->guard[i] - host < span)
            return 1;
    return 0;
}

#if NGH_WX_HOT
static void sh_wx_keep(struct sh *s, uint32_t hp)
{
    uint32_t i;

    for (i = 0; i < s->nwxh; i++)
        if (s->wxh_hp[i] == hp) {
            s->wxh_hp[i] = s->wxh_hp[--s->nwxh];
            return;
        }
}
#endif

int sh_guard_add(struct sh *s, uint32_t host_pa)
{
    uint32_t i, len = 0x1000u;

    host_pa &= ~0xFFFu;
    if (!(s->memcfg & SH_MC_WB) || s->wt_all)
        return 0;
    for (i = 0; i < s->nguard; i++)
        if (s->guard[i] == host_pa) {
#if NGH_WX_HOT
            sh_wx_keep(s, host_pa);
#endif
            return 0;
        }
    if (s->nguard < SH_GUARD_MAX) {
        s->guard[s->nguard++] = host_pa;
    } else {
        s->wt_all = 1;
        host_pa = 0;
        len = 0;
    }
    if (s->ops.guard)
        s->ops.guard(s->ops.ctx, host_pa, len);
    s->gdue = 1;
    s->fx++;
    return 1;
}

static uint32_t sh_view(const struct sh *s, uint32_t c, uint32_t b)
{
    // classes that differ in a cache level: inner write-back, any other C=1, and C=0 once OUTER_WB puts C=1 in the L2
    if (c && b && (s->memcfg & NGST_MC_INNER_WB))
        return 1u;
    return c || !(s->memcfg & NGST_MC_OUTER_WB) ? 2u : 3u;
}

// n host pages from host get the view of this fill; a page seen two ways joins the guard set
static void sh_dual(struct sh *s, uint32_t host, uint32_t n, uint32_t c, uint32_t b)
{
    uint32_t v = sh_view(s, c, b), m, sft;
    int32_t i;

    if (!(s->memcfg & SH_MC_WB) || !(s->g_sctlr & SCTLR_C) || s->wt_all)
        return;
    for (; n; n--, host += 0x1000u) {
        i = sh_hpi(host);
        if (i < 0)
            continue;
        sft = ((uint32_t)i & 15u) * 2u;
        m = s->dmap[i >> 4] >> sft & 3u;
        if (m == v)
            continue;
        if (!m)
            s->dmap[i >> 4] |= v << sft;
        else if (sh_guard_add(s, host))
            s->dual++;
    }
}

static void sh_memtype(const struct sh *s, uint32_t c, uint32_t b, uint32_t host, uint32_t *tex, uint32_t *cbits)
{
    int wb, l2, g;

    if (!(s->g_sctlr & SCTLR_C))
        c = 0;
    // memcfg 0 keeps every cacheable guest page inner write-through (interim);
    // INNER_WB follows the guest B bit, OUTER_WB moves TEX[1:0] from 00 to 01 on guest-slot pages only
    if (c) {
        wb = sh_wb(s, c, b);
        l2 = sh_outer(s, c, host);
        // a guard page keeps today's type, inner write-through outer non-cacheable
        g = (wb || l2) && sh_guarded(s, host & ~0xFFFu, 0x1000u);
        *tex = l2 && !g ? 5u : 4u;
        *cbits = wb && !g ? 0x4u : 0x8u;
    } else {
        *tex = b ? 1u : 0u;
        *cbits = 0;
    }
}

_Static_assert(SH_HPAGES2 == SH_HPAGES + ((PL_RAM16_BASE - PL_ROM_BASE) >> 12), "mgr map pages");

static int32_t sh_hpi2(const struct sh *s, uint32_t host)
{
    uint32_t rom = (s->pam.rom_size + 0xFFFu) & ~0xFFFu;
    int32_t i = sh_hpi(host);

    if (i >= 0)
        return i;
    if (rom > PL_RAM16_BASE - PL_ROM_BASE)
        rom = PL_RAM16_BASE - PL_ROM_BASE;
    if (host - PL_ROM_BASE < rom)
        return (int32_t)(SH_HPAGES + ((host - PL_ROM_BASE) >> 12));
    return -1;
}

static int sh_bit(const uint32_t *m, int32_t i)
{
    return (int)(m[(uint32_t)i >> 5] >> ((uint32_t)i & 31u) & 1u);
}

static int sh_wx_class(const struct sh *s, uint32_t c, uint32_t b, uint32_t host, const struct pam_res *r)
{
    uint32_t tex, cbits;

    if (!sh_wx_on(s) || r->kind == PAM_FWD || r->ro || sh_hpi(host) < 0)
        return 0;
    sh_memtype(s, c, b, host, &tex, &cbits);
    return cbits == 0x4u;
}

static int sh_wx_split(const struct sh *s, uint32_t c, uint32_t b, uint32_t ro, uint32_t host, uint32_t n)
{
    int32_t j = sh_hpi2(s, host);
    uint32_t k;

    if (sh_wb(s, c, b) && !ro && sh_hpi(host) >= 0)
        return 1;
    if (j < 0)
        return 0;
    // n is 16 or 256 and the index bases keep host's alignment
    for (k = 0; k < n; k += 32u)
        if (s->wx_m[((uint32_t)j + k) >> 5] & (n < 32u ? 0xFFFFu << ((uint32_t)j & 31u) : 0xFFFFFFFFu))
            return 1;
    return 0;
}

static uint32_t *sh_slot(struct sh *s, uint32_t va)
{
    uint32_t e = s->l1[va >> 20];

    if (in_window(s, va) || (e & 3u) != 1u)
        return 0;
    return s->l2 + (((e & 0xFFFFFC00u) - s->l2_pa) >> 2) + ((va >> 12) & 0xFFu);
}

static int sh_maps(struct sh *s, uint32_t va, uint32_t hp)
{
    uint32_t *p = sh_slot(s, va);

    return p && (*p & 2u) && (*p & 0xFFFFF000u) == hp;
}

#define WXV_GEN(s) (((s)->drops & 0x3FFu) << 2)

static void sh_wx_note(struct sh *s, int32_t i, uint32_t v, uint32_t hp)
{
    uint32_t r = s->wx_va[i], g = WXV_GEN(s), key = r & ~0xFFFu, n = (r & 0xFFCu) == g ? r & 3u : 0;

    if (n == 0 || (n == 1u && (key == v || !sh_maps(s, key, hp))))
        r = v | g | 1u;
    else if (n == 1u)
        r = (key ^ v) | g | 2u;
    else if (n == 2u && !sh_maps(s, key ^ v, hp))
        r = g | 3u;
    s->wx_va[i] = r;
}

// the real core skips every AP and XN check in a Manager domain, so its pages carry full acces at the fill
static int sh_mgr(const struct sh *s, uint32_t dom)
{
    return sh_wx_on(s) && ((s->g_dacr >> (2u * dom)) & 3u) == 3u;
}

static void sh_count(struct sh *s, uint32_t dom, int mgr)
{
    if (!sh_wx_on(s))
        return;
    s->dom_live[dom]++;
    if (mgr) {
        s->live_mgr |= 1u << dom;
        s->wx_mgr++;
    } else {
        s->live_cli |= 1u << dom;
    }
}

static uint32_t sh_wx_perm(struct sh *s, const struct pam_res *r, uint32_t cbits, uint32_t perm, uint32_t va,
                           uint32_t *xn, uint32_t dom)
{
    uint32_t hp = r->host_pa & ~0xFFFu;
    int32_t i = sh_hpi(hp), j = sh_hpi2(s, hp);

    if (sh_mgr(s, dom) || (j >= 0 && sh_bit(s->wx_m, j)))
        perm = 8;
    if (i < 0 || r->ro || cbits != 0x4u)
        return perm;
    sh_wx_note(s, i, va, hp);
    if (sh_bit(s->wx_x, i))
        return perm_ro(perm);
    *xn = 1u;
    return perm;
}

static int sh_v5_l2(uint32_t d, int via_fine, struct sh_l2 *o)
{
    uint32_t i;

    o->kind = SH_L2_FAULT;
    o->c = (d >> 3) & 1u;
    o->b = (d >> 2) & 1u;
    o->base = 0;
    for (i = 0; i < 4; i++)
        o->ap[i] = (d >> (4 + 2 * i)) & 3u;
    switch (d & 3u) {
    case 1:
        o->kind = SH_L2_LARGE;
        o->base = d & 0xFFFF0000u;
        break;
    case 2:
        o->kind = SH_L2_SMALL;
        o->base = d & 0xFFFFF000u;
        break;
    case 3:
        if (via_fine)
            return SH_E_TINY;
        break;
    default:
        break;
    }
    return SH_OK;
}

static int sh_walk(struct sh *s, uint32_t va, struct sh_walk *w)
{
    struct sh_l2 l;
    uint32_t d, i;
    int rc;

    rc = sh_rd(s, (s->g_ttb & 0xFFFFC000u) | ((va >> 20) << 2), &d);
    if (rc)
        return rc;
    if ((d & 3u) == 0) {
        w->fsr = 0x5;
        return SH_FAULT;
    }
    if ((d & 3u) == 3u)
        return SH_E_FINE;
    w->dom = (d >> 5) & 0xFu;
    if (w->dom == s->mon_dom)
        return SH_E_DOMAIN;
    if ((d & 3u) == 2u) {
        w->kind = W_SEC;
        w->base = d & 0xFFF00000u;
        w->c = (d >> 3) & 1u;
        w->b = (d >> 2) & 1u;
        for (i = 0; i < 4; i++)
            w->ap[i] = (d >> 10) & 3u;
        return SH_OK;
    }
    rc = sh_rd(s, (d & 0xFFFFFC00u) | (((va >> 12) & 0xFFu) << 2), &d);
    if (rc)
        return rc;
    rc = sh_v5_l2(d, 0, &l);
    if (rc)
        return rc;
    if (l.kind == SH_L2_FAULT) {
        w->fsr = (w->dom << 4) | 0x7u;
        return SH_FAULT;
    }
    w->kind = l.kind == SH_L2_LARGE ? W_LARGE : W_SMALL;
    w->base = l.base;
    w->c = l.c;
    w->b = l.b;
    for (i = 0; i < 4; i++)
        w->ap[i] = l.ap[i];
    return SH_OK;
}

static uint32_t sh_perm_all(const struct sh *s, const struct sh_walk *w, int *unequal)
{
    uint32_t p = sh_perm(w->ap[0], s->g_sctlr);
    uint32_t i;

    *unequal = 0;
    for (i = 1; i < 4; i++) {
        uint32_t q = sh_perm(w->ap[i], s->g_sctlr);
        if (q != p)
            *unequal = 1;
        p = perm_min(p, q);
    }
    return p;
}

static int sh_l2_get(struct sh *s, uint32_t va, uint32_t dom, uint32_t **t)
{
    uint32_t i1 = va >> 20;
    uint32_t cur = s->l1[i1];
    uint32_t idx, word, i, b;

    if ((cur & 3u) == 1u) {
        idx = ((cur & 0xFFFFFC00u) - s->l2_pa) >> 10;
    } else {
        if (s->l2_used >= s->n_l2)
            return SH_E_POOL;
        idx = s->l2_used++;
        s->l2_sec[idx >> 5] &= ~(1u << (idx & 31u));
        for (i = idx * 8u; i < idx * 8u + 8u; i++) {
            for (b = s->l2_set[i]; b; b &= b - 1u)
                s->l2[i * 32u + (uint32_t)__builtin_ctz(b)] = 0;
            s->l2_set[i] = 0;
        }
    }
    word = (s->l2_pa + (idx << 10)) | (dom << 5) | 1u;
    if (word != cur)
        sh_put_l1(s, i1, word);
    *t = s->l2 + idx * 256u;
    return SH_OK;
}

static int sh_small(struct sh *s, uint32_t *slot, uint32_t pa, uint32_t perm, uint32_t c, uint32_t b, uint32_t va,
                    uint32_t dom)
{
    struct pam_res r = pamap_page(pa, 0, &s->pam);
    uint32_t tex, cbits, ap, xn = 0;

    if (r.kind == PAM_FWD) {
        sh_put_l2(s, slot, 0);
        return SH_FWD;
    }
    if (r.tail_fill && s->ops.tail) {
        s->ops.tail(s->ops.ctx, r.host_pa + 0x1000u - r.tail_fill, r.tail_fill);
        s->fx++;
    }
    sh_dual(s, r.host_pa & ~0xFFFu, 1u, c, b);
    sh_memtype(s, c, b, r.host_pa, &tex, &cbits);
    if (sh_wx_on(s))
        perm = sh_wx_perm(s, &r, cbits, perm, va, &xn, dom);
    // after the Manager and widening rights: a store to ROM is a flash comand and must keep trapping
    if (r.ro)
        perm = perm_ro(perm);
    ap = v7_ap[perm];
    sh_put_l2(s, slot, r.host_pa | ((ap >> 2) << 9) | (tex << 6) | ((ap & 3u) << 4) | cbits | 2u | xn);
    sh_count(s, dom, sh_mgr(s, dom));
    return SH_OK;
}

static int sh_fill_large(struct sh *s, uint32_t va, const struct sh_walk *w, uint32_t *t, uint32_t perm)
{
    struct pam_res r[16];
    uint32_t g0 = ((va >> 12) & 0xFFu) & ~15u;
    uint32_t i, tex, cbits, ap, word;
    int merge = 1, rc = SH_OK;

    for (i = 0; i < 16; i++)
        r[i] = pamap_page(w->base + i * 0x1000u, 0, &s->pam);
    for (i = 0; i < 16; i++)
        if (r[i].kind == PAM_FWD || r[i].kind != r[0].kind || r[i].ro != r[0].ro || r[i].tail_fill ||
            r[i].host_pa != r[0].host_pa + i * 0x1000u)
            merge = 0;
    if (merge && (r[0].host_pa & 0xFFFFu))
        merge = 0;
    // write-back RAM pages and Manager pages keep their own 4 KB words
    if (merge && sh_wx_on(s) && sh_wx_split(s, w->c, w->b, r[0].ro, r[0].host_pa, 16u))
        merge = 0;
    if (merge) {
        sh_dual(s, r[0].host_pa, 16u, w->c, w->b);
        if (sh_dirty(s, w->c, w->b, r[0].host_pa) && sh_guarded(s, r[0].host_pa, 0x10000u))
            merge = 0;
    }
    if (merge) {
        if (sh_mgr(s, w->dom))
            perm = 8;
        if (r[0].ro)
            perm = perm_ro(perm);
        sh_memtype(s, w->c, w->b, r[0].host_pa, &tex, &cbits);
        ap = v7_ap[perm];
        word = r[0].host_pa | (tex << 12) | ((ap >> 2) << 9) | ((ap & 3u) << 4) | cbits | 1u;
        for (i = 0; i < 16; i++)
            sh_put_l2(s, &t[g0 + i], word);
        sh_count(s, w->dom, sh_mgr(s, w->dom));
        s->f_large++;
        return SH_OK;
    }
    for (i = 0; i < 16; i++) {
        int one = sh_small(s, &t[g0 + i], w->base + i * 0x1000u, perm, w->c, w->b,
                           (va & 0xFFF00000u) | ((g0 + i) << 12), w->dom);
        if (g0 + i == ((va >> 12) & 0xFFu))
            rc = one;
    }
    if (rc == SH_OK)
        s->f_small++;
    return rc;
}

int sh_init(struct sh *s, uint32_t *l1, uint32_t *l2, uint32_t l2_pa, uint32_t n_l2,
            uint32_t win_base, uint32_t win_size, uint32_t mon_dom)
{
    uint32_t i;

    if (!l1 || !l2 || (l2_pa & 0x3FFu) || ((win_base | win_size) & 0xFFFFFu) || mon_dom > 15u)
        return SH_E_ARG;
    s->l1 = l1;
    s->l2 = l2;
    s->l2_pa = l2_pa;
    s->n_l2 = n_l2;
    s->l2_used = 0;
    s->win_base = win_base;
    s->win_size = win_size;
    s->mon_dom = mon_dom;
    s->pam.rom_size = 0;
    s->pam.flash_array[0] = s->pam.flash_array[1] = s->pam.flash_array[2] = 0;
    s->pam.profile = PAM_PROFILE_NORMAL;
    s->ops.ctx = 0;
    s->ops.rd = 0;
    s->ops.rd_line = 0;
    s->ops.page_va = 0;
    s->ops.inv_line = 0;
    s->ops.rd_raw = 0;
    s->ops.tail = 0;
    s->ops.clean = 0;
    s->ops.barrier = 0;
    s->ops.guard = 0;
    s->ops.code_sync = 0;
    s->ops.tlb_mva = 0;
    s->g_ttb = 0;
    s->g_sctlr = 0;
    s->g_dacr = 0;
    s->memcfg = 0;
    s->npins = 0;
    for (i = 0; i < SH_POOL_MAX / 32u; i++)
        s->l2_sec[i] = 0;
    for (i = 0; i < 128u; i++)
        s->touched[i] = 0;
    // the pool holds junk until each table is zeroed in full once
    for (i = 0; i < SH_POOL_MAX * 8u; i++)
        s->l2_set[i] = 0xFFFFFFFFu;
    s->drops = s->drop_mva = s->drop_sec = s->rewalks = 0;
    s->f_sec = s->f_large = s->f_small = 0;
    s->nguard = s->wt_all = s->gdue = s->dual = 0;
    s->fx = 0;
    for (i = 0; i < SH_HPAGES / 16u; i++)
        s->dmap[i] = 0;
    for (i = 0; i < 4096; i++)
        l1[i] = 0;
    sh_wx_reset(s);
    return SH_OK;
}

int sh_window_set(struct sh *s, uint32_t va, uint32_t word)
{
    if (!in_window(s, va))
        return SH_E_WINDOW;
    sh_put(s, &s->l1[va >> 20], word);
    return SH_OK;
}

uint32_t sh_real_dacr(const struct sh *s, uint32_t guest_dacr)
{
    uint32_t sft = 2u * s->mon_dom;

    // the A9 skips AP and XN checks in a Manager domain, so every one runs as Client
    if (sh_wx_on(s))
        guest_dacr &= ~((guest_dacr & (guest_dacr >> 1) & 0x55555555u) << 1);
    return (guest_dacr & ~(3u << sft)) | (1u << sft);
}

// live domains holding a word of the other class under v (Client under 11, Manager under 01 or 10); 00 needs
// none, the host DACR faults it; from the classes alone, never from the old g_dacr, which the caller may have synced
uint32_t sh_set_dacr(struct sh *s, uint32_t v)
{
    uint32_t d, nd, cls, mask = 0;

    for (d = 0; d < 16u; d++) {
        nd = (v >> (2u * d)) & 3u;
        cls = nd == 3u ? s->live_cli : nd ? s->live_mgr : 0u;
        if (s->dom_live[d] && ((cls >> d) & 1u))
            mask |= 1u << d;
    }
    // every CP15 trap lands here: count real changes only, so a pending mask counts once
    if (v != s->g_dacr) {
        s->dacr_writes++;
        s->dacr_mdom |= mask;
        for (d = 0; d < 16u; d++) {
            if (!s->dom_live[d])
                continue;
            nd = (v >> (2u * d)) & 3u;
            if ((mask >> d) & 1u) {
                if (nd == 3u)
                    s->dacr_c2m++;
                else
                    s->dacr_m2c++;
                s->dacr_live += s->dom_live[d];
            } else if (nd == 0u && ((s->live_mgr >> d) & 1u)) {
                s->dacr_skip++;
                s->dacr_sdom |= 1u << d;
            }
            if (nd == 2u)
                s->dacr_rsv++;
        }
    }
    s->g_dacr = v;
    return mask;
}

static int sh_fill_w(struct sh *s, uint32_t va, uint32_t *fsr, struct sh_walk *w)
{
    struct pam_res r;
    uint32_t *t;
    uint32_t perm, tex, cbits, ap, off, hostbase, idx;
    int unequal, rc;

    s->fx++;
    if (in_window(s, va))
        return SH_E_WINDOW;
    rc = sh_walk(s, va, w);
    if (rc == SH_FAULT && fsr)
        *fsr = w->fsr;
    if (rc)
        return rc;
    perm = sh_perm_all(s, w, &unequal);

    if (w->kind == W_SEC) {
        off = va & 0xFFFFFu;
        r = pamap_lookup(w->base | off, 0, &s->pam);
        hostbase = r.host_pa - off;
        if (r.uniform_1m && r.kind == PAM_FWD)
            return SH_FWD;
        if (r.uniform_1m && !(hostbase & 0xFFFFFu))
            sh_dual(s, hostbase, 256u, w->c, w->b);
        // a host MB that holds a guard page goes page by page; one with a write-back RAM or Manager page
        if (r.uniform_1m && !(hostbase & 0xFFFFFu) &&
            !(sh_dirty(s, w->c, w->b, hostbase) && sh_guarded(s, hostbase, 0x100000u)) &&
            !(sh_wx_on(s) && sh_wx_split(s, w->c, w->b, r.ro, hostbase, 256u))) {
            if (sh_mgr(s, w->dom))
                perm = 8;
            if (r.ro)
                perm = perm_ro(perm);
            sh_memtype(s, w->c, w->b, hostbase, &tex, &cbits);
            ap = v7_ap[perm];
            sh_put_l1(s, va >> 20, hostbase | ((ap >> 2) << 15) | (tex << 12) | ((ap & 3u) << 10) |
                      (w->dom << 5) | cbits | 2u);
            sh_count(s, w->dom, sh_mgr(s, w->dom));
            s->f_sec++;
            sh_barrier(s);
            return SH_OK;
        }
    }
    rc = sh_l2_get(s, va, w->dom, &t);
    if (rc)
        return rc;
    if (w->kind == W_LARGE) {
        rc = sh_fill_large(s, va, w, t, perm);
    } else {
        if (w->kind == W_SMALL) {
            rc = sh_small(s, &t[(va >> 12) & 0xFFu], w->base, perm, w->c, w->b, va & ~0xFFFu, w->dom);
        } else {
            idx = (uint32_t)(t - s->l2) >> 8;
            s->l2_sec[idx >> 5] |= 1u << (idx & 31u);
            rc = sh_small(s, &t[(va >> 12) & 0xFFu], w->base | (va & 0xFF000u), perm, w->c, w->b, va & ~0xFFFu,
                          w->dom);
        }
        if (rc == SH_OK)
            s->f_small++;
    }
    sh_barrier(s);
    if (rc == SH_OK && unequal)
        rc = SH_SUBPAGE;
    return rc;
}

int sh_fill(struct sh *s, uint32_t va, uint32_t *fsr)
{
    struct sh_walk w;

    return sh_fill_w(s, va, fsr, &w);
}

static void sh_rewalk(struct sh *s)
{
    uint32_t i;
    s->rewalks += s->npins;
    for (i = 0; i < s->npins; i++)
        (void)sh_fill(s, s->pins[i], 0);
}

void sh_drop_all(struct sh *s)
{
    uint32_t w, b, i;

    s->drops++;
    s->fx++;
    // before the pin refills of sh_rewalk, which count themselves back in
    for (i = 0; i < 16u; i++)
        s->dom_live[i] = 0;
    s->live_mgr = s->live_cli = 0;
    // every nonzero non-window L1 word has its touched bit (set by sh_put_l1), so only marked words change
    for (w = 0; w < 128u; w++) {
        for (b = s->touched[w]; b; b &= b - 1u)
            s->l1[w * 32u + (uint32_t)__builtin_ctz(b)] = 0;
        s->touched[w] = 0;
    }
    s->l2_used = 0;
    for (i = 0; i < SH_POOL_MAX / 32u; i++)
        s->l2_sec[i] = 0;
    sh_rewalk(s);
}

int32_t sh_check_marked(const struct sh *s)
{
    uint32_t i;

    for (i = 0; i < 4096u; i++)
        if (s->l1[i] && !in_window(s, i << 20) && !((s->touched[i >> 5] >> (i & 31u)) & 1u))
            return (int32_t)i;
    return -1;
}

void sh_fill_all(struct sh *s, uint32_t reserve, struct sh_fill_stats *st)
{
    uint32_t i, p, d;
    int rc;

    st->pages = st->fwd = st->refused = st->stop = 0;
    for (i = 0; i < 4096u; i++) {
        if (in_window(s, i << 20))
            continue;
        if (sh_rd(s, (s->g_ttb & 0xFFFFC000u) | (i << 2), &d)) {
            st->refused++;
            continue;
        }
        if ((d & 3u) == 0)
            continue;
        if ((s->l1[i] & 3u) != 1u && s->l2_used + reserve >= s->n_l2) {
            st->stop = 1;
            return;
        }
        for (p = 0; p < 256u; p++) {
            rc = sh_fill(s, (i << 20) | (p << 12), 0);
            if (rc == SH_OK || rc == SH_SUBPAGE) {
                st->pages += (s->l1[i] & 3u) == 2u ? 256u : 1u;
            } else if (rc == SH_FWD) {
                st->fwd++;
            } else if (rc < 0) {
                st->refused++;
                break;
            }
            // a guest section ends here unless sh_fill split it into pages (non-uniform: one page per call)
            if ((d & 3u) == 2u && (s->l1[i] & 3u) != 1u)
                break;
        }
    }
}

void sh_drop_mva(struct sh *s, uint32_t va)
{
    uint32_t cur = s->l1[va >> 20];
    uint32_t *t, idx, i, k;

    s->drop_mva++;
    s->fx++;
    if (!in_window(s, va)) {
        if ((cur & 3u) == 2u) {
            sh_put(s, &s->l1[va >> 20], 0);
        } else if ((cur & 3u) == 1u) {
            k = ((cur & 0xFFFFFC00u) - s->l2_pa) >> 10;
            t = s->l2 + k * 256u;
            idx = (va >> 12) & 0xFFu;
            if (s->l2_sec[k >> 5] >> (k & 31u) & 1u) {
                // a guest section is one TLB entry: drop all its pages; the L1 keeps the table so sh_l2_get reuses it
                s->drop_sec++;
                for (i = 0; i < 256; i++)
                    t[i] = 0;
            } else if ((t[idx] & 3u) == 1u) {
                for (i = 0; i < 16; i++)
                    sh_put(s, &t[(idx & ~15u) + i], 0);
            } else {
                sh_put(s, &t[idx], 0);
            }
        }
    }
    sh_rewalk(s);
}

int sh_pin(struct sh *s, uint32_t va)
{
    uint32_t i;

    va &= ~0xFFFu;
    if (in_window(s, va))
        return SH_E_WINDOW;
    for (i = 0; i < s->npins; i++)
        if (s->pins[i] == va)
            return sh_fill(s, va, 0);
    if (s->npins >= SH_PIN_MAX)
        return SH_E_PINFULL;
    s->pins[s->npins++] = va;
    return sh_fill(s, va, 0);
}

void sh_unpin_all(struct sh *s)
{
    s->npins = 0;
}

static int32_t sh_check(const struct sh *s, const struct sh_walk *w, int rc, uint32_t va, int write, int user)
{
    uint32_t dbits, ap, sub;
    int sec, fault;

    if (rc == SH_FAULT)
        return (int32_t)w->fsr;
    if (rc)
        return rc;
    sec = w->kind == W_SEC;
    dbits = (s->g_dacr >> (2u * w->dom)) & 3u;
    if (dbits == 0)
        return (int32_t)((w->dom << 4) | (sec ? 0x9u : 0xBu));
    if (dbits == 3)
        return 0;
    sub = w->kind == W_LARGE ? (va >> 14) & 3u : (va >> 10) & 3u;
    ap = w->ap[sub];
    switch (ap) {
    case 0:
        fault = !(((s->g_sctlr & SCTLR_S) && !user && !write) || ((s->g_sctlr & SCTLR_R) && !write));
        break;
    case 1:
        fault = user;
        break;
    case 2:
        fault = user && write;
        break;
    default:
        fault = 0;
        break;
    }
    if (fault)
        return (int32_t)((w->dom << 4) | (sec ? 0xDu : 0xFu));
    return 0;
}

static uint32_t sh_pa(const struct sh_walk *w, uint32_t va)
{
    if (w->kind == W_SEC)
        return w->base | (va & 0xFFFFFu);
    if (w->kind == W_LARGE)
        return w->base | (va & 0xFFFFu);
    return w->base | (va & 0xFFFu);
}

int32_t sh_access_check(struct sh *s, uint32_t va, int write, int user)
{
    struct sh_walk w;
    int rc = sh_walk(s, va, &w);

    return sh_check(s, &w, rc, va, write, user);
}

int sh_va2pa(struct sh *s, uint32_t va, uint32_t *pa)
{
    struct sh_walk w;
    int rc;

    if (in_window(s, va))
        return SH_E_WINDOW;
    rc = sh_walk(s, va, &w);
    if (rc)
        return rc;
    *pa = sh_pa(&w, va);
    return SH_OK;
}

int sh_fill_hand(struct sh *s, uint32_t va, uint32_t *fsr, struct sh_hand *h)
{
    uint32_t fx = s->fx + 1u;
    int rc = sh_fill_w(s, va, fsr, &h->w);

    // the fill counts itself once: a ROM tail write or a guard join after its walk voids the hand
    h->ok = rc == SH_FWD && s->fx == fx;
    h->va = va;
    h->ttb = s->g_ttb;
    h->fx = fx;
    return rc;
}

int32_t sh_access_pa(struct sh *s, struct sh_hand *h, uint32_t va, int write, int user, uint32_t *pa)
{
    struct sh_walk w;
    const struct sh_walk *p = &w;
    int32_t chk;
    int rc = SH_OK;

    if (h && h->ok && !((h->va ^ va) >> 12) && h->ttb == s->g_ttb && h->fx == s->fx)
        p = &h->w;
    else
        rc = sh_walk(s, va, &w);
    if (h)
        h->ok = 0;
    chk = sh_check(s, p, rc, va, write, user);
    if (chk)
        return chk;
    if (in_window(s, va))
        return SH_E_WINDOW;
    *pa = sh_pa(p, va);
    return SH_OK;
}

// a guest mapping inside the window is never faulted on: guest accesses would reach monitor sections
int sh_window_guest(struct sh *s, uint32_t *va, uint32_t *l1e)
{
    uint32_t m, e, pa, v[8], line = 1u, nok = 0, k, mend, lim, pg = 1u, base = 0;

    if (s->ops.page_va && s->ops.inv_line && s->ops.rd_raw && s->ops.barrier) {
        // descriptors m..mend-1 as the loop below visits them: one lookup per 4 KB page (pamap maps a page as a
        // unit, so a line keeps its offset in the page alias), one invalidate per line, one barrier for all
        m = s->win_base >> 20;
        mend = m + (s->win_size >> 20) < 4096u ? m + (s->win_size >> 20) : 4096u;
        for (k = m; k < mend; k = (k | 7u) + 1u) {
            pa = ((s->g_ttb & 0xFFFFC000u) + k * 4u) & ~31u;
            if ((pa & ~0xFFFu) != pg) {
                if (s->ops.page_va(s->ops.ctx, pa & ~0xFFFu, &base) != 0)
                    break;
                pg = pa & ~0xFFFu;
            }
            s->gline[nok] = base + (pa & 0xFE0u);
            s->ops.inv_line(s->ops.ctx, s->gline[nok]);
            nok++;
        }
        sh_barrier(s);
        // SH_E_READ at the first line that failed above unless an earlier line hits
        for (k = 0; m < mend; k++) {
            if (k == nok)
                return SH_E_READ;
            s->ops.rd_raw(s->ops.ctx, s->gline[k], v);
            lim = (m | 7u) + 1u < mend ? (m | 7u) + 1u : mend;
            for (; m < lim; m++) {
                if (v[m & 7u] & 3u) {
                    *va = m << 20;
                    *l1e = v[m & 7u];
                    return 1;
                }
            }
        }
        return 0;
    }
    for (m = s->win_base >> 20; m < 4096u && (m << 20) - s->win_base < s->win_size; m++) {
        pa = (s->g_ttb & 0xFFFFC000u) + m * 4u;
        if (s->ops.rd_line) {
            if ((pa & ~31u) != line) {
                line = pa & ~31u;
                if (s->ops.rd_line(s->ops.ctx, pa, v) != 0)
                    return SH_E_READ;
            }
            e = v[(pa >> 2) & 7u];
        } else if (sh_rd(s, pa, &e) != SH_OK) {
            return SH_E_READ;
        }
        if (e & 3u) {
            *va = m << 20;
            *l1e = e;
            return 1;
        }
    }
    return 0;
}

// a guest C or M toggle cleans and invalidates the L1, so the views seen before it no longer matter
void sh_set_sctlr(struct sh *s, uint32_t sctlr)
{
    uint32_t i;

    if ((s->memcfg & SH_MC_WB) && ((s->g_sctlr ^ sctlr) & (SCTLR_C | SCTLR_M)))
        for (i = 0; i < SH_HPAGES / 16u; i++)
            s->dmap[i] = 0;
    s->g_sctlr = sctlr;
}

enum { WX_EXEC, WX_DATA, WX_MGR };

static int sh_wx_page(struct sh *s, uint32_t va, struct sh_walk *w, uint32_t *pa, struct pam_res *r)
{
    if (in_window(s, va) || sh_walk(s, va, w) != SH_OK)
        return 0;
    *pa = w->kind == W_SEC ? w->base | (va & 0xFF000u) : w->kind == W_LARGE ? w->base | (va & 0xF000u) : w->base;
    *r = pamap_page(*pa, 0, &s->pam);
    return 1;
}

static int sh_live(struct sh *s, uint32_t va)
{
    uint32_t *p = sh_slot(s, va);

    return !in_window(s, va) && ((s->l1[va >> 20] & 3u) == 2u || (p && *p));
}

static uint32_t sh_re_scan(struct sh *s, uint32_t mask)
{
    uint32_t w, b, i, e, k, j, x, slot, grp, n = 0;
    const uint32_t *t;

    for (w = 0; w < 128u; w++) {
        for (b = s->touched[w]; b; b &= b - 1u) {
            i = w * 32u + (uint32_t)__builtin_ctz(b);
            e = s->l1[i];
            if (in_window(s, i << 20) || (e & 3u) - 1u > 1u || !((mask >> ((e >> 5) & 15u)) & 1u))
                continue;
            if ((e & 3u) == 2u) {
                if (n == SH_RE_MAX)
                    return n + 1u;
                s->re_va[n++] = i << 20;
                continue;
            }
            k = ((e & 0xFFFFFC00u) - s->l2_pa) >> 10;
            t = s->l2 + k * 256u;
            grp = 16u;
            for (j = 0; j < 8u; j++) {
                for (x = s->l2_set[k * 8u + j]; x; x &= x - 1u) {
                    slot = j * 32u + (uint32_t)__builtin_ctz(x);
                    // a large word fills its 16 slots: one entry, collected at the first
                    if (!t[slot] || ((t[slot] & 3u) == 1u && slot >> 4 == grp))
                        continue;
                    if ((t[slot] & 3u) == 1u)
                        grp = slot >> 4;
                    if (n == SH_RE_MAX)
                        return n + 1u;
                    s->re_va[n++] = (i << 20) | (slot << 12);
                }
            }
        }
    }
    return n;
}

uint32_t sh_reemit(struct sh *s, uint32_t mask)
{
    uint32_t d, n = 0, i, k, va, *p;

    for (d = 0; d < 16u; d++)
        if ((mask >> d) & 1u)
            n += s->dom_live[d];
    // collect first: past the bound nothing is writen and the caller drops all
    if (n > SH_RE_MAX || (n = sh_re_scan(s, mask)) > SH_RE_MAX)
        return mask;
    for (d = 0; d < 16u; d++)
        if ((mask >> d) & 1u)
            s->dom_live[d] = 0;
    s->live_mgr &= ~mask;
    s->live_cli &= ~mask;
    if (!n)
        return 0;
    s->fx++;
    for (i = 0; i < n; i++) {
        va = s->re_va[i];
        if ((s->l1[va >> 20] & 3u) == 2u) {
            sh_put(s, &s->l1[va >> 20], 0);
            continue;
        }
        p = s->l2 + (((s->l1[va >> 20] & 0xFFFFFC00u) - s->l2_pa) >> 2) + ((va >> 12) & 0xF0u);
        k = (va >> 12) & 15u;
        if ((p[k] & 3u) == 1u)
            for (k = 0; k < 16u; k++)
                sh_put(s, &p[k], 0);
        else
            sh_put(s, &p[k], 0);
    }
    // sh_fill builds each word under the new g_dacr, as a drop-all and a refault would; a large refill may cover
    // a later VA already
    for (i = 0; i < n; i++)
        if (!sh_live(s, s->re_va[i]))
            (void)sh_fill(s, s->re_va[i], 0);
    s->reemits++;
    s->reemit_n += n;
    // m_tlb_mva is the whole TLB, so one op covers every changed entry
    if (s->ops.tlb_mva)
        s->ops.tlb_mva(s->ops.ctx, SH_WX_TLB_ALL);
    return 0;
}

static int sh_wx_put(struct sh *s, uint32_t va, uint32_t hp, const struct sh_walk *w, uint32_t pa)
{
    struct sh_walk nw;
    struct pam_res r;
    uint32_t *p = sh_slot(s, va), old, perm, live, mgr;
    int uneq;

    // 0: no live small entry for hp at va; 1: the same word again; 2: a new word, TLB entry dropped; -1: the guest
    // walk no longer gives hp
    if (!p || !(*p & 2u) || (*p & 0xFFFFF000u) != hp)
        return 0;
    if (!w) {
        if (!sh_wx_page(s, va, &nw, &pa, &r) || r.kind == PAM_FWD || (r.host_pa & ~0xFFFu) != hp)
            return -1;
        w = &nw;
    }
    old = *p;
    perm = sh_perm_all(s, w, &uneq);
    // a flip re-emits a live entry, not a new one: its counts stay
    live = s->dom_live[w->dom];
    mgr = s->wx_mgr;
    if (sh_small(s, p, pa, perm, w->c, w->b, va, w->dom) != SH_OK)
        return -1;
    s->dom_live[w->dom] = live;
    s->wx_mgr = mgr;
    if (*p == old)
        return 1;
    if (s->ops.tlb_mva)
        s->ops.tlb_mva(s->ops.ctx, va);
    return 2;
}

#if NGH_WX_HOT
static int sh_wx_hot(struct sh *s, int32_t i)
{
#if NGH_WX_OSD
    if (!s->wx_osd)
        return 0;
#endif
    if (s->wxh_n[i] >= SH_WX_HOT_K || ++s->wxh_n[i] < SH_WX_HOT_K)
        return 0;
    // at either cap the page stays W^X: a join never takes the slots the DMA edges need nor turns on wt_all
    return s->nwxh < SH_WX_HOT_MAX && s->nguard < SH_GUARD_MAX;
}
#endif

static int sh_wx_flip(struct sh *s, uint32_t va, uint32_t to)
{
    struct sh_walk w;
    struct pam_res r;
    uint32_t pa, hp, rec, n = 0, o = 0;
    int32_t i, j;
    int a = 0, b = 0, fresh = 1, drop;
#if NGH_WX_HOT
    int hot;
#endif

    va &= ~0xFFFu;
    if (!sh_wx_on(s) || !sh_wx_page(s, va, &w, &pa, &r) || r.kind == PAM_FWD)
        return SH_WX_NOOP;
    hp = r.host_pa & ~0xFFFu;
    i = sh_hpi(hp);
    if (to == WX_MGR) {
        j = sh_hpi2(s, hp);
        // only a right the guest has by Manager alone widens; a Client domain keeps HEAD's path, and a ROM page
        // stays AP 6 in every domain (its store is a flash command), so its fault goes to emulate at once
        if (((s->g_dacr >> (2u * w.dom)) & 3u) != 3u || j < 0 || r.ro)
            return SH_WX_NOOP;
        fresh = !sh_bit(s->wx_m, j);
        s->wx_m[j >> 5] |= 1u << (j & 31);
    } else {
        if (!sh_wx_class(s, w.c, w.b, hp, &r) || sh_bit(s->wx_x, i) == (to == WX_EXEC))
            return SH_WX_NOOP;
#if NGH_WX_HOT
        hot = sh_wx_hot(s, i);
        // a write flip leaves I lines of the exec state, and a joined page gets no later code_sync
        if (hot && to != WX_EXEC && s->ops.code_sync)
            s->ops.code_sync(s->ops.ctx, hp, 0x1000u);
#endif
        // the page's dirty lines reach memory and the I side forgets the page before any entry turns executable
        if (to == WX_EXEC && s->ops.code_sync)
            s->ops.code_sync(s->ops.ctx, hp, 0x1000u);
        s->wx_x[i >> 5] ^= 1u << (i & 31);
        if (to == WX_EXEC) {
            s->wx_exec++;
            s->wx_pages++;
        } else {
            s->wx_write++;
            s->wx_pages--;
        }
#if NGH_WX_HOT
        if (hot && sh_guard_add(s, hp))
            s->wxh_hp[s->nwxh++] = hp;
#endif
    }
    if (i >= 0) {
        rec = s->wx_va[i];
        n = (rec & 0xFFCu) == WXV_GEN(s) ? rec & 3u : 0;
        o = n == 2u ? (rec & ~0xFFFu) ^ va : rec & ~0xFFFu;
    }
    if (n == 3u) {
        drop = 1;
    } else {
        a = sh_wx_put(s, va, hp, &w, pa);
        if (a >= 0 && n && o != va)
            b = sh_wx_put(s, o, hp, 0, 0);
        // a section or large word over the page, or an entry the guest walk no longer explains
        drop = a < 0 || b < 0 || (a == 0 && sh_live(s, va));
    }
    if (drop) {
        s->wx_multi++;
        sh_drop_all(s);
        if (s->ops.tlb_mva)
            s->ops.tlb_mva(s->ops.ctx, SH_WX_TLB_ALL);
    }
    if (to == WX_MGR) {
        if (!fresh && !drop && a != 2 && b != 2)
            return SH_WX_NOOP;
        s->wx_widen++;
    }
    return SH_WX_DONE;
}

int sh_wx_exec(struct sh *s, uint32_t va)
{
    return sh_wx_flip(s, va, WX_EXEC);
}

int sh_wx_write(struct sh *s, uint32_t va)
{
    return sh_wx_flip(s, va, WX_DATA);
}

int sh_wx_widen(struct sh *s, uint32_t va)
{
    return sh_wx_flip(s, va, WX_MGR);
}

int sh_wx_exec_range(const struct sh *s, uint32_t host, uint32_t len)
{
    uint32_t a = host & ~0xFFFu, e = (host + len - 1u) & ~0xFFFu;
    int32_t i;

    if (!len)
        return 0;
    for (;; a += 0x1000u) {
        i = sh_hpi(a);
        if (i >= 0 && sh_bit(s->wx_x, i))
            return 1;
        if (a == e)
            return 0;
    }
}

void sh_wx_reset(struct sh *s)
{
    uint32_t i;
#if NGH_WX_HOT
    uint32_t j, k;
#endif

    for (i = 0; i < SH_HPAGES / 32u; i++)
        s->wx_x[i] = 0;
    for (i = 0; i < SH_HPAGES2 / 32u; i++)
        s->wx_m[i] = 0;
    for (i = 0; i < SH_HPAGES; i++)
        s->wx_va[i] = 0;
    s->wx_exec = s->wx_write = s->wx_pages = s->wx_mgr = s->wx_widen = s->wx_multi = 0;
    for (i = 0; i < 16u; i++)
        s->dom_live[i] = 0;
    s->live_mgr = s->live_cli = s->drop_dacr = s->dacr_writes = 0;
    s->dacr_m2c = s->dacr_c2m = s->dacr_skip = s->dacr_rsv = s->dacr_live = s->dacr_mdom = s->dacr_sdom = 0;
    s->reemits = s->reemit_n = 0;
#if NGH_WX_HOT
    for (i = 0; i < SH_HPAGES; i++)
        s->wxh_n[i] = 0;
    // sh_init empties the guard set before it gets here, so nwxh needs no init of its own
    for (i = j = 0; i < s->nguard; i++) {
        for (k = 0; k < s->nwxh && s->wxh_hp[k] != s->guard[i]; k++)
            ;
        if (k == s->nwxh)
            s->guard[j++] = s->guard[i];
    }
    s->nguard = j;
    s->nwxh = 0;
#endif
}

static int sh_host_of(struct sh *s, uint32_t va, uint32_t *h)
{
    uint32_t e = s->l1[va >> 20], *p = sh_slot(s, va);

    if (in_window(s, va))
        return 0;
    if ((e & 3u) == 2u) {
        *h = (e & 0xFFF00000u) | (va & 0xFF000u);
        return 1;
    }
    if (!p || !(*p & 3u))
        return 0;
    *h = (*p & 2u) ? *p & 0xFFFFF000u : (*p & 0xFFFF0000u) | (va & 0xF000u);
    return 1;
}

int sh_wx_own(struct sh *s, uint32_t pc, uint32_t va)
{
    uint32_t a, b;

    return sh_host_of(s, pc, &a) && sh_host_of(s, va, &b) && a == b;
}
