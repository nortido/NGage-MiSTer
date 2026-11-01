// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_CP15_H
#define NGH_CP15_H

#include <stdint.h>

#define CP15_ID         0x41029200u
#define CP15_CTR        0x0D152152u
#define CP15_CTRL_RESET 0x00000078u
#define CP15_MAX_ACT    4

enum cp15_act_kind {
    CP15_ACT_NOP = 1,
    CP15_ACT_MMU_MODE,
    CP15_ACT_SHADOW_RESET,
    CP15_ACT_VECTOR_BASE,
    CP15_ACT_ALIGN_MODE,
    CP15_ACT_SET_DACR,
    CP15_ACT_CLEAN_D_MVA,
    CP15_ACT_INV_D_MVA,
    CP15_ACT_CLEAN_INV_D_MVA,
    CP15_ACT_CLEAN_INV_D_ALL,
    CP15_ACT_CLEAN_D_SW,
    CP15_ACT_CLEAN_INV_D_SW,
    CP15_ACT_IMB_ALL,
    CP15_ACT_DRAIN,
    CP15_ACT_WFI,
    CP15_ACT_SHADOW_DROP_ALL,
    CP15_ACT_SHADOW_DROP_MVA,
    CP15_ACT_UNDEF,
    CP15_ACT_FATAL_FCSE,
    CP15_ACT_FATAL_UNKNOWN
};

enum cp15_res_kind {
    CP15_RES_NONE = 0,
    CP15_RES_RD,
    CP15_RES_FLAGS
};

struct cp15_state {
    uint32_t control;
    uint32_t ttbr;
    uint32_t dacr;
    uint32_t fsr;
    uint32_t far;
    uint32_t fcse;
    uint32_t ctxid;
    uint32_t c15[8];
    uint32_t mon_domain;
};

struct cp15_action {
    uint32_t kind;
    uint32_t arg;
};

struct cp15_result {
    uint32_t kind;
    uint32_t value;
    uint32_t nact;
    struct cp15_action act[CP15_MAX_ACT];
};

void cp15_reset(struct cp15_state *s, uint32_t mon_domain);
struct cp15_result cp15_exec(struct cp15_state *s, uint32_t insn, uint32_t rd_value, uint32_t nzcv);

#endif
