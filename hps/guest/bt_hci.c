// SPDX-License-Identifier: GPL-3.0-or-later
#include "bt_hci.h"

enum { K_CC, K_RESET, K_SET, K_GET, K_INQ, K_PAGE, K_NAME, K_NOCS, K_NOCC };

struct ent {
    uint16_t op;
    uint8_t k;
    uint8_t a;
    uint8_t b;
};

#define Z   21
#define IAC 29
#define NM  25

static const uint8_t info[] = {
    0x01, 0x00, 0x00, 0xee, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x0d, 0x00, 0x00, 0x00,
    0x53, 0x01, 0x30, 0x04, 0x00, 0x01, 0x00,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x01, 0x33, 0x8b, 0x9e,
};

static const uint8_t def_name[] = { 'N', '-', 'G', 'a', 'g', 'e' };

// K_CC: a return bytes from info[b]; K_SET, K_GET: a bytes at set[b];
// K_NOCC: a return bytes, the first b echo the command's handle or BD_ADDR.
static const struct ent tab[] = {
    { 0x0401, K_INQ, 0, 0 },
    { 0x0402, K_CC, 0, Z },
    { 0x0403, K_CC, 0, Z },
    { 0x0404, K_CC, 0, Z },
    { 0x0405, K_PAGE, 0, 0 },
    { 0x0406, K_NOCS, 0, 0 },
    { 0x0407, K_NOCS, 0, 0 },
    { 0x0409, K_NOCS, 0, 0 },
    { 0x040a, K_NOCS, 0, 0 },
    { 0x040b, K_NOCC, 6, 6 },
    { 0x040c, K_NOCC, 6, 6 },
    { 0x040d, K_NOCC, 6, 6 },
    { 0x040e, K_NOCC, 6, 6 },
    { 0x040f, K_NOCS, 0, 0 },
    { 0x0411, K_NOCS, 0, 0 },
    { 0x0413, K_NOCS, 0, 0 },
    { 0x0415, K_NOCS, 0, 0 },
    { 0x0417, K_NOCS, 0, 0 },
    { 0x0419, K_NAME, 0, 0 },
    { 0x041b, K_NOCS, 0, 0 },
    { 0x041d, K_NOCS, 0, 0 },
    { 0x041f, K_NOCS, 0, 0 },
    { 0x0801, K_NOCS, 0, 0 },
    { 0x0803, K_NOCS, 0, 0 },
    { 0x0804, K_NOCS, 0, 0 },
    { 0x0805, K_NOCS, 0, 0 },
    { 0x0806, K_NOCS, 0, 0 },
    { 0x0807, K_NOCS, 0, 0 },
    { 0x0809, K_NOCC, 3, 2 },
    { 0x080b, K_NOCS, 0, 0 },
    { 0x080c, K_NOCC, 4, 2 },
    { 0x080d, K_NOCC, 2, 2 },
    { 0x0c01, K_CC, 0, Z },
    { 0x0c03, K_RESET, 0, Z },
    { 0x0c05, K_CC, 0, Z },
    { 0x0c08, K_NOCC, 2, 2 },
    { 0x0c09, K_GET, 1, 20 },
    { 0x0c0a, K_SET, 1, 20 },
    { 0x0c0b, K_CC, 0, Z },
    { 0x0c0d, K_CC, 4, Z },
    { 0x0c11, K_CC, 1, Z },
    { 0x0c12, K_CC, 2, Z },
    { 0x0c13, K_SET, BT_NAME, NM },
    { 0x0c14, K_GET, BT_NAME, NM },
    { 0x0c15, K_GET, 2, 6 },
    { 0x0c16, K_SET, 2, 6 },
    { 0x0c17, K_GET, 2, 4 },
    { 0x0c18, K_SET, 2, 4 },
    { 0x0c19, K_GET, 1, 0 },
    { 0x0c1a, K_SET, 1, 0 },
    { 0x0c1b, K_GET, 4, 12 },
    { 0x0c1c, K_SET, 4, 12 },
    { 0x0c1d, K_GET, 4, 16 },
    { 0x0c1e, K_SET, 4, 16 },
    { 0x0c1f, K_GET, 1, 8 },
    { 0x0c20, K_SET, 1, 8 },
    { 0x0c21, K_GET, 1, 9 },
    { 0x0c22, K_SET, 1, 9 },
    { 0x0c23, K_GET, 3, 1 },
    { 0x0c24, K_SET, 3, 1 },
    { 0x0c25, K_GET, 2, 10 },
    { 0x0c26, K_SET, 2, 10 },
    { 0x0c27, K_NOCC, 4, 2 },
    { 0x0c28, K_NOCC, 2, 2 },
    { 0x0c29, K_CC, 1, Z },
    { 0x0c2a, K_CC, 0, Z },
    { 0x0c2b, K_CC, 1, Z },
    { 0x0c2c, K_CC, 0, Z },
    { 0x0c2d, K_NOCC, 3, 2 },
    { 0x0c2e, K_CC, 1, Z },
    { 0x0c2f, K_CC, 0, Z },
    { 0x0c31, K_CC, 0, Z },
    { 0x0c33, K_CC, 0, Z },
    { 0x0c36, K_NOCC, 4, 2 },
    { 0x0c37, K_NOCC, 2, 2 },
    { 0x0c38, K_CC, 1, IAC },
    { 0x0c39, K_GET, 4, 21 },
    { 0x0c3a, K_SET, 4, 21 },
    { 0x0c3b, K_CC, 1, Z },
    { 0x0c3c, K_CC, 0, Z },
    { 0x0c3d, K_CC, 1, Z },
    { 0x0c3e, K_CC, 0, Z },
    { 0x1001, K_CC, 8, 6 },
    { 0x1003, K_CC, 8, Z },
    { 0x1005, K_CC, 7, 14 },
    { 0x1007, K_CC, 1, Z },
    { 0x1009, K_CC, 6, 0 },
    { 0x1401, K_NOCC, 4, 2 },
    { 0x1402, K_NOCC, 2, 2 },
    { 0x1403, K_NOCC, 3, 2 },
    { 0x1405, K_NOCC, 3, 2 },
};

static void put(uint8_t *d, const uint8_t *s, unsigned sn, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        d[i] = i < sn ? s[i] : 0;
}

static unsigned ev(uint8_t *o, uint8_t code, unsigned plen)
{
    o[0] = 0x04;
    o[1] = 0x00;
    o[2] = code;
    o[3] = (uint8_t)plen;
    return 4;
}

static unsigned cmd_status(uint8_t *o, uint8_t status, const uint8_t *pkt)
{
    ev(o, 0x0f, 4);
    o[4] = status;
    o[5] = 1;
    o[6] = pkt[2];
    o[7] = pkt[3];
    return 8;
}

static unsigned cmd_complete(uint8_t *o, uint8_t status, const uint8_t *pkt, unsigned ret)
{
    ev(o, 0x0e, 4 + ret);
    o[4] = 1;
    o[5] = pkt[2];
    o[6] = pkt[3];
    o[7] = status;
    return 8;
}

static void defaults(struct bt_st *st)
{
    put(st->set, 0, 0, 21);
    put(st->set + 21, info + IAC, 4, 4);
    put(st->set + NM, def_name, sizeof def_name, BT_NAME);
}

void bt_hci_init(struct bt_st *st)
{
    put((uint8_t *)st, 0, 0, sizeof *st);
    st->magic = BT_MAGIC;
    defaults(st);
}

unsigned bt_hci_cmd(struct bt_st *st, const uint8_t *pkt, unsigned n)
{
    const struct ent *e = 0;
    const uint8_t *p;
    uint8_t *o = st->out;
    unsigned op, plen, i, k;

    if (st->magic != BT_MAGIC)
        bt_hci_init(st);
    if (n >= 1 && pkt[0] == 0x02) {
        st->acl++;
        return 0;
    }
    // hci.dll's TX framing: 01 00 op_lo op_hi plen params
    if (n < 5 || pkt[0] != 0x01 || n != 5u + pkt[4]) {
        st->dropped++;
        return 0;
    }
    op = pkt[2] | (unsigned)pkt[3] << 8;
    plen = pkt[4];
    p = pkt + 5;
    st->cmds++;
    st->last[st->last_idx & 7u] = (uint16_t)op;
    st->last_idx = (st->last_idx + 1u) & 7u;

    for (i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (tab[i].op == op) {
            e = &tab[i];
            break;
        }
    }
    if (!e) {
        if ((op >> 10) == 0x3fu)
            return cmd_complete(o, 0x00, pkt, 0);
        if (st->unknown < 4u)
            st->unk[st->unknown] = (uint16_t)op;
        st->unknown++;
        return cmd_complete(o, 0x01, pkt, 0);
    }

    switch (e->k) {
    case K_RESET:
        defaults(st);
        return cmd_complete(o, 0x00, pkt, 0);
    case K_CC:
        k = cmd_complete(o, 0x00, pkt, e->a);
        put(o + k, info + e->b, e->a, e->a);
        return k + e->a;
    case K_SET:
        // one supported IAC: a longer list is rejected and the stored one kept
        if (op == 0x0c3au && plen != 0 && p[0] > 1u)
            return cmd_complete(o, 0x12, pkt, 0);
        put(st->set + e->b, p, plen < e->a ? plen : e->a, e->a);
        return cmd_complete(o, 0x00, pkt, 0);
    case K_GET:
        k = cmd_complete(o, 0x00, pkt, e->a);
        put(o + k, st->set + e->b, e->a, e->a);
        return k + e->a;
    case K_NOCC:
        k = cmd_complete(o, 0x02, pkt, e->a);
        put(o + k, p, plen < e->b ? plen : e->b, e->a);
        return k + e->a;
    case K_NOCS:
        return cmd_status(o, 0x02, pkt);
    case K_INQ:
        k = cmd_status(o, 0x00, pkt);
        k += ev(o + k, 0x01, 1);
        o[k] = 0x00;
        return k + 1;
    case K_PAGE:
        // Connection Complete: Page Timeout, handle 0, BD_ADDR, link type ACL, no encryption
        k = cmd_status(o, 0x00, pkt);
        k += ev(o + k, 0x03, 11);
        o[k] = 0x04;
        o[k + 1] = 0x00;
        o[k + 2] = 0x00;
        put(o + k + 3, p, plen < 6 ? plen : 6, 6);
        o[k + 9] = 0x01;
        o[k + 10] = 0x00;
        return k + 11;
    default:
        k = cmd_status(o, 0x00, pkt);
        k += ev(o + k, 0x07, 1 + 6 + BT_NAME);
        o[k] = 0x04;
        put(o + k + 1, p, plen < 6 ? plen : 6, 6 + BT_NAME);
        return k + 1 + 6 + BT_NAME;
    }
}
