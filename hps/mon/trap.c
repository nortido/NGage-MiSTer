// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/platform.h"
#include "../common/status.h"
#include "../common/xcap.h"
#include "cp15.h"
#include "ldst.h"
#include "patch.h"
#include "shadow.h"
#include "cache.h"
#include "guest_census.h"

#define TM_USR 0x10u
#define TM_IRQ 0x12u
#define TM_ABT 0x17u
#define TM_UND 0x1Bu
#define TPSR_T 0x20u

// test profile: a patched `b .` ends the guest
#define TRAP_HALT_WORD 0xEAFFFFFEu
#define TRAP_PF_MAX 6u
#define TFS_EXT_SYNC 0x08u
#define TFS_EXT_ASYNC 0x16u
#define TFS_SEC_TRANS 0x05u
// way<<30 | set<<5 for 4 ways x 128 sets; both ROM loops end at 0xC0000FE0
#define SW_MASK 0xC0000FE0u
#define SW_LAST 0xC0000FE0u
#define SW_NET  512u

enum { TU_FWD = 0, TU_CP15 = 1, TU_HALT = 2, TU_MARK = 3, TU_EXIT = 4 };
enum { TMK_BAD = 0, TMK_EMUL = 1, TMK_PARK = 2, TMK_DONE = 3, TMK_FAULT = 4, TMK_UNSUP = 5 };
enum { TD_RESUME = 0, TD_UNDEF = 1, TD_FATAL = 2 };
enum { FC_TRANS = 1, FC_PERM = 2, FC_ALIGN = 3, FC_OTHER = 4 };
enum { TW_OUT = 0, TW_GUEST = 1, TW_FATAL = 2 };
enum { TDC_NONE = 0, TDC_TTBR, TDC_CSR, TDC_MMU, TDC_TLB, TDC_MVA, TDC_N };
enum { TCK_OT = 0, TCK_I0, TCK_I1, TCK_DM, TCK_DALL, TCK_SW, TCK_TL, TCK_DA, TCK_N };
enum { TEM_OT = 0, TEM_IO, TEM_LS, TEM_N };

struct trap_hw {
    void *ctx;
    void (*mmu_mode)(void *ctx, uint32_t m);
    void (*shadow_drop_all)(void *ctx);
    void (*shadow_drop_mva)(void *ctx, uint32_t va);
    void (*set_dacr)(void *ctx, uint32_t v);
    void (*vector_base)(void *ctx, uint32_t base);
    void (*align_mode)(void *ctx, uint32_t a);
    void (*dcache_mva)(void *ctx, uint32_t kind, uint32_t va);
    void (*dcache_all)(void *ctx);
    void (*dcache_clean_all)(void *ctx);
    void (*imb)(void *ctx);
    void (*drain)(void *ctx);
    void (*wfi)(void *ctx);
};

struct trap_sw {
    uint32_t count;
    uint32_t off;
    uint32_t ops;
    uint32_t full;
    uint32_t ci;
    uint32_t ci_full;
    uint32_t co;
    uint32_t co_full;
    uint32_t safe;
    uint32_t drop_rst;
    uint32_t drop_all;
    uint32_t drop_mmu;
    uint32_t drop_mva;
    // a write-back stage is on; c7,c6,1 ran as clean+invalidate
    uint32_t wb;
    uint32_t civ2;
};

struct trap_live {
    uint8_t abt;
    uint8_t und;
};

enum { TGP_NONE = 0, TGP_LEARN = 1, TGP_EXIT = 2 };
enum { TIG_NONE = 0, TIG_FWD = 1, TIG_DEFER = 2, TIG_UNMASK = 3 };

struct trap_gp {
    uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v);
    void *ctx;
    uint32_t cur;
    uint32_t gproc;
    uint32_t exit_seen;
};

int trap_cond_pass(uint32_t cond, uint32_t flags)
{
    uint32_t n = flags >> 31 & 1u, z = flags >> 30 & 1u, c = flags >> 29 & 1u, v = flags >> 28 & 1u;
    int r;

    switch (cond >> 1) {
    case 0: r = (int)z; break;
    case 1: r = (int)c; break;
    case 2: r = (int)n; break;
    case 3: r = (int)v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: return cond == 14;
    }
    return (cond & 1u) ? !r : r;
}

int trap_udf_kind(uint32_t word, int thumb, uint32_t gmode, int test, uint32_t *orig)
{
    int idx = patch_mark_slot(word, thumb);

    if (idx >= 0) {
        *orig = (uint32_t)idx;
        return patch_mark_is_exit((uint32_t)idx) ? TU_EXIT : TU_MARK;
    }
    if (thumb)
        return TU_FWD;
    idx = site_decode(word);
    if (idx < 0 || (uint32_t)idx >= patch_count())
        return TU_FWD;
    *orig = patch_orig((uint32_t)idx);
    if (test && *orig == TRAP_HALT_WORD)
        return TU_HALT;
    if (gmode == TM_USR)
        return TU_FWD;
    return TU_CP15;
}

uint32_t trap_vec_abt(int pabt)
{
    return pabt ? 0x0Cu : 0x10u;
}

int trap_cp15_log(uint32_t log_mode, uint32_t profile)
{
    return log_mode == NGST_LOG_STOP_FULL || profile == NGST_PROFILE_TEST;
}

int trap_mark_act(uint32_t slot, uint32_t *insn, uint32_t *park)
{
    if (slot >= patch_mark_count())
        return TMK_BAD;
    if (patch_mark_is_exit(slot)) {
        *park = NGST_PARK_EXIT;
        return TMK_PARK;
    }
    *insn = patch_mark_orig(slot);
    return TMK_EMUL;
}

static void cond_skip(struct ldst_result *res, uint32_t pc)
{
    res->status = LDST_DONE;
    res->next_pc = pc + 4u;
    res->thumb = 0;
    res->spsr_restore = 0;
    res->fault_va = 0;
    res->fsr = 0;
}

int trap_mark_run(uint32_t slot, uint32_t pc, int thumb, uint32_t cpsr, const struct ldst_ops *ops,
                  struct ldst_result *res, uint32_t *park)
{
    uint32_t insn = 0;
    int a = trap_mark_act(slot, &insn, park), st;

    if (a != TMK_EMUL)
        return a;
    // the BKPT replaced the instruction whatever its condition was
    if (!thumb && !trap_cond_pass(insn >> 28, cpsr)) {
        cond_skip(res, pc);
        return TMK_DONE;
    }
    st = ldst_exec(insn, thumb, pc, cpsr, ops, res);
    return st == LDST_DONE ? TMK_DONE : st == LDST_FAULT ? TMK_FAULT : TMK_UNSUP;
}

// the A9 takes an ARM LDM with writeback and the base in its list (S=0, base not pc) as UNDEFINED, the ARM925T
// runs it; every other word stays a forwarded UNDEFINED
int trap_und_ldm(uint32_t insn, uint32_t pc, uint32_t cpsr, const struct ldst_ops *ops, struct ldst_result *res)
{
    uint32_t rn = (insn >> 16) & 15u;

    res->status = LDST_UNSUP;
    if ((cpsr & TPSR_T) || (insn >> 28) == 15u || (insn & 0x0E700000u) != 0x08300000u || rn == 15u ||
        !((insn >> rn) & 1u))
        return LDST_UNSUP;
    if (!trap_cond_pass(insn >> 28, cpsr)) {
        cond_skip(res, pc);
        return LDST_DONE;
    }
    return ldst_uldm(insn, pc, cpsr, ops, res);
}

uint32_t trap_und_abt_psr(uint32_t cpsr)
{
    return (cpsr & 0xF80F0240u) | 0x100u | 0x80u | TM_ABT;
}

void trap_gp_reset(struct trap_gp *g)
{
    g->cur = 0;
    g->gproc = 0;
    g->exit_seen = 0;
}

int trap_gp_hit(struct trap_gp *g, uint32_t slot, uint32_t pc, uint32_t r0)
{
    uint32_t cur, v;

    switch (patch_mark_kind(slot)) {
    case PATCH_K_LEARN:
        // 0 stays "nothing learned" for the exit rule and in ST gproc
        if (g->gproc || g->rd(g->ctx, patch_mark_param(slot, 0), &cur) ||
            g->rd(g->ctx, cur + patch_mark_param(slot, 1), &v) || !v)
            return TGP_NONE;
        g->cur = cur;
        g->gproc = v;
        return TGP_LEARN;
    case PATCH_K_EXITIF:
        if (!g->gproc || r0 != g->gproc)
            return TGP_NONE;
        g->exit_seen = (pc & ~7u) | slot;
        return TGP_EXIT;
    default:
        return TGP_NONE;
    }
}

static void sw_op(struct trap_sw *sw, const struct trap_hw *hw, uint32_t k, uint32_t v)
{
    int co = k == CP15_ACT_CLEAN_D_SW;

    sw->ops++;
    if (co)
        sw->co++;
    else
        sw->ci++;
    if (!sw->off) {
        if ((v & ~SW_MASK) == 0 && v != SW_LAST && ++sw->count < SW_NET)
            return;
        if (v != SW_LAST)
            sw->safe++;
        sw->count = 0;
    }
    sw->full++;
    if (co) {
        sw->co_full++;
        hw->dcache_clean_all(hw->ctx);
    } else {
        sw->ci_full++;
        hw->dcache_all(hw->ctx);
    }
}

uint32_t trap_drop_cause(uint32_t insn, uint32_t kind)
{
    uint32_t crn = insn >> 16 & 15u;

    switch (kind) {
    case CP15_ACT_SHADOW_RESET:
        return crn == 2u ? TDC_TTBR : crn == 1u ? TDC_CSR : TDC_NONE;
    case CP15_ACT_MMU_MODE:
        return crn == 1u ? TDC_MMU : TDC_NONE;
    case CP15_ACT_SHADOW_DROP_ALL:
        return crn == 8u ? TDC_TLB : TDC_NONE;
    case CP15_ACT_SHADOW_DROP_MVA:
        return crn == 8u ? TDC_MVA : TDC_NONE;
    default:
        return TDC_NONE;
    }
}

void trap_em_end(uint32_t *em, uint32_t last)
{
    em[last < TEM_N ? last : TEM_OT]++;
}

uint32_t trap_cp15_kind(uint32_t insn, const struct cp15_result *r)
{
    uint32_t crn = insn >> 16 & 15u;

    if (insn & 0x100000u)
        return TCK_OT;
    if (crn == 8u)
        return TCK_TL;
    if (crn == 3u)
        return TCK_DA;
    switch (r->nact ? r->act[0].kind : 0) {
    case CP15_ACT_IMB_ALL:
        return (insn & 15u) == 5u && (insn >> 5 & 7u) == 1u ? TCK_I1 : TCK_I0;
    case CP15_ACT_CLEAN_D_MVA:
    case CP15_ACT_INV_D_MVA:
    case CP15_ACT_CLEAN_INV_D_MVA:
    case CP15_ACT_DRAIN:
        return TCK_DM;
    case CP15_ACT_CLEAN_INV_D_ALL:
        return TCK_DALL;
    case CP15_ACT_CLEAN_D_SW:
    case CP15_ACT_CLEAN_INV_D_SW:
        return TCK_SW;
    default:
        return TCK_OT;
    }
}

int trap_dispatch(const struct cp15_result *r, const struct trap_hw *hw, struct trap_sw *sw, uint32_t *fatal_kind, uint32_t *fatal_arg)
{
    uint32_t i;

    for (i = 0; i < r->nact && i < CP15_MAX_ACT; i++) {
        uint32_t k = r->act[i].kind, a = r->act[i].arg;
        switch (k) {
        case CP15_ACT_NOP:
            break;
        case CP15_ACT_MMU_MODE:
            sw->drop_mmu++;
            hw->mmu_mode(hw->ctx, a);
            break;
        case CP15_ACT_SHADOW_RESET:
        case CP15_ACT_SHADOW_DROP_ALL:
            if (k == CP15_ACT_SHADOW_RESET)
                sw->drop_rst++;
            else
                sw->drop_all++;
            hw->shadow_drop_all(hw->ctx);
            break;
        case CP15_ACT_SHADOW_DROP_MVA:
            sw->drop_mva++;
            hw->shadow_drop_mva(hw->ctx, a);
            break;
        case CP15_ACT_SET_DACR:
            hw->set_dacr(hw->ctx, a);
            break;
        case CP15_ACT_VECTOR_BASE:
            hw->vector_base(hw->ctx, a);
            break;
        case CP15_ACT_ALIGN_MODE:
            hw->align_mode(hw->ctx, a);
            break;
        case CP15_ACT_CLEAN_D_MVA:
        case CP15_ACT_INV_D_MVA:
        case CP15_ACT_CLEAN_INV_D_MVA:
            // a write-back line holds stores the ARM920T had already written through
            if (k == CP15_ACT_INV_D_MVA && sw->wb) {
                sw->civ2++;
                k = CP15_ACT_CLEAN_INV_D_MVA;
            }
            hw->dcache_mva(hw->ctx, k, a);
            break;
        case CP15_ACT_CLEAN_INV_D_ALL:
            hw->dcache_all(hw->ctx);
            break;
        case CP15_ACT_CLEAN_D_SW:
        case CP15_ACT_CLEAN_INV_D_SW:
            sw_op(sw, hw, k, a);
            break;
        case CP15_ACT_IMB_ALL:
            hw->imb(hw->ctx);
            break;
        case CP15_ACT_DRAIN:
            hw->drain(hw->ctx);
            break;
        case CP15_ACT_WFI:
            hw->wfi(hw->ctx);
            break;
        case CP15_ACT_UNDEF:
            return TD_UNDEF;
        default:
            *fatal_kind = k;
            *fatal_arg = a;
            return TD_FATAL;
        }
    }
    return TD_RESUME;
}

int trap_vec_literal(uint32_t vec_va, uint32_t word, uint32_t *lit_va)
{
    uint32_t imm = word & 0xFFFu;

    if ((word & 0xFF7FF000u) != 0xE51FF000u)
        return 0;
    *lit_va = (word & 0x00800000u) ? vec_va + 8u + imm : vec_va + 8u - imm;
    return 1;
}

static void pf_add(uint32_t *pages, uint32_t *n, uint32_t va)
{
    uint32_t i, p = va & ~0xFFFu;

    for (i = 0; i < *n; i++)
        if (pages[i] == p)
            return;
    if (*n < TRAP_PF_MAX)
        pages[(*n)++] = p;
}

uint32_t trap_prefill_set(uint32_t vec_va, uint32_t word, int have_lit, uint32_t lit, uint32_t sp, uint32_t *pages)
{
    uint32_t n = 0, lit_va, h;
    int handler = 0;

    pf_add(pages, &n, vec_va);
    if ((word & 0xFF000000u) == 0xEA000000u) {
        h = vec_va + 8u + ((uint32_t)((int32_t)(word << 8) >> 6));
        handler = 1;
    } else if (trap_vec_literal(vec_va, word, &lit_va)) {
        pf_add(pages, &n, lit_va);
        h = lit;
        handler = have_lit;
    }
    if (handler) {
        pf_add(pages, &n, h);
        pf_add(pages, &n, (h & ~0xFFFu) + 0x1000u);
    }
    pf_add(pages, &n, sp - 256u);
    pf_add(pages, &n, sp);
    return n;
}

static uint8_t *live_flag(struct trap_live *l, uint32_t mode)
{
    if (mode == TM_ABT)
        return &l->abt;
    if (mode == TM_UND)
        return &l->und;
    return 0;
}

int trap_live_check(struct trap_live *l, uint32_t emode, uint32_t gmode)
{
    uint8_t *g = live_flag(l, gmode);

    if (g)
        return emode == gmode && *g;
    l->abt = 0;
    l->und = 0;
    return 0;
}

void trap_live_forward(struct trap_live *l, uint32_t fmode)
{
    uint8_t *f = live_flag(l, fmode);

    if (f)
        *f = 1;
}

int trap_irq_gate(struct trap_live *l, uint32_t gmode, int held, int guest_irq)
{
    uint8_t *f = live_flag(l, gmode);
    int live = f && *f;

    // a nested guest IRQ handler traps through the real ABT and UND modes and overwrites the banked LR and
    // SPSR of the forwarded handler it interrupted, so its line waits until the guest has left that mode
    if (guest_irq)
        return live ? TIG_DEFER : TIG_FWD;
    return held && !live ? TIG_UNMASK : TIG_NONE;
}

int trap_fsr_class(uint32_t fsr)
{
    uint32_t fs = (fsr & 0xFu) | ((fsr >> 6) & 0x10u);

    switch (fs) {
    case 0x05:
    case 0x07:
        return FC_TRANS;
    case 0x09:
    case 0x0B:
    case 0x0D:
    case 0x0F:
        return FC_PERM;
    case 0x01:
        return FC_ALIGN;
    default:
        return FC_OTHER;
    }
}

int trap_host_of(const struct sh *s, uint32_t va, uint32_t *host)
{
    uint32_t e = s->l1[va >> 20], pe;

    switch (e & 3u) {
    case 2:
        *host = (e & 0xFFF00000u) | (va & 0xFFFFFu);
        return 1;
    case 1:
        pe = s->l2[((e & 0xFFFFFC00u) - s->l2_pa) / 4u + ((va >> 12) & 0xFFu)];
        if (pe & 2u) {
            *host = (pe & 0xFFFFF000u) | (va & 0xFFFu);
            return 1;
        }
        if (pe & 1u) {
            *host = (pe & 0xFFFF0000u) | (va & 0xFFFFu);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

// no guest store reachs the ROM slot (its writes become flash FWD), but the test profile maps RAM there
__attribute__((noinline)) int trap_rom_site(const struct pam_ctx *c, uint32_t pa)
{
    return c->profile == PAM_PROFILE_NORMAL && pa - PL_ROM_BASE < c->rom_size;
}

uint32_t trap_ident_desc(uint32_t pa)
{
    return (((pa & 0x3FFFu) >> 2) << 20) | 0xC06u;
}

// one forward pins at most TRAP_PF_MAX distinct pages: make room before the first, never during
void trap_pin_begin(struct sh *s)
{
    if (SH_PIN_MAX - s->npins < TRAP_PF_MAX)
        sh_unpin_all(s);
}

int trap_pin(struct sh *s, uint32_t va)
{
    return sh_pin(s, va);
}

// a memcfg bit is accepted once its stage landed; LCD_WT, L1PF and L2PH stay FK_MEMCFG
int trap_memcfg_ok(uint32_t mc)
{
    return !(mc & ~(NGST_MC_INNER_WB | NGST_MC_OUTER_WB | NGST_MC_SWCO_OFF));
}

// the status version names the policy of an arm, e.g. "pw1+m1"
void trap_ver(char *out, const char *v, uint32_t mc)
{
    static const char hx[] = "0123456789abcdef";
    uint32_t n;

    for (n = 0; n < 15u && v[n]; n++)
        out[n] = v[n];
    if (mc) {
        if (n > 11u)
            n = 11u;
        out[n++] = '+';
        out[n++] = 'm';
        if (mc > 15u)
            out[n++] = hx[mc >> 4 & 15u];
        out[n++] = hx[mc & 15u];
    }
    while (n < 16u)
        out[n++] = 0;
}

void trap_sw_memcfg(struct trap_sw *sw, uint32_t mc)
{
    sw->off = mc & NGST_MC_SWCO_OFF;
    sw->wb = (mc & SH_MC_WB) != 0;
}

uint32_t trap_l2_flags(uint32_t mc)
{
    return (mc & NGST_MC_OUTER_WB) ? CACHE_OUTER : 0;
}

uint32_t trap_abort_class(uint32_t fs, int win, int brg_ok)
{
    if (fs == TFS_EXT_SYNC)
        return win ? NGST_PARK_WINDOW_ABORT : 0;
    if (fs != TFS_EXT_ASYNC)
        return 0;
    // with the bridges up it is the PL310 refusing a register write (SLVERR during a Linux background operation)
    return brg_ok ? NGST_PARK_L2 : NGST_PARK_WINDOW_ABORT;
}

uint32_t trap_win_abort(int (*rd)(void *ctx, uint32_t pa, uint32_t *v), void *ctx, uint32_t ttb, int mmu, uint32_t far,
                        uint32_t *l1e)
{
    // a window VA the guest leaves unmapped faults on a phone too (Asphalt 2 reads its poisoned ECP pointer);
    // one the guest maps cannot be aliased, so it stays fatal with the guest L1 word
    if (far - MON_VA_BASE >= MON_VA_SIZE)
        return TW_OUT;
    if (!mmu || rd(ctx, (ttb & 0xFFFFC000u) | ((far >> 20) << 2), l1e) != 0) {
        *l1e = far;
        return TW_FATAL;
    }
    return (*l1e & 3u) ? TW_FATAL : TW_GUEST;
}

uint32_t trap_l2_reason(uint32_t reason, uint32_t abt)
{
    return abt ? NGST_PARK_L2 : reason;
}

#define XC_KB_UNREAD 0xFFFFFFFFu

enum { XC_KB_LEARN = 0, XC_KB_LEAVE = 1, XC_KB_EXIT = 2, XC_KB_N = 3 };

struct xc {
    int32_t code[XC_LV_NCODE];
    uint32_t caller[XC_LV_NCODE];
    uint32_t thr[XC_LV_NCODE];
    uint32_t nlv;
    uint32_t t_lv;
    // launcher hand-overs, carried across exit captures until guest_boot
    uint32_t fol;
    // g_lim.ticks at the learn
    uint32_t t_learn;
    uint32_t kb[XC_KB_N];
    uint32_t kb_min;
    uint32_t row[XC_NEXIT];
    // the newest guest exception forward, packed; lr at the Die; both stop at the gproc exit
    uint32_t dlr, epc, elr, efa, efs;
    uint32_t frozen;
    uint32_t kproc;
    // card-identity reads, all and the game process's, and the newest game read's time
    uint32_t cid_all, cid_game, t_cid;
};

void xc_reset(struct xc *x)
{
    uint32_t i;

    for (i = 0; i < XC_LV_NCODE; i++) {
        x->code[i] = 0;
        x->caller[i] = 0;
        x->thr[i] = 0;
    }
    x->nlv = 0;
    x->t_lv = 0;
    x->fol = 0;
    x->t_learn = 0;
    for (i = 0; i < XC_KB_N; i++)
        x->kb[i] = XC_KB_UNREAD;
    x->kb_min = XC_KB_UNREAD;
    for (i = 0; i < XC_NEXIT; i++)
        x->row[i] = 0;
    x->dlr = 0;
    x->epc = 0;
    x->elr = 0;
    x->efa = 0;
    x->efs = 0;
    x->frozen = 0;
    x->kproc = 0;
    x->cid_all = 0;
    x->cid_game = 0;
    x->t_cid = 0;
}

void xc_cat(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t desc, uint32_t *rows,
            uint32_t *flags)
{
    // TDesC16: type << 28 | length in 16-bit units; any failed read leaves no characters
    uint32_t h, n, a = 0, w = 0, i, u, ch;

    for (i = 0; i < 4u; i++)
        rows[i] = 0;
    if (rd(ctx, desc, &h))
        goto bad;
    n = h & 0x0FFFFFFFu;
    switch (h >> 28) {
    case 0: a = desc + 4u; break;
    case 1: if (n && rd(ctx, desc + 4u, &a)) goto bad; break;
    case 2: if (n && rd(ctx, desc + 8u, &a)) goto bad; break;
    case 3: a = desc + 8u; break;
    case 4: if (n && rd(ctx, desc + 8u, &a)) goto bad; a += 4u; break;
    default: goto bad;
    }
    if (!n)
        return;
    if (a & 1u)
        goto bad;
    if (n > XC_CAT_MAX) {
        *flags |= XC_F_LONG;
        n = XC_CAT_MAX;
    }
    for (i = 0; i < n; i++) {
        u = a + 2u * i;
        if ((i == 0 || !(u & 3u)) && rd(ctx, u & ~3u, &w))
            goto bad;
        ch = (u & 2u) ? w >> 16 : w & 0xFFFFu;
        if (ch < 0x20u || ch > 0x7Eu)
            ch = '?';
        rows[i / 4u] |= ch << (24u - 8u * (i % 4u));
    }
    return;
bad:
    for (i = 0; i < 4u; i++)
        rows[i] = 0;
    *flags |= XC_F_CAT;
}

void xc_leave_push(struct xc *x, int32_t code, uint32_t caller, uint32_t thr, uint32_t t)
{
    uint32_t i;

    for (i = XC_LV_NCODE - 1u; i > 0; i--) {
        x->code[i] = x->code[i - 1u];
        x->caller[i] = x->caller[i - 1u];
        x->thr[i] = x->thr[i - 1u];
    }
    x->code[0] = code;
    x->caller[0] = caller;
    x->thr[0] = thr;
    if (x->nlv != 0xFFFFFFFFu)
        x->nlv++;
    x->t_lv = t;
}

void xc_lrows(const struct xc *x, uint32_t *r)
{
    uint32_t i;

    for (i = 0; i < XC_LV_NCODE; i++) {
        r[XC_L_C0 + i] = x->caller[i];
        r[XC_L_T0 + i] = x->thr[i];
    }
}

__attribute__((noinline)) int xc_leave_gate(uint32_t va, uint32_t orig, uint32_t own, uint32_t gproc)
{
    // only the learned game process's own threads feed the Leave list
    return va == XC_LEAVE_VA && orig == XC_LEAVE_ORIG && gproc && own == gproc;
}

enum { XC_CID_NONE = 0, XC_CID_OTHER = 1, XC_CID_GAME = 2 };

__attribute__((noinline)) int xc_cid_gate(uint32_t va, uint32_t orig, uint32_t own, uint32_t gproc)
{
    if (va != XC_CID_VA || orig != XC_CID_ORIG || !gproc)
        return XC_CID_NONE;
    return own == gproc ? XC_CID_GAME : XC_CID_OTHER;
}

void xc_cid_push(struct xc *x, int kind, uint32_t t)
{
    // the exit freezes it with the exit rows: the fork compares the death with the reads before it
    if (kind == XC_CID_NONE || x->frozen)
        return;
    if (x->cid_all != 0xFFFFFFFFu)
        x->cid_all++;
    if (kind != XC_CID_GAME)
        return;
    if (x->cid_game != 0xFFFFFFFFu)
        x->cid_game++;
    x->t_cid = t;
}

static uint32_t xc_u16(uint32_t v)
{
    return v < 0xFFFFu ? v : 0xFFFFu;
}

void xc_crows(const struct xc *x, uint32_t *r)
{
    r[XC_C_CN] = xc_u16(x->cid_game) << 16 | xc_u16(x->cid_all);
    r[XC_C_CT] = x->t_cid / XC_TIM_MS;
}

uint32_t xc_own_flags(uint32_t rc_cur, uint32_t rc_own, uint32_t own, uint32_t gproc)
{
    if (rc_cur)
        return XC_F_CUR | XC_F_OWN;
    if (rc_own)
        return XC_F_OWN;
    return own != gproc ? XC_F_FOREIGN : 0;
}

static uint32_t xc_s16(int32_t v)
{
    return v < -32768 ? 0x8000u : v > 32767 ? 0x7FFFu : (uint32_t)v & 0xFFFFu;
}

void xc_pack_codes(const struct xc *x, uint32_t *xlvc, uint32_t *xlvd)
{
    *xlvc = xc_s16(x->code[0]) << 16 | xc_s16(x->code[1]);
    *xlvd = xc_s16(x->code[2]) << 16 | (x->nlv < XC_LV_CNT_MAX ? x->nlv : XC_LV_CNT_MAX);
}

uint32_t xc_pack_time(uint32_t t_exit, uint32_t t_leave, int have_leave)
{
    uint32_t e = t_exit / XC_TIM_MS, l = XC_TIM_LV_NONE;

    if (have_leave) {
        l = t_exit > t_leave ? (t_exit - t_leave) / XC_TIM_MS : 0;
        if (l > XC_TIM_LV_MAX)
            l = XC_TIM_LV_MAX;
    }
    return l << XC_TIM_LV_SHIFT | (e < XC_TIM_EXIT ? e : XC_TIM_EXIT);
}

static uint32_t xc_kb16(uint32_t kb)
{
    return kb == XC_KB_UNREAD ? XC_KB_NONE : kb < XC_KB_MAX ? kb : XC_KB_MAX;
}

uint32_t xc_pack_kb(uint32_t hi, uint32_t lo)
{
    return xc_kb16(hi) << 16 | xc_kb16(lo);
}

uint32_t xc_kb_pages(uint32_t pages)
{
    return pages < XC_KB_MAX / XC_PAGE_KB ? pages * XC_PAGE_KB : XC_KB_MAX;
}

void xc_kb_set(struct xc *x, uint32_t which, uint32_t kb)
{
    x->kb[which] = kb;
    if (kb < x->kb_min)
        x->kb_min = kb;
}

void xc_exit_fill(struct xc *x, uint32_t type, uint32_t reason, const uint32_t *cat, uint32_t flags, uint32_t thr,
                  uint32_t t_exit)
{
    // the one writer of the exit rows: every row replaced, the hand-over count kept
    uint32_t i;

    x->row[XC_R_TYP] = (type & XC_TYP_TYPE) | (flags & XC_F_ALL) |
                       (x->fol < XC_TYP_FOL_MAX ? x->fol : XC_TYP_FOL_MAX) << XC_TYP_FOL_SHIFT;
    x->row[XC_R_RSN] = reason;
    for (i = 0; i < 4u; i++)
        x->row[XC_R_CT0 + i] = cat[i];
    x->row[XC_R_THR] = thr;
    x->row[XC_R_TIM] = xc_pack_time(t_exit, x->t_lv, x->nlv != 0);
}

void xc_rows(const struct xc *x, uint32_t *r)
{
    uint32_t i;

    for (i = 0; i < XC_NEXIT; i++)
        r[i] = x->row[i];
    xc_pack_codes(x, &r[XC_R_LVC], &r[XC_R_LVD]);
    r[XC_R_FRA] = xc_pack_kb(x->kb[XC_KB_LEARN], x->kb[XC_KB_LEAVE]);
    r[XC_R_FRE] = xc_pack_kb(x->kb[XC_KB_EXIT], x->kb_min);
}

// DThread::Die calls DProcess::Die for a main thread with the dying DThread in r5; the exec paths (Kill
// 0x503325e0, Terminate 0x50332624, the Panic tail) name no thread, their requester sits in a kernel cell
#define XC_DIE_LR_THREAD 0x5033383Cu
#define XC_DIE_REG 5u
#define XC_THR_OWNER 0x18u

struct xc_die {
    uint32_t lr, sreg, rc_cur, cur, own, rc_req, req;
};

__attribute__((noinline)) void xc_die_who(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx,
                                          const struct xc_die *d, uint32_t gproc, uint32_t kproc, uint32_t *thr,
                                          uint32_t *flags)
{
    // every Die runs in the Supervisor, so the killer is the exec's requester, not the current thread
    uint32_t o;

    *thr = d->req;
    if (d->lr == XC_DIE_LR_THREAD) {
        *thr = d->sreg;
        *flags |= XC_F_SREG;
    }
    if (d->rc_req)
        *flags |= XC_F_CUR | XC_F_OWN;
    else if (rd(ctx, d->req + XC_THR_OWNER, &o))
        *flags |= XC_F_OWN;
    else if (o != gproc && o != kproc)
        *flags |= XC_F_FOREIGN;
    if (!d->rc_cur && kproc && d->own == kproc)
        *flags |= XC_F_KDIE;
}

__attribute__((noinline)) void xc_fwd_note(struct xc *x, uint32_t kind, uint32_t pc, uint32_t lr, uint32_t far,
                                           uint32_t fsr, uint32_t mode, uint32_t t)
{
    uint32_t u = t / XC_TIM_MS;

    if (x->frozen)
        return;
    x->epc = pc;
    x->elr = lr;
    x->efa = far;
    x->efs = (fsr & 0xFFu) | (kind & 3u) << XC_EFS_KIND_SHIFT | (mode & 0x1Fu) << XC_EFS_MODE_SHIFT |
             (u < XC_EFS_TIM_MAX ? u : XC_EFS_TIM_MAX) << XC_EFS_TIM_SHIFT;
}

void xc_freeze(struct xc *x, uint32_t flags, uint32_t dlr)
{
    // xc_exit_fill keeps XTYP to the bits the live fork prints; the harness bits join here
    x->row[XC_R_TYP] |= flags & XC_F_HX;
    x->dlr = dlr;
    x->frozen = 1;
}

// the process container cell; slots in creation order, a dead process stays listed
#define XC_PCON 0x80000398u
#define XC_PCON_N 8u
#define XC_PCON_ARR 0x10u
#define XC_P_EXIT 0x2Cu
#define XC_P_PATH 0x3Cu
#define XC_P_ALIVE 3u
#define XC_FOL_NMAX 64u
#define XC_FOL_WIN_MS 30000u

static const unsigned char xc_fol_run[] = "z:\\system\\programs\\apprun.exe";
static const unsigned char xc_fol_app[] = "?:\\system\\apps\\";

static int xc_fol_img(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t p)
{
    // AppRun.exe itself or any image under <A-Y>:\System\Apps\, case ignored; -1 on a failed read
    uint32_t h, n, w = 0, i, u;
    int run, app;

    if (rd(ctx, p + XC_P_PATH, &h) || rd(ctx, h, &n))
        return -1;
    n &= 0x0FFFFFFFu;
    run = n == sizeof xc_fol_run - 1u;
    app = n >= sizeof xc_fol_app - 1u;
    for (i = 0; run || app; i++) {
        if ((run && i == sizeof xc_fol_run - 1u) || (app && i == sizeof xc_fol_app - 1u))
            return 1;
        if (!(i & 1u) && rd(ctx, h + 4u + 2u * i, &w))
            return -1;
        u = (i & 1u) ? w >> 16 : w & 0xFFFFu;
        if (u - 'A' < 26u)
            u += 'a' - 'A';
        run = run && u == xc_fol_run[i];
        app = app && (i ? u == xc_fol_app[i] : u - 'a' < 25u);
    }
    return 0;
}

__attribute__((noinline)) uint32_t xc_follow(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx,
                                             int first, uint32_t dying, uint32_t typ, uint32_t rsn, uint32_t flags,
                                             uint32_t cause, uint32_t fol, uint32_t t_learn, uint32_t now)
{
    // the lowest app above the launcher is the one it started, followed only alive. An ended
    // app leaves the container once its last handle closes, so an armed Die follows only within 30 s of the learn
    uint32_t c, n, a, i, k, p, e, nx = 0;
    int m, live = 0;

    if (!first || typ || rsn || (flags & XC_F_FOREIGN) || (cause && (fol || now - t_learn > XC_FOL_WIN_MS)))
        return 0;
    if (rd(ctx, XC_PCON, &c) || rd(ctx, c + XC_PCON_N, &n) || rd(ctx, c + XC_PCON_ARR, &a))
        return 0;
    for (i = n, k = 0; i && k < XC_FOL_NMAX; i--, k++) {
        if (rd(ctx, a + 4u * (i - 1u), &p))
            return 0;
        if (p == dying)
            return live ? nx : 0;
        if (rd(ctx, p + XC_P_EXIT, &e))
            return 0;
        m = xc_fol_img(rd, ctx, p);
        if (m < 0)
            return 0;
        if (m) {
            nx = p;
            live = e == XC_P_ALIVE;
        }
    }
    return 0;
}

void xc_follow_step(struct xc *x, struct trap_gp *g, uint32_t nx)
{
    // unfrozen, so the successor's own Die rewrites every exit row and sets its own harness bits; the
    // Leave list and CID counts stay
    if (x->fol < XC_TYP_FOL_MAX)
        x->fol++;
    x->row[XC_R_TYP] = (x->row[XC_R_TYP] & ~(XC_TYP_FOL_MAX << XC_TYP_FOL_SHIFT | XC_F_HX)) |
                       x->fol << XC_TYP_FOL_SHIFT;
    x->frozen = 0;
    g->gproc = nx;
    g->exit_seen = 0;
}

void xc_drows(const struct xc *x, uint32_t *r)
{
    r[XC_D_DLR] = x->dlr;
    r[XC_D_EPC] = x->epc;
    r[XC_D_ELR] = x->elr;
    r[XC_D_EFA] = x->efa;
    r[XC_D_EFS] = x->efs;
}

// EX0 q1: the exec reschedule path saves a descheduled thread's user sp, lr and pc here
#define XC_TR_SP 0x158u
#define XC_TR_LR 0x15Cu
#define XC_TR_PC 0x160u
// EX0 q2: RAM-loaded code slots seen from 0xfeb00000 to 0xfff9a000, pseudo handles from 0xffff0000
#define XC_RAMCODE_LO 0xFE000000u
#define XC_RAMCODE_HI 0xFFFF0000u
#define XC_TR_SPAN 128u
#define XC_TR_NW 8u
#define XC_TR_NBL 4u
#define XC_TF_BLK 0x100u
#define XC_TF_UP 0x200u
#define XC_TF_DN 0x400u
#define XC_TF_BL 0x800u

enum { XC_T_SP, XC_T_PC, XC_T_LR, XC_T_FL, XC_T_W0, XC_T_O0 = XC_T_W0 + 8, XC_T_I0 = XC_T_O0 + 2, XC_NTROW = XC_T_I0 + 4 };

// DThread::Die frees a main thread's stack chunk before it calls DProcess::Die, so the stack
// is read at the DThread::Die entry (r0 the thread) through the chunk's current base, home or run address
#define XC_TDIE_VA 0x503336D0u
#define XC_TDIE_ORIG 0xE92D47F0u
#define XC_THR_FLAGS 0x20u
#define XC_THR_MAIN 0x80000000u
#define XC_THR_EXIT 0x24u
#define XC_THR_LIVE 3u
#define XC_THR_STK 0x2Cu
#define XC_CH_SIZE 0x18u
#define XC_CH_BASE 0x28u
#define XC_CH_HOME 0x80000000u
#define XC_CH_UNREAD 0xFFFFFFFFu
#define XC_P_NCH 0xACu
#define XC_P_CHS 0xB4u
#define XC_P_NCH_MAX 16u
#define XC_TF_EARLY 0x1000u
#define XC_TF_HOME 0x2000u
#define XC_TF_NOCH 0x4000u
#define XC_TF_NOENT 0x8000u
#define XC_TF_RANGE 0x10000u

enum { XC_TK_CH, XC_TK_CB, XC_TK_EA, XC_NTK };
// rows after the trace rows: [thr+0x2c] re-read and the direct read's flags at the Die hit, then the kept capture
enum { XC_E_CH, XC_E_CF, XC_E_KT, XC_E_KC, XC_E_KB, XC_E_EA, XC_NEROW };

#if NGH_XC_TRACE
int xc_ramcode(uint32_t w)
{
    return w >= XC_RAMCODE_LO && w < XC_RAMCODE_HI;
}

static int xc_trace_blk(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t thr, uint32_t *r)
{
    uint32_t i;

    for (i = 0; i < XC_NTROW; i++)
        r[i] = 0;
    if (!thr || rd(ctx, thr + XC_TR_SP, &r[XC_T_SP]) || rd(ctx, thr + XC_TR_PC, &r[XC_T_PC]) ||
        rd(ctx, thr + XC_TR_LR, &r[XC_T_LR])) {
        r[XC_T_FL] = XC_TF_BLK;
        return 1;
    }
    return 0;
}

static void xc_scan(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t ea, uint32_t *r)
{
    // above sp first, then the stale frames below it, nearest first; a failed read ends that direction
    uint32_t i, d, w, n = 0;
    int32_t o;

    for (d = 0; d < 2u; d++)
        for (i = 0; i < XC_TR_SPAN && n < XC_TR_NW; i++) {
            o = d ? -(int32_t)i - 1 : (int32_t)i;
            if (rd(ctx, ea + 4u * (uint32_t)o, &w)) {
                r[XC_T_FL] |= d ? XC_TF_DN : XC_TF_UP;
                break;
            }
            if (!xc_ramcode(w))
                continue;
            r[XC_T_W0 + n] = w;
            r[XC_T_O0 + n / 4u] |= ((uint32_t)o & 0xFFu) << (24u - 8u * (n % 4u));
            n++;
        }
    // a Thumb return address has bit 0 set: the aligned word then holds its BL
    for (i = 0; i < n && i < XC_TR_NBL; i++)
        if (rd(ctx, (r[XC_T_W0 + i] - 4u) & ~3u, &r[XC_T_I0 + i]))
            r[XC_T_FL] |= XC_TF_BL;
    r[XC_T_FL] |= n;
}

void xc_trace(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t thr, uint32_t *r)
{
    if (!xc_trace_blk(rd, ctx, thr, r))
        xc_scan(rd, ctx, r[XC_T_SP], r);
}

__attribute__((noinline)) int xc_tdie_gate(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx,
                                           uint32_t va, uint32_t orig, uint32_t thr, uint32_t frozen, uint32_t kproc)
{
    // DThread::Die's own tests before its DProcess::Die call: exit type still 3 (a second Die skips), main thread
    uint32_t e, f, o;

    if (va != XC_TDIE_VA || orig != XC_TDIE_ORIG || frozen || !thr)
        return 0;
    if (rd(ctx, thr + XC_THR_EXIT, &e) || e != XC_THR_LIVE || rd(ctx, thr + XC_THR_FLAGS, &f) || !(f & XC_THR_MAIN))
        return 0;
    return !rd(ctx, thr + XC_THR_OWNER, &o) && o != kproc;
}

uint32_t xc_stk_ea(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t thr, uint32_t sp,
                   uint32_t *ch, uint32_t *cb, uint32_t *ea)
{
    // the kernel's user-to-kernel translation (0x50333a28): the process entry of the chunk gives its run base, the
    // chunk's current base says where it sits now; a run base needs no entry and cannot alias another process
    uint32_t p, n, a, i, c, b, sz;

    *ch = 0;
    *cb = 0;
    *ea = 0;
    if (rd(ctx, thr + XC_THR_STK, ch) || !*ch || rd(ctx, *ch + XC_CH_BASE, cb))
        return XC_TF_NOCH;
    if (*cb < XC_CH_HOME) {
        *ea = sp;
        return 0;
    }
    if (rd(ctx, thr + XC_THR_OWNER, &p) || rd(ctx, p + XC_P_NCH, &n) || rd(ctx, p + XC_P_CHS, &a))
        return XC_TF_HOME | XC_TF_NOENT;
    for (i = 0; i < n && i < XC_P_NCH_MAX; i++) {
        if (rd(ctx, a + 12u * i + 4u, &c))
            return XC_TF_HOME | XC_TF_NOENT;
        if (c != *ch)
            continue;
        if (rd(ctx, a + 12u * i, &b) || rd(ctx, *ch + XC_CH_SIZE, &sz))
            return XC_TF_HOME | XC_TF_NOENT;
        if (sp - b >= sz)
            return XC_TF_HOME | XC_TF_RANGE;
        *ea = *cb + (sp - b);
        return XC_TF_HOME;
    }
    return XC_TF_HOME | XC_TF_NOENT;
}

__attribute__((noinline)) void xc_trace_early(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx,
                                              uint32_t thr, uint32_t *r, uint32_t *x)
{
    uint32_t f;

    x[XC_TK_CH] = 0;
    x[XC_TK_CB] = 0;
    x[XC_TK_EA] = 0;
    if (xc_trace_blk(rd, ctx, thr, r)) {
        r[XC_T_FL] |= XC_TF_EARLY;
        return;
    }
    f = xc_stk_ea(rd, ctx, thr, r[XC_T_SP], &x[XC_TK_CH], &x[XC_TK_CB], &x[XC_TK_EA]);
    r[XC_T_FL] = XC_TF_EARLY | f;
    if (!(f & (XC_TF_NOCH | XC_TF_NOENT | XC_TF_RANGE)))
        xc_scan(rd, ctx, x[XC_TK_EA], r);
}

struct xc_tk {
    uint32_t thr;
    uint32_t x[XC_NTK];
    uint32_t row[XC_NTROW];
};

__attribute__((noinline)) void xc_trace_hit(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx,
                                            const struct xc_tk *k, uint32_t thr, uint32_t *r, uint32_t *e)
{
    // the direct read stays as the control; the entry capture replaces its rows only for the same dying thread
    uint32_t i;

    xc_trace(rd, ctx, thr, r);
    e[XC_E_CF] = r[XC_T_FL];
    if (!thr || rd(ctx, thr + XC_THR_STK, &e[XC_E_CH]))
        e[XC_E_CH] = XC_CH_UNREAD;
    e[XC_E_KT] = k->thr;
    for (i = 0; i < XC_NTK; i++)
        e[XC_E_KC + i] = k->x[XC_TK_CH + i];
    if (!thr || k->thr != thr)
        return;
    for (i = 0; i < XC_NTROW; i++)
        r[i] = k->row[i];
}
#endif

#if NGH_MK_CAP
#include "../common/evlog.h"

#define MK_SPAN 256u
#define MK_NW 9u
#define MK_NNM 3u
#define MK_KLO 0x80000000u
#define MK_KHI 0x81000000u
#define MK_ROM_LO 0x50000000u
#define MK_ROM_HI 0x52000000u
#define MK_M_SVC 0x13u
#define MK_M_SYS 0x1Fu
#define MK_OBJ_NAME 0x10u
#define MK_HB_TEXT 4u
#define MK_RF_CUR 1u
#define MK_RF_OWN 2u
#define MK_RF_LR 4u
#define MK_SF_RD 1u
#define MK_SF_NONE 2u
#define MK_SF_CAP 4u

struct mk_in {
    uint32_t r[4], lr, sp, cur, own, gm, thumb, rf, ulr, usp, scan;
    uint32_t w[MK_NW], o[MK_NW], nm[2][MK_NNM];
};

static int mk_kva(uint32_t p)
{
    return !(p & 3u) && p >= MK_KLO && p < MK_KHI;
}

static int mk_code(uint32_t w)
{
    return (w >= MK_ROM_LO && w < MK_ROM_HI) || (w >= XC_RAMCODE_LO && w < XC_RAMCODE_HI);
}

uint32_t mk_scan(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t gm, uint32_t sp, uint32_t *w,
                 uint32_t *o)
{
    uint32_t i, v, n = 0, sf = 0;

    for (i = 0; i < MK_NW; i++) {
        w[i] = 0;
        o[i] = 0;
    }
    if ((gm != MK_M_SVC && gm != MK_M_SYS) || !mk_kva(sp))
        return MK_SF_NONE << 20;
    for (i = 0; i < MK_SPAN; i++) {
        if (rd(ctx, sp + 4u * i, &v)) {
            sf = MK_SF_RD;
            break;
        }
        if (!mk_code(v))
            continue;
        w[n] = v;
        o[n] = i;
        if (++n == MK_NW) {
            sf = MK_SF_CAP;
            i++;
            break;
        }
    }
    return n | i << 8 | sf << 20;
}

uint32_t mk_name(uint32_t (*rd)(void *ctx, uint32_t va, uint32_t *v), void *ctx, uint32_t obj, uint32_t *nm)
{
    uint32_t h, t[MK_NNM], i;

    for (i = 0; i < MK_NNM; i++)
        nm[i] = 0;
    if (!mk_kva(obj) || rd(ctx, obj + MK_OBJ_NAME, &h) || !mk_kva(h))
        return 1;
    for (i = 0; i < MK_NNM; i++)
        if (rd(ctx, h + MK_HB_TEXT + 4u * i, &t[i]))
            return 1;
    for (i = 0; i < MK_NNM; i++)
        nm[i] = t[i];
    return 0;
}

void mk_pack(const struct mk_in *in, uint32_t slot, uint32_t rec[EV_MKCAP_NREC][4])
{
    uint32_t k, i, n = in->scan & 0xFFu, w[MK_NW], ob[3] = { 0, 0, 0 };

    for (i = 0; i < MK_NW; i++) {
        w[i] = i < n ? in->w[i] : 0;
        if (i < n)
            ob[i / 4u] |= (in->o[i] & 0xFFu) << (24u - 8u * (i % 4u));
    }
    for (k = 0; k < EV_MKCAP_NREC; k++)
        rec[k][0] = EV_MKCAP_TAG | (k + 1u) << 8 | slot;
    rec[EV_MKCAP_REGS - 1u][1] = in->r[0];
    rec[EV_MKCAP_REGS - 1u][2] = in->r[1];
    rec[EV_MKCAP_REGS - 1u][3] = in->r[2];
    rec[EV_MKCAP_LRSP - 1u][1] = in->r[3];
    rec[EV_MKCAP_LRSP - 1u][2] = in->lr;
    rec[EV_MKCAP_LRSP - 1u][3] = in->sp;
    rec[EV_MKCAP_THR - 1u][1] = in->cur;
    rec[EV_MKCAP_THR - 1u][2] = in->own;
    rec[EV_MKCAP_THR - 1u][3] = in->gm | in->thumb << 8 | in->rf << 16;
    rec[EV_MKCAP_USR - 1u][1] = in->ulr;
    rec[EV_MKCAP_USR - 1u][2] = in->usp;
    rec[EV_MKCAP_USR - 1u][3] = in->scan;
    for (i = 0; i < MK_NW; i++)
        rec[EV_MKCAP_W0 - 1u + i / 3u][1 + i % 3u] = w[i];
    for (i = 0; i < 3u; i++) {
        rec[EV_MKCAP_OFF - 1u][1 + i] = ob[i];
        rec[EV_MKCAP_NM0 - 1u][1 + i] = in->nm[0][i];
        rec[EV_MKCAP_NM1 - 1u][1 + i] = in->nm[1][i];
    }
}
#endif
