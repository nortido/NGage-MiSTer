// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/hyb_regs.h"
#include "pamap.h"

#define FWD_PAHI_NONE 0xFFFFFFFFu
#define FWD_FLASH_END 0x06000000u

struct fwd_io {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t off);
    void (*wr)(void *ctx, uint32_t off, uint32_t v);
};

struct fwd {
    uint32_t pahi;
    struct pam_ctx *pam;
};

uint32_t fwd_off(uint32_t pa, unsigned size)
{
    uint32_t acc = size == 1 ? HYB_ACC_BYTE : size == 2 ? HYB_ACC_HALF : HYB_ACC_WORD;

    return HYB_FWD_OFF(pa, acc);
}

uint32_t fwd_mask(uint32_t v, unsigned size)
{
    return size == 1 ? v & 0xFFu : size == 2 ? v & 0xFFFFu : v;
}

void fwd_invalidate(struct fwd *f)
{
    f->pahi = FWD_PAHI_NONE;
}

void fwd_init(struct fwd *f, struct pam_ctx *pam)
{
    f->pam = pam;
    fwd_invalidate(f);
}

uint32_t fwd_flash_sync(struct pam_ctx *c, uint32_t status)
{
    uint32_t m = (status & HYB_ST_FLASH_ARRAY_MASK) >> HYB_ST_FLASH_ARRAY_SHIFT, chg = 0, i;

    for (i = 0; i < 3; i++) {
        uint8_t a = (uint8_t)((m >> i) & 1u);
        if (c->flash_array[i] != a) {
            c->flash_array[i] = a;
            chg |= 1u << i;
        }
    }
    return chg;
}

uint32_t fwd_tail_len(uint32_t rom_size)
{
    return (rom_size & 0xFFFu) ? 0x1000u - (rom_size & 0xFFFu) : 0;
}

// returns the chips whose array mode changed: their shadow pages must go
uint32_t fwd_access(struct fwd *f, const struct fwd_io *io, uint32_t pa, unsigned size, int write, uint32_t *data)
{
    uint32_t hi = HYB_PAHI_OF(pa), off = fwd_off(pa, size);

    if (hi != f->pahi) {
        io->wr(io->ctx, HYB_REG_OFF(HYB_R_PAHI), hi);
        f->pahi = hi;
    }
    if (!write) {
        *data = fwd_mask(io->rd(io->ctx, off), size);
        return 0;
    }
    *data = fwd_mask(*data, size);
    io->wr(io->ctx, off, *data);
    if (pa >= FWD_FLASH_END)
        return 0;
    return fwd_flash_sync(f->pam, io->rd(io->ctx, HYB_REG_OFF(HYB_R_STATUS)));
}
