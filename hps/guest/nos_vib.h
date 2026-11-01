// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_NOS_VIB_H
#define NGH_NOS_VIB_H

#include <stdint.h>

// in NOS bss past the earlier stub's PM, IFACE and VTBL (0x80006F00..0x80007163), zero at boot
#define NOS_VIB_VA     0x80007200u
#define NOS_VIB_MAGIC  0x3142564Eu
#define NOS_VIB_WORDS  12u

#define NOS_VIB_LEVEL_MASK 0xFFu
#define NOS_VIB_ON         0x100u
#define NOS_VIB_ANS_SHIFT  16u
#define NOS_VIB_LEVEL(w)    ((w) & NOS_VIB_LEVEL_MASK)
#define NOS_VIB_IS_ON(w)    (((w) & NOS_VIB_ON) != 0)
#define NOS_VIB_ANSWERED(w) ((w) >> NOS_VIB_ANS_SHIFT)
#define NOS_VIB_LEVEL_INIT 100u

struct nos_vib {
    uint32_t magic;
    uint32_t vib;
    uint32_t sends;
    uint32_t acc_reqs;
    uint32_t completions;
    uint32_t posted;
    uint32_t cancels;
    uint32_t replaced;
    uint32_t bad;
    uint32_t created;
    uint32_t closed;
    uint32_t last_req;
};

#define NOS_VIB_OFF_MAGIC 0u
#define NOS_VIB_OFF_VIB   4u

#endif
