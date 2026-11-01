// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_SHADOW_H
#define NGH_SHADOW_H

#include <stdint.h>
#include "pamap.h"
#include "../common/status.h"

#define SH_PIN_MAX 16u
#define SH_POOL_MAX 256u
#define SH_GUARD_MAX 64u
// a drop-all with its refaults costs about 32-40 refills, so a flip re-emits up to this many entries
#define SH_RE_MAX 32u
// either write-back stage keeps a guard set and a DUAL map
#define SH_MC_WB (NGST_MC_INNER_WB | NGST_MC_OUTER_WB)
#define SH_L2_NSET 3u
// host RAM16, then SRAM up to the end of RAM2M
#define SH_HPAGES 5120u
// the mgr bitmap also covers the ROM slot pages after the RAM pages
#define SH_HPAGES2 (SH_HPAGES + 8192u)
#ifndef NGH_WX
#define NGH_WX 1
#endif
// inner write-back stays at the limiter arm, W^X goes off until the next guest boot
#ifndef NGH_PLAY_WB
#define NGH_PLAY_WB 0
#endif
#ifndef NGH_WX_OSD
#define NGH_WX_OSD 0
#endif
#ifndef NGH_WX_HOT
#define NGH_WX_HOT 0
#endif
#if NGH_WX_HOT
#define SH_WX_HOT_K 4u
#define SH_WX_HOT_MAX 32u
#endif

enum {
    SH_OK = 0,
    SH_FWD = 1,
    SH_FAULT = 2,
    SH_SUBPAGE = 3,
    SH_E_FINE = -1,
    SH_E_TINY = -2,
    SH_E_DOMAIN = -3,
    SH_E_WINDOW = -4,
    SH_E_READ = -5,
    SH_E_POOL = -6,
    SH_E_PINFULL = -7,
    SH_E_ARG = -8
};

enum { SH_WX_NOOP = 0, SH_WX_DONE = 1 };
// tlb_mva with this value: the whole TLB, after a drop-all
#define SH_WX_TLB_ALL 1u

struct sh_ops {
    void *ctx;
    int (*rd)(void *ctx, uint32_t pa, uint32_t *v);
    int (*rd_line)(void *ctx, uint32_t pa, uint32_t *v);
    // window guard batch: map a 4 KB page to its alias, clean+invalidate one line of it by alias without a barrier,
    // then read the line back by that alias without maintenance
    int (*page_va)(void *ctx, uint32_t pa, uint32_t *va);
    void (*inv_line)(void *ctx, uint32_t va);
    void (*rd_raw)(void *ctx, uint32_t va, uint32_t *v);
    void (*tail)(void *ctx, uint32_t host_pa, uint32_t len);
    void (*clean)(void *ctx, const void *p, uint32_t len);
    void (*barrier)(void *ctx);
    // a page joins the guard set (len 0: every page turns write-through)
    void (*guard)(void *ctx, uint32_t host_pa, uint32_t len);
    // clean a host page to the point of coherence and invalidate the I side; one shadow entry changed
    void (*code_sync)(void *ctx, uint32_t host_pa, uint32_t len);
    void (*tlb_mva)(void *ctx, uint32_t va);
};

struct sh_walk {
    uint32_t kind;
    uint32_t dom;
    uint32_t ap[4];
    uint32_t c;
    uint32_t b;
    uint32_t base;
    uint32_t fsr;
};

struct sh_hand {
    struct sh_walk w;
    uint32_t va;
    uint32_t ttb;
    uint32_t fx;
    uint32_t ok;
};

struct sh_fill_stats {
    uint32_t pages;
    uint32_t fwd;
    uint32_t refused;
    uint32_t stop;
};

struct sh {
    uint32_t *l1;
    uint32_t *l2;
    uint32_t l2_pa;
    uint32_t n_l2;
    uint32_t l2_used;
    uint32_t win_base;
    uint32_t win_size;
    uint32_t mon_dom;
    struct pam_ctx pam;
    struct sh_ops ops;
    uint32_t g_ttb;
    uint32_t g_sctlr;
    uint32_t g_dacr;
    uint32_t memcfg;
    uint32_t pins[SH_PIN_MAX];
    uint32_t npins;
    // pool tables built from a guest section split into pages, one bit per pool index
    uint32_t l2_sec[SH_POOL_MAX / 32u];
    // pool entires stored since their table was last zeroed, one bit per entry; a reused table zeroes only these
    uint32_t l2_set[SH_POOL_MAX * 8u];
    // L1 indices given a non-window shadow word since the last drop-all, one bit per index
    uint32_t touched[128];
    uint32_t drops;
    uint32_t drop_mva;
    uint32_t drop_sec;
    uint32_t rewalks;
    uint32_t f_sec;
    uint32_t f_large;
    uint32_t f_small;
    // guard set: host pages that keep inner write-through outer non-cacheable; wt_all once it overflows
    uint32_t guard[SH_GUARD_MAX];
    uint32_t nguard;
    uint32_t wt_all;
    // a page joined since the last drop-all
    uint32_t gdue;
    // 2 bits per host RAM page from SCTLR.C=1 fills: the sh_view class seen first, 0 none
    uint32_t dmap[SH_HPAGES / 16u];
    uint32_t dual;
    // aliases from the guard's invalidate pass for its read pass, one per line of the 16 KB guest L1
    uint32_t gline[512];
    // fills, drops, guard joins and ROM tail writes so far: a hand from before any of them is void
    uint32_t fx;
    // write-back pages in state exec, and pages widened to Manager rights, until the shadow reset
    uint32_t wx_x[SH_HPAGES / 32u];
    uint32_t wx_m[SH_HPAGES2 / 32u];
    // per host page: the guest VA page of its one live entry, or the XOR of two, | drop generation << 2 | count
    uint32_t wx_va[SH_HPAGES];
    uint32_t wx_exec;
    uint32_t wx_write;
    uint32_t wx_pages;
    uint32_t wx_mgr;
    uint32_t wx_widen;
    uint32_t wx_multi;
    // live entries per guest domain; live_mgr marks a domain with a live entry emitted under dbits 11, live_cli
    // one emitted under 00, 01 or 10 (a domain may hold both); set by fills only, zeroed before the pin refills
    uint32_t dom_live[16];
    uint32_t live_mgr;
    uint32_t live_cli;
    uint32_t drop_dacr;
    // per DACR change: mask bits Manager class to 01 or 10, Client class to 11, 00 skips of Manager-class domains,
    // 10 on live domains, dom_live over the mask, and the sticky mask and skip domains
    uint32_t dacr_writes;
    uint32_t dacr_m2c;
    uint32_t dacr_c2m;
    uint32_t dacr_skip;
    uint32_t dacr_rsv;
    uint32_t dacr_live;
    uint32_t dacr_mdom;
    uint32_t dacr_sdom;
    // re-emits that found entries, entries they collected, and the scratch list of their VAs
    uint32_t reemits;
    uint32_t reemit_n;
    uint32_t re_va[SH_RE_MAX];
#if NGH_PLAY_WB
    uint32_t wx_off;
#endif
#if NGH_WX_HOT
    uint8_t wxh_n[SH_HPAGES];
    uint32_t wxh_hp[SH_WX_HOT_MAX];
    uint32_t nwxh;
#endif
#if NGH_WX_OSD
    uint32_t wx_osd;
#endif
};

static inline int sh_wx_on(const struct sh *s)
{
#if NGH_PLAY_WB
    if (s->wx_off)
        return 0;
#endif
    return NGH_WX && (s->memcfg & NGST_MC_INNER_WB);
}

static inline int sh_wx_osd(const struct sh *s)
{
#if NGH_WX_OSD
    return s->wx_osd;
#else
    (void)s;
    return 0;
#endif
}

int sh_init(struct sh *s, uint32_t *l1, uint32_t *l2, uint32_t l2_pa, uint32_t n_l2,
            uint32_t win_base, uint32_t win_size, uint32_t mon_dom);
int sh_window_set(struct sh *s, uint32_t va, uint32_t word);
uint32_t sh_real_dacr(const struct sh *s, uint32_t guest_dacr);
uint32_t sh_set_dacr(struct sh *s, uint32_t v);
uint32_t sh_reemit(struct sh *s, uint32_t mask);
int sh_fill(struct sh *s, uint32_t va, uint32_t *fsr);
void sh_drop_all(struct sh *s);
void sh_fill_all(struct sh *s, uint32_t reserve, struct sh_fill_stats *st);
void sh_drop_mva(struct sh *s, uint32_t va);
int sh_pin(struct sh *s, uint32_t va);
void sh_unpin_all(struct sh *s);
int32_t sh_access_check(struct sh *s, uint32_t va, int write, int user);
int sh_va2pa(struct sh *s, uint32_t va, uint32_t *pa);
int sh_fill_hand(struct sh *s, uint32_t va, uint32_t *fsr, struct sh_hand *h);
int32_t sh_access_pa(struct sh *s, struct sh_hand *h, uint32_t va, int write, int user, uint32_t *pa);
int sh_window_guest(struct sh *s, uint32_t *va, uint32_t *l1e);
int32_t sh_check_marked(const struct sh *s);
int sh_guard_add(struct sh *s, uint32_t host_pa);
int sh_l2_host(uint32_t mc, uint32_t rom_len, uint32_t host);
uint32_t sh_l2_set(uint32_t mc, uint32_t rom_len, uint32_t *base, uint32_t *len);
void sh_set_sctlr(struct sh *s, uint32_t sctlr);
int sh_wx_exec(struct sh *s, uint32_t va);
int sh_wx_write(struct sh *s, uint32_t va);
int sh_wx_widen(struct sh *s, uint32_t va);
int sh_wx_exec_range(const struct sh *s, uint32_t host, uint32_t len);
void sh_wx_reset(struct sh *s);
int sh_wx_own(struct sh *s, uint32_t pc, uint32_t va);

#endif
