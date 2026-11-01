// SPDX-License-Identifier: GPL-3.0-or-later
#include "ldst.h"

#define MODE_USER 0x10u
#define MODE_SYS 0x1fu

struct wr {
    uint32_t v;
    uint8_t mode;
    uint8_t n;
};

struct ctx {
    const struct ldst_ops *ops;
    struct ldst_result *res;
    uint32_t pc;
    unsigned mode;
    int thumb;
    struct wr w[18];
    int nw;
    int pc_set;
    uint32_t pc_val;
    int spsr;
    int uldm;
};

struct xfer {
    unsigned rn, rd;
    unsigned size;
    int load, sign, pre, up, wb, user;
    uint32_t off;
};

struct blk {
    unsigned rn;
    uint32_t list;
    int load, pre, up, wb, s;
};

static uint32_t pc_operand(const struct ctx *c)
{
    return c->pc + (c->thumb ? 4u : 8u);
}

static uint32_t reg(const struct ctx *c, unsigned mode, unsigned n)
{
    if (n == 15)
        return pc_operand(c);
    return c->ops->get_reg(c->ops->ctx, mode, n);
}

// RTL feeds fetch_PC, one fetch past the operand PC, to a stored r15
static uint32_t store_pc(const struct ctx *c)
{
    return c->pc + 12u;
}

static void queue(struct ctx *c, unsigned mode, unsigned n, uint32_t v)
{
    if (n == 15) {
        c->pc_set = 1;
        c->pc_val = v;
        return;
    }
    c->w[c->nw].mode = (uint8_t)mode;
    c->w[c->nw].n = (uint8_t)n;
    c->w[c->nw].v = v;
    c->nw++;
}

static int mem_access(struct ctx *c, uint32_t va, unsigned size, int write, int t_form, uint32_t *data, uint32_t fva)
{
    uint32_t fsr = c->ops->mem(c->ops->ctx, va, size, write, t_form || c->mode == MODE_USER, data);
    if (!fsr)
        return 0;
    c->res->status = LDST_FAULT;
    c->res->fault_va = fva;
    c->res->fsr = fsr;
    return 1;
}

static int align_fault(struct ctx *c, uint32_t ea, unsigned size)
{
    if (!c->ops->align || (ea & (size - 1u)) == 0)
        return 0;
    c->res->status = LDST_FAULT;
    c->res->fault_va = ea;
    c->res->fsr = 1;
    return 1;
}

static int finish(struct ctx *c)
{
    int i;
    struct ldst_result *r = c->res;

    for (i = 0; i < c->nw; i++)
        c->ops->set_reg(c->ops->ctx, c->w[i].mode, c->w[i].n, c->w[i].v);
    r->status = LDST_DONE;
    r->spsr_restore = c->spsr;
    r->thumb = c->thumb;
    r->next_pc = c->pc + (c->thumb ? 2u : 4u);
    if (c->pc_set) {
        if (c->spsr) {
            r->next_pc = c->pc_val;
            r->thumb = 0;
        } else if (c->uldm) {
            // the ARM925T is ARMv4T, an LDM to pc does not interwork
            r->thumb = 0;
            r->next_pc = c->pc_val & ~3u;
        } else {
            r->thumb = c->pc_val & 1;
            r->next_pc = c->pc_val & (r->thumb ? ~1u : ~3u);
        }
    }
    return LDST_DONE;
}

static uint32_t sext8(uint32_t v)
{
    return (v & 0x80u) ? (v & 0xffu) | 0xffffff00u : (v & 0xffu);
}

static uint32_t sext16(uint32_t v)
{
    return (v & 0x8000u) ? (v & 0xffffu) | 0xffff0000u : (v & 0xffffu);
}

static int do_single(struct ctx *c, const struct xfer *x)
{
    uint32_t base, addr, ea, va, d = 0;

    if (x->rn == 15) {
        if (x->wb)
            return LDST_UNSUP;
        base = pc_operand(c) & ~2u;
    } else {
        base = reg(c, c->mode, x->rn);
    }
    addr = x->up ? base + x->off : base - x->off;
    ea = x->pre ? addr : base;
    va = ea & ~(x->size - 1u);
    if (x->size > 1 && align_fault(c, ea, x->size))
        return LDST_FAULT;

    if (x->load) {
        if (mem_access(c, va, x->size, 0, x->user, &d, ea))
            return LDST_FAULT;
        if (x->size == 4)
            queue(c, c->mode, x->rd, d);
        else if (x->size == 2 && !x->sign)
            queue(c, c->mode, x->rd, d & 0xffffu);
        else if (x->size == 2)
            queue(c, c->mode, x->rd, (ea & 1u) ? sext8(d) : sext16(d));
        else
            queue(c, c->mode, x->rd, x->sign ? sext8(d) : d & 0xffu);
        if (x->wb && x->rd != x->rn)
            queue(c, c->mode, x->rn, addr);
    } else {
        d = x->rd == 15 ? store_pc(c) : reg(c, c->mode, x->rd);
        if (x->size == 2)
            d &= 0xffffu;
        else if (x->size == 1)
            d &= 0xffu;
        if (mem_access(c, va, x->size, 1, x->user, &d, ea))
            return LDST_FAULT;
        if (x->wb)
            queue(c, c->mode, x->rn, addr);
    }
    return LDST_DONE;
}

static int do_swap(struct ctx *c, uint32_t insn)
{
    unsigned rn = (insn >> 16) & 15u, rd = (insn >> 12) & 15u, rm = insn & 15u;
    unsigned size = (insn & (1u << 22)) ? 1u : 4u;
    uint32_t ea, va, old = 0, nv;

    if (rn == 15 || rd == 15 || rm == 15)
        return LDST_UNSUP;
    ea = reg(c, c->mode, rn);
    va = ea & ~(size - 1u);
    if (size == 4 && align_fault(c, ea, size))
        return LDST_FAULT;
    nv = reg(c, c->mode, rm);
    if (size == 1)
        nv &= 0xffu;
    if (mem_access(c, va, size, 0, 0, &old, ea))
        return LDST_FAULT;
    if (mem_access(c, va, size, 1, 0, &nv, ea))
        return LDST_FAULT;
    queue(c, c->mode, rd, size == 1 ? old & 0xffu : old);
    return LDST_DONE;
}

static unsigned popcount16(uint32_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1u)
        n++;
    return n;
}

static int do_block(struct ctx *c, const struct blk *b)
{
    uint32_t list = b->list, base, end, start, bytes;
    unsigned bank = c->mode, i, k = 0;
    int pcload = b->load && (list & 0x8000u);
    int wb = b->wb;

    if (b->rn == 15)
        return LDST_UNSUP;
    if (!list) {
        if (c->thumb)
            return LDST_UNSUP;
        list = 0x8000u;
        bytes = 64u;
    } else {
        bytes = 4u * popcount16(list);
    }
    if (pcload && b->s) {
        if (c->mode == MODE_USER || c->mode == MODE_SYS)
            return LDST_UNSUP;
        c->spsr = 1;
    } else if (b->s) {
        bank = MODE_USER;
    }
    if (b->load && ((list >> b->rn) & 1u) && !c->uldm)
        wb = 0;

    base = reg(c, c->mode, b->rn);
    end = b->up ? base + bytes : base - bytes;
    start = b->up ? base + (b->pre ? 4u : 0u) : end + (b->pre ? 0u : 4u);
    start &= ~3u;

    for (i = 0; i < 16; i++) {
        uint32_t va = start + 4u * k, d = 0;
        if (!((list >> i) & 1u))
            continue;
        if (b->load) {
            if (mem_access(c, va, 4, 0, 0, &d, va))
                return LDST_FAULT;
            queue(c, i == 15 ? c->mode : bank, i, d);
        } else {
            if (i == 15)
                d = store_pc(c);
            else if (i == b->rn && wb && k > 0)
                d = end;
            else
                d = reg(c, bank, i);
            if (mem_access(c, va, 4, 1, 0, &d, va))
                return LDST_FAULT;
        }
        k++;
    }
    if (wb)
        queue(c, c->mode, b->rn, end);
    return LDST_DONE;
}

static uint32_t shifted(const struct ctx *c, uint32_t insn, uint32_t cpsr)
{
    uint32_t v = reg(c, c->mode, insn & 15u);
    unsigned amt = (insn >> 7) & 31u;

    switch ((insn >> 5) & 3u) {
    case 0:
        return v << amt;
    case 1:
        return amt ? v >> amt : 0;
    case 2:
        if (!amt)
            return (v & 0x80000000u) ? 0xffffffffu : 0;
        return (v >> amt) | ((v & 0x80000000u) ? ~(0xffffffffu >> amt) : 0);
    default:
        if (!amt)
            return (v >> 1) | ((cpsr & 0x20000000u) ? 0x80000000u : 0);
        return (v >> amt) | (v << (32 - amt));
    }
}

static int arm_exec(struct ctx *c, uint32_t insn, uint32_t cpsr)
{
    unsigned cls = (insn >> 25) & 7u;
    struct xfer x;

    if ((insn >> 28) == 0xfu)
        return LDST_UNSUP;
    if ((insn & 0x0fb00ff0u) == 0x01000090u)
        return do_swap(c, insn);
    if ((insn & 0x0fff0ff0u) == 0x01a00000u) {
        // `mov rd, rm` without S or shift, the User::Leave entry word; pc as rd or rm stays unsupported
        unsigned rd = (insn >> 12) & 15u, rm = insn & 15u;

        if (rd == 15 || rm == 15)
            return LDST_UNSUP;
        queue(c, c->mode, rd, reg(c, c->mode, rm));
        return LDST_DONE;
    }

    x.rn = (insn >> 16) & 15u;
    x.rd = (insn >> 12) & 15u;
    x.pre = (insn >> 24) & 1u;
    x.up = (insn >> 23) & 1u;
    x.load = (insn >> 20) & 1u;
    x.user = 0;
    x.sign = 0;

    if (cls == 4) {
        struct blk b;
        b.rn = x.rn;
        b.list = insn & 0xffffu;
        b.load = x.load;
        b.pre = x.pre;
        b.up = x.up;
        b.wb = (insn >> 21) & 1u;
        b.s = (insn >> 22) & 1u;
        return do_block(c, &b);
    }
    if (cls == 2 || cls == 3) {
        if (cls == 3 && (insn & 0x10u))
            return LDST_UNSUP;
        x.size = (insn & (1u << 22)) ? 1u : 4u;
        x.wb = !x.pre || ((insn >> 21) & 1u);
        x.user = !x.pre && ((insn >> 21) & 1u);
        x.off = cls == 3 ? shifted(c, insn, cpsr) : insn & 0xfffu;
        return do_single(c, &x);
    }
    if (cls == 0 && (insn & 0x90u) == 0x90u && (insn & 0x60u)) {
        unsigned sh = (insn >> 5) & 3u;
        if (!x.load && sh != 1)
            return LDST_UNSUP;
        if (!(insn & (1u << 22)) && (insn & 0xf00u))
            return LDST_UNSUP;
        x.size = sh == 2 ? 1u : 2u;
        x.sign = sh >= 2;
        x.wb = !x.pre || ((insn >> 21) & 1u);
        x.off = (insn & (1u << 22)) ? ((insn >> 4) & 0xf0u) | (insn & 0xfu) : reg(c, c->mode, insn & 15u);
        return do_single(c, &x);
    }
    return LDST_UNSUP;
}

static int thumb_exec(struct ctx *c, uint32_t insn)
{
    struct xfer x;
    struct blk b;
    unsigned top = (insn >> 12) & 15u;

    x.rn = 0; x.rd = 0; x.size = 4; x.load = 0; x.sign = 0; x.pre = 1; x.up = 1; x.wb = 0; x.user = 0; x.off = 0;

    switch (top) {
    case 4:
        if (!(insn & 0x800u))
            return LDST_UNSUP;
        x.rn = 15; x.rd = (insn >> 8) & 7u; x.load = 1; x.off = (insn & 0xffu) << 2;
        return do_single(c, &x);
    case 5:
        x.rn = (insn >> 3) & 7u; x.rd = insn & 7u;
        x.off = reg(c, c->mode, (insn >> 6) & 7u);
        if (!(insn & 0x200u)) {
            x.load = (insn >> 11) & 1u;
            x.size = (insn & 0x400u) ? 1u : 4u;
        } else {
            unsigned op = (insn >> 10) & 3u;
            x.load = op != 0;
            x.size = op == 1 ? 1u : 2u;
            x.sign = op == 1 || op == 3;
        }
        return do_single(c, &x);
    case 6:
    case 7:
        x.rn = (insn >> 3) & 7u; x.rd = insn & 7u; x.load = (insn >> 11) & 1u;
        x.size = (insn & 0x1000u) ? 1u : 4u;
        x.off = ((insn >> 6) & 31u) * x.size;
        return do_single(c, &x);
    case 8:
        x.rn = (insn >> 3) & 7u; x.rd = insn & 7u; x.load = (insn >> 11) & 1u;
        x.size = 2;
        x.off = ((insn >> 6) & 31u) * 2u;
        return do_single(c, &x);
    case 9:
        x.rn = 13; x.rd = (insn >> 8) & 7u; x.load = (insn >> 11) & 1u; x.off = (insn & 0xffu) << 2;
        return do_single(c, &x);
    case 11:
        if ((insn & 0x0600u) != 0x0400u)
            return LDST_UNSUP;
        b.rn = 13; b.load = (insn >> 11) & 1u; b.s = 0; b.wb = 1;
        b.pre = !b.load; b.up = b.load;
        b.list = insn & 0xffu;
        if (insn & 0x100u)
            b.list |= b.load ? 0x8000u : 0x4000u;
        return do_block(c, &b);
    case 12:
        b.rn = (insn >> 8) & 7u; b.load = (insn >> 11) & 1u; b.s = 0; b.wb = 1; b.pre = 0; b.up = 1;
        b.list = insn & 0xffu;
        return do_block(c, &b);
    default:
        return LDST_UNSUP;
    }
}

static int ldst_run(uint32_t insn, int thumb, uint32_t pc, uint32_t cpsr, const struct ldst_ops *ops,
                    struct ldst_result *res, int uldm)
{
    struct ctx c;
    int s;

    res->status = LDST_UNSUP;
    res->next_pc = 0;
    res->thumb = 0;
    res->spsr_restore = 0;
    res->fault_va = 0;
    res->fsr = 0;

    c.ops = ops;
    c.res = res;
    c.pc = pc;
    c.mode = cpsr & 0x1fu;
    c.thumb = thumb != 0;
    c.nw = 0;
    c.pc_set = 0;
    c.pc_val = 0;
    c.spsr = 0;
    c.uldm = uldm;

    s = c.thumb ? thumb_exec(&c, insn & 0xffffu) : arm_exec(&c, insn, cpsr);
    if (s == LDST_DONE)
        return finish(&c);
    res->status = s;
    return s;
}

int ldst_exec(uint32_t insn, int thumb, uint32_t pc, uint32_t cpsr,
              const struct ldst_ops *ops, struct ldst_result *res)
{
    return ldst_run(insn, thumb, pc, cpsr, ops, res, 0);
}

// the base ends at its writeback value, which returns the A9's push {.., sp, ..} to the sp it started from
int ldst_uldm(uint32_t insn, uint32_t pc, uint32_t cpsr, const struct ldst_ops *ops, struct ldst_result *res)
{
    return ldst_run(insn, 0, pc, cpsr, ops, res, 1);
}
