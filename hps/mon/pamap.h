// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_PAMAP_H
#define NGH_PAMAP_H

#include <stdint.h>

enum pam_kind {
    PAM_RAM = 0,
    PAM_ROM,
    PAM_FWD
};

#define PAM_PROFILE_NORMAL 0
#define PAM_PROFILE_TEST   1

struct pam_ctx {
    uint32_t rom_size;
    uint8_t flash_array[3];
    uint8_t profile;
};

struct pam_res {
    uint8_t kind;
    uint8_t ro;
    uint8_t uniform_4k;
    uint8_t uniform_1m;
    uint16_t tail_fill;
    uint32_t host_pa;
};

struct pam_res pamap_page(uint32_t pa, int write, const struct pam_ctx *ctx);
struct pam_res pamap_lookup(uint32_t pa, int write, const struct pam_ctx *ctx);

#endif
