// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_NOS_ISI_H
#define NGH_NOS_ISI_H

#include <stdint.h>
#include "nos_vib.h"

#define NOS_OBJ_OLD   8u
#define NOS_CHAN_OBJ  0x38u
#define NOS_RES_ACC   0x1Cu
#define NOS_RES_LIGHT 0x3Au
#define NOS_FN_SEND   4u
#define NOS_REQ_RECV  0u
#define NOS_REPLY_LEN 11u
#define NOS_LIGHT_GET 1u
#define NOS_LIGHT_REQ 16u
#define NOS_LIGHT_HEAD 12u
#define NOS_LIGHT_SUBS 8u
#define NOS_SUB_LEN 4u
#define NOS_LIGHT_V1 100u
#define NOS_REPLY_MAX (NOS_LIGHT_HEAD + NOS_LIGHT_SUBS * NOS_SUB_LEN)
#define NOS_MSG_ONOFF 0x0Cu
#define NOS_MSG_LEVEL 0x0Eu
#define NOS_LEVEL_MAX 100u
#define NOS_ERR_ARG   (-6)
#define NOS_ERR_OVF   (-9)

// acc_reqs packs the answered requests per resource: 0x1C [15:0], 0x3A [31:16]
#define NOS_ACC_ONE_1C 0x1u
#define NOS_ACC_ONE_3A 0x10000u
#define NOS_ACC_1C(w)  ((w) & 0xFFFFu)
#define NOS_ACC_3A(w)  ((w) >> 16)

struct nos_ch {
    uint32_t chan;
    uint32_t unit;
    uint32_t rx_pend;
    uint32_t rx;
    uint32_t queued;
    uint32_t reply_len;
    uint8_t reply[NOS_REPLY_MAX];
};

struct nos_g {
    struct nos_vib v;
};

extern const uint32_t nos_vtbl[6];
void nos_complete(uint32_t chan, uint32_t n, int32_t reason);
void nos_delete(uint32_t obj);

uint32_t nos_ctor(uint32_t obj, uint32_t unit);
uint32_t nos_attach(uint32_t chan, uint32_t unit);
uint32_t nos_dtor(uint32_t obj, uint32_t flags);
uint32_t nos_cancel(uint32_t obj, uint32_t n);
uint32_t nos_request(uint32_t obj, uint32_t req, uint32_t a1, uint32_t a2);
uint32_t nos_control(uint32_t obj, uint32_t fn, uint32_t a1, uint32_t a2);

#endif
