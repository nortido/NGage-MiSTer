// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_AWP_H
#define NGH_AWP_H

#include <stdint.h>

// Record layout of kind 7 (sites, T, S, A, STK, K, PROC, IDLE payloads).
#define EVK_AWP         7u
#define AWP_S_KICK      1u
#define AWP_S_M3        2u
#define AWP_S_IRQ       3u
#define AWP_S_MMIO      4u
#define AWP_T_T         0x00u
#define AWP_T_S         0x10u
#define AWP_T_A         0x20u
#define AWP_IX(i)       ((uint32_t)(i) << 6)
#define AWP_F_WGP       0x100u
#define AWP_F_WCHG      0x200u
#define AWP_NMMIO       3u
#define AWP_S_STK       5u
#define AWP_S_PROC      6u
#define AWP_S_IDLE      7u
#define AWP_T_K         0x30u
#define AWP_RIX(i)      ((uint32_t)(i) << 10)
#define AWP_F_TRUNC     0x4000u
#define AWP_NK          6u
#define AWP_NBL         4u
#define AWP_BAD         0xFFFFFFFFu

// main.c EXIT_CUR_THREAD, EXIT_REQ_THREAD, EXIT_SUP_THREAD
#define AWP_CUR         0x80006740u
#define AWP_REQ         0x800003E0u
#define AWP_SUP         0x800066A8u
// the requester while no exec runs (main.c:40-44)
#define AWP_REQ_IDLE    0xDDB3D743u
// thread owner process
#define AWP_TH_OWN      0x18u
// 2 on MH1's three blocked threads, read as the state (UNPROVEN)
#define AWP_TH_STATE    0xFCu
// saved user lr, pc, cpsr (kv_thr.out: 8040f5e8 +0x15C..+0x164 = 503b2484 5032752c 600b0010)
#define AWP_TH_LR       0x15Cu
#define AWP_TH_PC       0x160u
#define AWP_TH_CPSR     0x164u
// trap.c's stack walk (XC_TR_SP, XC_THR_STK, XC_CH_*, XC_P_*, XC_RAMCODE_*, XC_TF_*), copied: trap.c builds it only
// under NGH_XC_TRACE; a test pins the values equal
#define AWP_TH_SP       0x158u
#define AWP_THR_STK     0x2Cu
#define AWP_CH_SIZE     0x18u
#define AWP_CH_BASE     0x28u
#define AWP_CH_HOME     0x80000000u
#define AWP_P_NCH       0xACu
#define AWP_P_CHS       0xB4u
#define AWP_P_NCH_MAX   16u
#define AWP_P_ENT       12u
#define AWP_RAMCODE_LO  0xFE000000u
#define AWP_RAMCODE_HI  0xFFFF0000u
#define AWP_TF_BLK      0x100u
#define AWP_TF_HOME     0x2000u
#define AWP_TF_NOCH     0x4000u
#define AWP_TF_NOENT    0x8000u
#define AWP_TF_RANGE    0x10000u
// our plugin at the pcm site: r6 = D, O1 = [D + 0x10], frames returned so far
#define AWP_D_O1        0x10u
#define AWP_D_POS       0x15Cu
// O1, the player policy object grown to 0x5c bytes
#define AWP_O_PEND      0x38u
#define AWP_O_CHUNK     0x3Cu
#define AWP_O_CREDIT    0x44u
#define AWP_O_BPS       0x48u

struct awp_io {
    void *ctx;
    int (*rd)(void *ctx, uint32_t va, uint32_t *w);
    void (*put)(void *ctx, uint32_t flags, uint32_t pc, uint32_t a, uint32_t b, uint32_t c);
    uint32_t (*gp)(void *ctx);
};

struct awp {
    uint32_t st;
    uint32_t nm;
    uint32_t d;
    uint32_t sup;
    uint32_t w;
    uint32_t wown;
    uint32_t wchg;
    uint32_t th;
    uint32_t eff;
    uint32_t own;
    uint32_t have;
    uint32_t cur;
    uint32_t req;
    uint32_t rq;
    uint32_t idle;
    uint32_t pgp;
};

void awp_m3(struct awp *a, const struct awp_io *io, uint32_t pc, uint32_t d);
void awp_irq(struct awp *a, const struct awp_io *io, uint32_t pc);
void awp_kick(struct awp *a, const struct awp_io *io, uint32_t pc);
void awp_mmio(struct awp *a, const struct awp_io *io, uint32_t pc, uint32_t gm);
void awp_idle(struct awp *a, const struct awp_io *io, uint32_t pc);

#endif
