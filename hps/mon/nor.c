// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/hyb_regs.h"
#include "../common/status.h"
#include "nor.h"

enum {
    ST_ARRAY = 0,
    ST_U1,
    ST_U2,
    ST_PROG,
    ST_E1,
    ST_E2,
    ST_E3,
    ST_ID
};

void nor_reset(struct nor *n)
{
    n->st = ST_ARRAY;
    n->susp = 0;
    n->busy = 0;
    n->esec = 0;
    n->elen = 0;
}

void nor_init(struct nor *n)
{
    nor_reset(n);
    n->program = 0;
    n->erase = 0;
    n->stray = 0;
}

int nor_sector(uint32_t off, uint32_t *base, uint32_t *len)
{
    if (off >= NOR_SIZE)
        return -1;
    *len = (off & 0x1FFFFFu) >= 0x1F0000u ? 0x4000u : 0x10000u;
    *base = off & ~(*len - 1u);
    return 0;
}

static void mark(struct nor_fx *fx, uint32_t lo, uint32_t hi)
{
    if (fx->lo == fx->hi) {
        fx->lo = lo;
        fx->hi = hi;
        return;
    }
    if (lo < fx->lo)
        fx->lo = lo;
    if (hi > fx->hi)
        fx->hi = hi;
}

static void put(uint16_t *mem, uint32_t off, uint16_t v, struct nor_fx *fx)
{
    uint16_t x = mem[off >> 1] ^ v;

    if (!x)
        return;
    mark(fx, (x & 0xFFu) ? off : off + 1u, (x & 0xFF00u) ? off + 2u : off + 1u);
    mem[off >> 1] = v;
}

static int in_esec(const struct nor *n, uint32_t off)
{
    return off - n->esec < n->elen;
}

static uint16_t half_rd(struct nor *n, const uint16_t *mem, uint32_t off)
{
    if (n->st == ST_ID) {
        // ngage_flash.sv:64-65 answers, so [s+4] (sector protect) reads 0
        switch (off & 0x7FEu) {
        case 0x00u: return 0x0001u;
        case 0x02u: return 0x257Eu;
        case 0x1Cu: return 0x2502u;
        case 0x1Eu: return 0x2500u;
        default: return 0;
        }
    }
    if (n->busy && !n->susp) {
        n->busy--;
        return NOR_ST_BUSY;
    }
    if (n->susp && in_esec(n, off))
        return NOR_ST_SUSP;
    return mem[off >> 1];
}

static void program(struct nor *n, uint16_t *mem, uint32_t off, uint16_t d, struct nor_fx *fx)
{
    uint16_t old = mem[off >> 1];

    n->st = ST_ARRAY;
    if (n->susp && in_esec(n, off)) {
        fx->stray++;
        return;
    }
    put(mem, off, old & d, fx);
    fx->program++;
}

static void erase(struct nor *n, uint16_t *mem, uint32_t off, struct nor_fx *fx)
{
    uint32_t i, end;

    (void)nor_sector(off, &n->esec, &n->elen);
    end = n->esec + n->elen;
    for (i = n->esec; i < end; i += 2u)
        put(mem, i, 0xFFFFu, fx);
    n->busy = NOR_BUSY_READS;
    fx->erase++;
}

static void half_wr(struct nor *n, uint16_t *mem, uint32_t off, uint16_t d, struct nor_fx *fx)
{
    uint32_t a = off & 0xFFFu;
    uint8_t c = (uint8_t)d;
    uint8_t st = n->st;

    if (st == ST_PROG) {
        program(n, mem, off, d, fx);
        return;
    }
    if (c == 0xF0u || c == 0xFFu) {
        nor_reset(n);
        return;
    }
    if (n->busy && !n->susp) {
        if (c == 0xB0u)
            n->susp = 1;
        else
            fx->stray++;
        return;
    }
    n->st = ST_ARRAY;
    if (st == ST_ARRAY && c == 0xAAu && a == 0xAAAu)
        n->st = ST_U1;
    else if (st == ST_ARRAY && c == 0x30u && n->susp) {
        n->susp = 0;
        n->busy = NOR_BUSY_READS;
    } else if (st == ST_U1 && c == 0x55u && a == 0x554u)
        n->st = ST_U2;
    else if (st == ST_U2 && c == 0xA0u && a == 0xAAAu)
        n->st = ST_PROG;
    else if (st == ST_U2 && c == 0x80u && a == 0xAAAu && !n->susp)
        n->st = ST_E1;
    else if (st == ST_U2 && c == 0x90u && a == 0xAAAu)
        n->st = ST_ID;
    else if (st == ST_E1 && c == 0xAAu && a == 0xAAAu)
        n->st = ST_E2;
    else if (st == ST_E2 && c == 0x55u && a == 0x554u)
        n->st = ST_E3;
    else if (st == ST_E3 && c == 0x30u)
        erase(n, mem, off, fx);
    else if (st != ST_ARRAY || c != 0x60u) {
        if (st == ST_ID)
            n->st = st;
        fx->stray++;
    }
}

int nor_access(struct nor *n, uint16_t *mem, uint32_t off, unsigned size, int write, uint32_t *data,
               struct nor_fx *fx)
{
    uint32_t h, d;

    fx->lo = fx->hi = 0;
    fx->program = fx->erase = fx->stray = 0;
    if ((size != 1u && size != 2u && size != 4u) || off >= NOR_SIZE || (off & (size - 1u)))
        return -1;
    if (write) {
        d = *data;
        if (size == 4u) {
            half_wr(n, mem, off, (uint16_t)d, fx);
            half_wr(n, mem, off + 2u, (uint16_t)(d >> 16), fx);
        } else if (size == 2u)
            half_wr(n, mem, off, (uint16_t)d, fx);
        else if (off & 1u)
            half_wr(n, mem, off - 1u, (uint16_t)(((d & 0xFFu) << 8) | 0xFFu), fx);
        else
            half_wr(n, mem, off, (uint16_t)(0xFF00u | (d & 0xFFu)), fx);
        n->program += fx->program;
        n->erase += fx->erase;
        n->stray += fx->stray;
        return 0;
    }
    if (size == 4u) {
        h = half_rd(n, mem, off);
        *data = h | ((uint32_t)half_rd(n, mem, off + 2u) << 16);
    } else if (size == 2u)
        *data = half_rd(n, mem, off);
    else {
        h = half_rd(n, mem, off & ~1u);
        *data = (off & 1u) ? h >> 8 : h & 0xFFu;
    }
    return 0;
}

uint32_t nor_gen_next(uint32_t gen, int after, int changed)
{
    if (after && !changed)
        return gen - 1u;
    return gen + 1u;
}

// an rbf before HYB VERSION 2 has no FIQ source, so Lffs would hang in its first program
int nor_route_on(uint32_t cdrv, uint32_t version)
{
    return cdrv == NGST_CDRV_MAGIC && version >= HYB_VERSION_FIQ;
}
