// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGH_BT_HCI_H
#define NGH_BT_HCI_H

#include <stdint.h>

#define BT_MAGIC   0x314D5442u
#define BT_NAME    248u
#define BT_SET_LEN (25u + BT_NAME)
#define BT_OUT_LEN 520u

struct bt_st {
    uint32_t magic;
    uint32_t cmds;
    uint32_t unknown;
    uint32_t dropped;
    uint32_t acl;
    uint32_t ring_full;
    uint32_t last_idx;
    uint16_t last[8];
    uint16_t unk[4];
    uint8_t set[BT_SET_LEN]; // offset 52: the board harness decodes the head before it
    uint8_t out[BT_OUT_LEN];
};

void bt_hci_init(struct bt_st *st);
unsigned bt_hci_cmd(struct bt_st *st, const uint8_t *pkt, unsigned n);

#endif
