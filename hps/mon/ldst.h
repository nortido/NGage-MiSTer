// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_LDST_H
#define NGH_LDST_H

#include <stdint.h>

enum { LDST_DONE = 1, LDST_FAULT = 2, LDST_UNSUP = 3 };

struct ldst_ops {
    void *ctx;
    // guest SCTLR.A: misaligned word/halfword faults, FSR 1, before any mem call
    int align;
    uint32_t (*get_reg)(void *ctx, unsigned mode, unsigned n);
    void (*set_reg)(void *ctx, unsigned mode, unsigned n, uint32_t v);
    uint32_t (*mem)(void *ctx, uint32_t va, unsigned size, int write, int user, uint32_t *data);
};

struct ldst_result {
    int status;
    uint32_t next_pc;
    int thumb;
    int spsr_restore;
    uint32_t fault_va;
    uint32_t fsr;
};

int ldst_exec(uint32_t insn, int thumb, uint32_t pc, uint32_t cpsr,
              const struct ldst_ops *ops, struct ldst_result *res);
int ldst_uldm(uint32_t insn, uint32_t pc, uint32_t cpsr, const struct ldst_ops *ops, struct ldst_result *res);

#endif
