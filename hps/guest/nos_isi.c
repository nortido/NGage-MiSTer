// SPDX-License-Identifier: GPL-3.0-or-later
#include "nos_isi.h"

#ifndef NOS_MEM
#define NOS_MEM(a) ((volatile uint8_t *)(uintptr_t)(a))
#endif

#ifndef NOS_VIB_STORE
#define NOS_VIB_STORE(g, w) ((g)->v.vib = (w))
#endif

#define RD32(a) (*(volatile uint32_t *)NOS_MEM(a))
#define WR32(a, v) (*(volatile uint32_t *)NOS_MEM(a) = (v))
#define DES_TYPE(w) ((w) >> 28)
#define DES_LEN(w) ((w) & 0x0FFFFFFFu)
#define DES_TOP(w) ((w) & 0xF0000000u)

// mask bit -> light sub-block id, HWTRICKS 0x10002058
static const uint8_t light_sub[NOS_LIGHT_SUBS] = { 0x01, 0x05, 0x06, 0x02, 0x07, 0x03, 0x04, 0x00 };

static volatile struct nos_g *glob(void)
{
    volatile struct nos_g *g = (volatile struct nos_g *)NOS_MEM(NOS_VIB_VA);
    volatile uint32_t *w = (volatile uint32_t *)g;
    unsigned i;
    if (g->v.magic != NOS_VIB_MAGIC) {
        for (i = 0; i < sizeof(struct nos_g) / 4; i++)
            w[i] = 0;
        NOS_VIB_STORE(g, NOS_VIB_LEVEL_INIT);
        g->v.magic = NOS_VIB_MAGIC;
    }
    return g;
}

static volatile struct nos_ch *chst(uint32_t obj)
{
    return (volatile struct nos_ch *)NOS_MEM(obj + NOS_OBJ_OLD);
}

// TDesC8::Ptr's layouts by hand (types >= 5 panic there); 0 = unusable
static uint32_t des_src(uint32_t d, uint32_t *n)
{
    uint32_t w = RD32(d), p;
    switch (DES_TYPE(w)) {
    case 0:
        p = d + 4;
        break;
    case 1:
        p = RD32(d + 4);
        break;
    case 2:
    case 4:
        if (DES_LEN(w) > RD32(d + 4))
            return 0;
        p = RD32(d + 8);
        if (p && DES_TYPE(w) == 4)
            p += 4;
        break;
    case 3:
        if (DES_LEN(w) > RD32(d + 4))
            return 0;
        p = d + 8;
        break;
    default:
        return 0;
    }
    *n = DES_LEN(w);
    return p;
}

static void deliver(volatile struct nos_g *g, volatile struct nos_ch *c)
{
    uint32_t d = c->rx, w, t, p, i, len = c->reply_len;
    int32_t r = NOS_ERR_ARG;
    c->rx_pend = 0;
    c->rx = 0;
    if (d) {
        w = RD32(d);
        t = DES_TYPE(w);
        if (t >= 2 && t <= 4) {
            p = t == 3 ? d + 8 : RD32(d + 8);
            if (!p || DES_LEN(w) > RD32(d + 4)) {
                r = NOS_ERR_ARG;
            } else if (RD32(d + 4) < len) {
                r = NOS_ERR_OVF;
            } else {
                if (t == 4) {
                    WR32(p, DES_TOP(RD32(p)) | len);
                    p += 4;
                }
                for (i = 0; i < len; i++)
                    *NOS_MEM(p + i) = c->reply[i];
                WR32(d, DES_TOP(w) | len);
                c->queued = 0;
                r = 0;
            }
        }
    }
    // a failed receive keeps the reply for the next one, as phonet keeps a message on KErrOverflow
    if (r)
        g->v.bad++;
    g->v.completions++;
    nos_complete(c->chan, 0, r);
}

uint32_t nos_ctor(uint32_t obj, uint32_t unit)
{
    volatile struct nos_g *g = glob();
    volatile struct nos_ch *c = chst(obj);
    unsigned i;
    WR32(obj + 4, (uint32_t)(uintptr_t)nos_vtbl);
    c->chan = 0;
    c->unit = unit;
    c->rx_pend = 0;
    c->rx = 0;
    c->queued = 0;
    c->reply_len = 0;
    for (i = 0; i < sizeof c->reply; i++)
        c->reply[i] = 0;
    g->v.created++;
    return obj;
}

uint32_t nos_attach(uint32_t chan, uint32_t unit)
{
    uint32_t obj = RD32(chan + NOS_CHAN_OBJ);
    (void)unit;
    if (obj)
        chst(obj)->chan = chan;
    return 0;
}

uint32_t nos_dtor(uint32_t obj, uint32_t flags)
{
    volatile struct nos_g *g = glob();
    volatile struct nos_ch *c = chst(obj);
    // vibra state is the phone's, not the channel's: HWTRICKS closes its channel inside every call
    c->rx_pend = 0;
    c->rx = 0;
    c->queued = 0;
    g->v.closed++;
    // the deleting form frees as the kernel's own deleting destructors do (0x503a10ac), the object untouched after
    if (flags & 1u)
        nos_delete(obj);
    return 0;
}

uint32_t nos_cancel(uint32_t obj, uint32_t n)
{
    volatile struct nos_g *g = glob();
    volatile struct nos_ch *c = chst(obj);
    // the kernel calls this once per cancelled request number (0x503350f8); the receive is request 0
    if (n == NOS_REQ_RECV) {
        c->rx_pend = 0;
        c->rx = 0;
    }
    g->v.cancels++;
    return 0;
}

uint32_t nos_request(uint32_t obj, uint32_t req, uint32_t a1, uint32_t a2)
{
    volatile struct nos_g *g;
    volatile struct nos_ch *c;
    (void)a2;
    if (req != NOS_REQ_RECV)
        return 0;
    g = glob();
    c = chst(obj);
    g->v.posted++;
    c->rx = a1;
    c->rx_pend = 1;
    if (c->queued)
        deliver(g, c);
    return 0;
}

uint32_t nos_control(uint32_t obj, uint32_t fn, uint32_t a1, uint32_t a2)
{
    volatile struct nos_g *g;
    volatile struct nos_ch *c;
    uint32_t p, n = 0, i, w, lv, len, mask, cnt;
    uint8_t q[NOS_LIGHT_REQ];
    (void)a2;
    if (fn != NOS_FN_SEND)
        return 0;
    g = glob();
    c = chst(obj);
    g->v.sends++;
    p = a1 ? des_src(a1, &n) : 0;
    if (!p) {
        g->v.bad++;
        return 0;
    }
    if (n < 10)
        return 0;
    for (i = 0; i < NOS_LIGHT_REQ; i++)
        q[i] = i < n ? *NOS_MEM(p + i) : 0;
    g->v.last_req = q[3] | (uint32_t)q[9] << 8 | (uint32_t)q[10] << 16 | (c->unit & 0xFFu) << 24;
    if (q[3] != NOS_RES_ACC && q[3] != NOS_RES_LIGHT)
        return 0;

    c->reply[0] = q[0];
    c->reply[1] = q[2];
    c->reply[2] = q[1];
    c->reply[3] = q[3];
    len = NOS_REPLY_LEN;
    if (q[3] == NOS_RES_LIGHT && q[9] == NOS_LIGHT_GET) {
        // the light status response: count at [11], one sub-block per set mask bit 0-7, ascending
        mask = n >= NOS_LIGHT_REQ ? (uint32_t)q[12] << 24 | (uint32_t)q[13] << 16 | (uint32_t)q[14] << 8 | q[15] : 0;
        len = NOS_LIGHT_HEAD;
        cnt = 0;
        for (i = 0; i < NOS_LIGHT_SUBS; i++) {
            if (!(mask >> i & 1u))
                continue;
            c->reply[len] = light_sub[i];
            c->reply[len + 1] = NOS_SUB_LEN;
            c->reply[len + 2] = NOS_LIGHT_V1;
            c->reply[len + 3] = 0;
            len += NOS_SUB_LEN;
            cnt++;
        }
        c->reply[11] = (uint8_t)cnt;
    }
    c->reply_len = len;
    if ((q[4] | (uint32_t)q[5] << 8) != n - 6 && ((uint32_t)q[4] << 8 | q[5]) == n - 6) {
        c->reply[4] = (uint8_t)((len - 6) >> 8);
        c->reply[5] = (uint8_t)(len - 6);
    } else {
        c->reply[4] = (uint8_t)(len - 6);
        c->reply[5] = (uint8_t)((len - 6) >> 8);
    }
    c->reply[6] = q[7];
    c->reply[7] = q[6];
    c->reply[8] = q[8];
    c->reply[9] = (uint8_t)(q[9] + 1);
    c->reply[10] = 0;
    if (c->queued)
        g->v.replaced++;
    c->queued = 1;

    w = g->v.vib + (1u << NOS_VIB_ANS_SHIFT);
    if (q[3] == NOS_RES_ACC && n >= 11) {
        if (q[9] == NOS_MSG_ONOFF) {
            w &= ~NOS_VIB_ON;
            if (q[10])
                w |= NOS_VIB_ON;
        } else if (q[9] == NOS_MSG_LEVEL) {
            lv = q[10] > NOS_LEVEL_MAX ? NOS_LEVEL_MAX : q[10];
            w = (w & ~NOS_VIB_LEVEL_MASK) | lv;
        }
    }
    NOS_VIB_STORE(g, w);
    g->v.acc_reqs += q[3] == NOS_RES_ACC ? NOS_ACC_ONE_1C : NOS_ACC_ONE_3A;

    if (c->rx_pend)
        deliver(g, c);
    return 0;
}
