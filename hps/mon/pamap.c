// SPDX-License-Identifier: GPL-3.0-or-later
#include "pamap.h"
#include "../common/platform.h"

struct pam_reg {
    uint32_t lo;
    uint32_t hi;
};

// RAM2M offsets of the ngage_soc.sv:197-208 register windows that sel_ram2m excludes
static const struct pam_reg ram2m_regs[] = {
    { 0x120000u, 0x1200FFu },
    { 0x149000u, 0x149FFFu },
    { 0x14F000u, 0x14FFFFu },
    { 0x150000u, 0x150FFFu },
    { 0x121200u, 0x12120Fu },
    { 0x120100u, 0x12027Fu },
    { 0x120A00u, 0x120AFFu },
    { 0x120600u, 0x12063Fu },
    { 0x120C00u, 0x120CFFu }
};

static void flash_page(uint32_t pb, unsigned chip, const struct pam_ctx *c, struct pam_res *r)
{
    uint32_t off = pb & 0x1FFFFFFu;
    uint32_t file;

    if (!c->flash_array[chip])
        return;
    if (chip == 0) {
        if (off < 0x700000u)
            file = off;
        else if (off < 0x800000u)
            file = 0x1300000u + (off - 0x700000u);
        else
            return;
    } else if (chip == 1) {
        if (off >= 0x400000u)
            return;
        file = 0x700000u + off;
    } else {
        if (off >= 0x800000u)
            return;
        file = 0xB00000u + off;
    }
    if (file >= c->rom_size)
        return;
    r->kind = PAM_ROM;
    r->ro = 1;
    r->host_pa = PL_ROM_BASE + file;
    if (c->rom_size - file < 0x1000u)
        r->tail_fill = (uint16_t)(0x1000u - (c->rom_size - file));
}

static void ram2m_page(uint32_t pb, struct pam_res *r)
{
    uint32_t off = pb & 0x1FFFFFu;
    unsigned i;
    int hit = 0;

    for (i = 0; i < sizeof(ram2m_regs) / sizeof(ram2m_regs[0]); i++) {
        if (ram2m_regs[i].lo <= off + 0xFFFu && ram2m_regs[i].hi >= off) {
            hit = 1;
            if (ram2m_regs[i].lo <= off && ram2m_regs[i].hi >= off + 0xFFFu)
                return;
        }
    }
    if (hit) {
        r->uniform_4k = 0;
        return;
    }
    r->kind = PAM_RAM;
    r->host_pa = PL_RAM2M_BASE + off;
}

static void page_cls(uint32_t pb, int write, const struct pam_ctx *c, struct pam_res *r)
{
    r->kind = PAM_FWD;
    r->ro = 0;
    r->uniform_4k = 1;
    r->uniform_1m = 0;
    r->tail_fill = 0;
    r->host_pa = 0;

    if (c->profile == PAM_PROFILE_TEST) {
        if (pb < 0x1000000u) {
            r->kind = PAM_RAM;
            r->host_pa = PL_RAM16_BASE + pb;
        } else if (pb - 0x10000000u < 0x1000000u) {
            r->kind = PAM_RAM;
            r->host_pa = PL_ROM_BASE + (pb - 0x10000000u);
        }
        return;
    }
    if (pb < 0x06000000u) {
        flash_page(pb, pb >> 25, c, r);
        if (write && r->kind == PAM_ROM) {
            r->kind = PAM_FWD;
            r->ro = 0;
            r->tail_fill = 0;
            r->host_pa = 0;
        }
    } else if ((pb >> 24) == 0x0Au) {
        r->kind = PAM_RAM;
        r->host_pa = PL_RAM16_BASE + (pb & 0xFFFFFFu);
    } else if (pb - 0x0C000000u < 0x200000u) {
        ram2m_page(pb, r);
    } else if (pb - 0x20000000u < 0x30000u) {
        r->kind = PAM_RAM;
        r->host_pa = PL_SRAM_BASE + (pb - 0x20000000u);
    }
}

struct pam_res pamap_page(uint32_t pa, int write, const struct pam_ctx *ctx)
{
    struct pam_res r;

    page_cls(pa & ~0xFFFu, write, ctx, &r);
    if (r.kind != PAM_FWD)
        r.host_pa += pa & 0xFFFu;
    return r;
}

// sh_fill asks for the same sections after evrey drop-all; uniform_1m depends only on the MB and this key
static struct {
    struct pam_ctx key;
    int write;
    uint32_t known[128];
    uint32_t uni[128];
} pam_memo;

struct pam_res pamap_lookup(uint32_t pa, int write, const struct pam_ctx *ctx)
{
    struct pam_res r, ref, p;
    uint32_t sec = pa & 0xFFF00000u;
    uint32_t mb = pa >> 20, i;

    r = pamap_page(pa, write, ctx);
    if (pam_memo.write != write || pam_memo.key.rom_size != ctx->rom_size || pam_memo.key.profile != ctx->profile ||
        pam_memo.key.flash_array[0] != ctx->flash_array[0] || pam_memo.key.flash_array[1] != ctx->flash_array[1] ||
        pam_memo.key.flash_array[2] != ctx->flash_array[2]) {
        for (i = 0; i < 128u; i++)
            pam_memo.known[i] = 0;
        pam_memo.key = *ctx;
        pam_memo.write = write;
    }
    if (pam_memo.known[mb >> 5] >> (mb & 31u) & 1u) {
        r.uniform_1m = pam_memo.uni[mb >> 5] >> (mb & 31u) & 1u;
        return r;
    }
    page_cls(sec, write, ctx, &ref);
    r.uniform_1m = 1;
    for (i = 0; i < 256u && r.uniform_1m; i++) {
        page_cls(sec + i * 0x1000u, write, ctx, &p);
        if (p.kind != ref.kind || p.ro != ref.ro || p.tail_fill != 0 || !p.uniform_4k ||
            (p.kind != PAM_FWD && p.host_pa != ref.host_pa + i * 0x1000u))
            r.uniform_1m = 0;
    }
    pam_memo.known[mb >> 5] |= 1u << (mb & 31u);
    pam_memo.uni[mb >> 5] = (pam_memo.uni[mb >> 5] & ~(1u << (mb & 31u))) | (uint32_t)r.uniform_1m << (mb & 31u);
    return r;
}
