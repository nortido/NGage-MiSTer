// SPDX-License-Identifier: GPL-3.0-or-later
#include "ngl_cpu1.h"

enum {
    NGL_LIVE_MS = 100,
    NGL_ONLINE_MS = 2000,
    NGL_ONLINE_POLL_MS = 10,
    NGL_RETRY_MS = 100,
    NGL_SETTLE_MS = 50,
    NGL_PARK_TRIES = 1000,
    NGL_C1RST_MS = 100
};

_Static_assert(sizeof(struct ngst) <= PL_C1RST_FLAG - PL_STATUS_BASE, "D41 words outside NGST");

static const char *const ngl_err_str[NGL_NERR] = {
    "ok",
    "bad argument",
    "session live",
    "cannot set cpu affinity",
    "cpu1 hotplug access failed",
    "register write failed",
    "cpu1 entry readback mismatch; cpu1 left in reset",
    "cpu1 did not come back online",
    "no park ack",
    "DDR write failed",
    "monitor readback mismatch",
    "patch count readback mismatch",
    "the running core is not NGage",
    "bridges",
    "FPGA register file magic mismatch",
    "ROM not loaded",
    "no card image",
    "cpu1 is owned by Linux",
    "fpga not in user mode",
};

const char *ngl_strerror(int e)
{
    if (e > 0 || e <= -NGL_NERR)
        return "unknown error";
    return ngl_err_str[-e];
}

static void ngl_say(const struct ngl_io *io, const char *msg)
{
    if (io->log)
        io->log(io->ctx, msg);
}

int ngl_mon_live(const struct ngl_io *io)
{
    uint32_t st = io->rd32(io->ctx, NGL_ST(state)), hb;

    if (st != NGST_STATE_BOOT && st != NGST_STATE_RUN)
        return 0;
    hb = io->rd32(io->ctx, NGL_ST(heartbeat));
    io->sleep_ms(io->ctx, NGL_LIVE_MS);
    return io->rd32(io->ctx, NGL_ST(heartbeat)) != hb;
}

static int ngl_online_is(const struct ngl_io *io, char want)
{
    char b[8];
    int n = io->sys_read(io->ctx, NGL_CPU1_ONLINE, b, sizeof b);

    if (n < 1)
        return -1;
    return b[0] == want;
}

static int ngl_cpu1_hold(const struct ngl_io *io)
{
    uint32_t v = io->rd32(io->ctx, PL_RSTMGR_MPUMODRST);

    return io->wr32(io->ctx, PL_RSTMGR_MPUMODRST, v | PL_MPUMODRST_CPU1) ? NGL_E_REG : NGL_OK;
}

int ngl_cpu1_take(const struct ngl_io *io, uint32_t entry, int force)
{
    uint32_t v;
    int on;

    if (!force && ngl_mon_live(io))
        return NGL_E_LIVE;
    if (io->set_affinity(io->ctx, 0))
        return NGL_E_AFFINITY;
    on = ngl_online_is(io, '1');
    if (on < 0)
        return NGL_E_SYSFS;
    // cpu1 may still be on its way into WFI after the offline write returns; Linux's online 1 resets it at once too
    if (on) {
        if (io->sys_write(io->ctx, NGL_CPU1_ONLINE, "0"))
            return NGL_E_SYSFS;
        io->sleep_ms(io->ctx, NGL_SETTLE_MS);
    }
    if (io->sys_write(io->ctx, NGL_CPU1_ONLINE, "1") || io->sys_write(io->ctx, NGL_CPU1_ONLINE, "0"))
        return NGL_E_SYSFS;
    io->sleep_ms(io->ctx, NGL_SETTLE_MS);
    if (ngl_cpu1_hold(io))
        return NGL_E_REG;
    // a return that died inside its c1rst window leaves the flag set: start.S would never init the monitor
    if (io->wr32(io->ctx, PL_C1RST_FLAG, 0))
        return NGL_E_MEM;
    if (io->wr32(io->ctx, PL_SYSMGR_CPU1STARTADDR, entry))
        return NGL_E_REG;
    if (io->rd32(io->ctx, PL_SYSMGR_CPU1STARTADDR) != entry)
        return NGL_E_READBACK;
    v = io->rd32(io->ctx, PL_RSTMGR_MPUMODRST);
    if (io->wr32(io->ctx, PL_RSTMGR_MPUMODRST, v & ~PL_MPUMODRST_CPU1))
        return NGL_E_REG;
    return NGL_OK;
}

static int ngl_online_wait(const struct ngl_io *io)
{
    uint32_t t = 0;

    for (;;) {
        if (ngl_online_is(io, '1') == 1)
            return 1;
        if (t >= NGL_ONLINE_MS)
            return 0;
        io->sleep_ms(io->ctx, NGL_ONLINE_POLL_MS);
        t += NGL_ONLINE_POLL_MS;
    }
}

static int ngl_return_body(const struct ngl_io *io)
{
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        if (ngl_cpu1_hold(io))
            return NGL_E_REG;
        if (attempt)
            io->sleep_ms(io->ctx, NGL_RETRY_MS);
        if (!io->sys_write(io->ctx, NGL_CPU1_ONLINE, "1") && ngl_online_wait(io)) {
            if (io->set_affinity(io->ctx, 1))
                ngl_say(io, "cannot restore cpu1 affinity");
            return NGL_OK;
        }
    }
    ngl_say(io, ngl_strerror(NGL_E_ONLINE));
    return NGL_E_ONLINE;
}

static int ngl_c1rst_start(const struct ngl_io *io)
{
    uint32_t v;

    if (io->wr32(io->ctx, PL_C1RST_DONE, 0) || io->wr32(io->ctx, PL_C1RST_FLAG, PL_C1RST_ARM) ||
        io->wr32(io->ctx, PL_SYSMGR_CPU1STARTADDR, PL_MON_BASE) ||
        io->rd32(io->ctx, PL_SYSMGR_CPU1STARTADDR) != PL_MON_BASE)
        return -1;
    v = io->rd32(io->ctx, PL_RSTMGR_MPUMODRST);
    return io->wr32(io->ctx, PL_RSTMGR_MPUMODRST, v & ~PL_MPUMODRST_CPU1) ? -1 : 0;
}

// only core 1 reaches its banked IGROUPR0 and GICC_CTLR, so it runs the monitor entry start.S c1rst once
static void ngl_c1rst(const struct ngl_io *io)
{
    uint32_t t = 0;

    if (ngl_cpu1_hold(io))
        return;
    if (ngl_c1rst_start(io)) {
        ngl_say(io, "cpu1 GIC reset skipped");
    } else {
        while (io->rd32(io->ctx, PL_C1RST_DONE) != PL_C1RST_DONE_V) {
            if (t >= NGL_C1RST_MS) {
                ngl_say(io, "cpu1 GIC reset: no done word");
                break;
            }
            io->sleep_ms(io->ctx, 1);
            t++;
        }
    }
    // clear only with cpu1 held: a late core 1 must never see the flag gone and boot the monitor
    if (!ngl_cpu1_hold(io))
        (void)io->wr32(io->ctx, PL_C1RST_FLAG, 0);
}

// a released core 1 fetches from PA 0: Linux's headsmp.S secondary_trampoline (LE) from the take's online cycle
// must still be there; word 2 holds PAGE_OFFSET and is not checked
static int ngl_tramp_ok(const struct ngl_io *io)
{
    static const uint32_t w[][2] = {
        { 0x0u, 0xE28F0010u }, { 0x4u, 0xE8900006u }, { 0xCu, 0xE5923000u }, { 0x10u, 0xE5934000u },
        { 0x14u, 0xE12FFF14u }
    };
    size_t i;

    for (i = 0; i < sizeof w / sizeof w[0]; i++)
        if (io->rd32(io->ctx, w[i][0]) != w[i][1])
            return 0;
    return 1;
}

// a park ack means irq_hw_park restored the GIC; a monitor image without the marker has no entry
static int ngl_back_body(const struct ngl_io *io, int parked)
{
    // park() acks PARKED before its publish() L2 sync store: let core 1 reach its WFE before the hold
    if (parked)
        io->sleep_ms(io->ctx, NGL_SETTLE_MS);
    if (!parked && io->rd32(io->ctx, PL_C1RST_MARK_PA) == PL_C1RST_MARK) {
        if (ngl_tramp_ok(io))
            ngl_c1rst(io);
        else
            ngl_say(io, "cpu1 GIC reset skipped: no trampoline at PA 0");
    }
    return ngl_return_body(io);
}

static int ngl_parked(const struct ngl_io *io)
{
    return io->rd32(io->ctx, NGL_ST(state)) == NGST_STATE_PARKED;
}

static int ngl_park_body(const struct ngl_io *io)
{
    int i, acked = 0, rc;

    (void)io->wr32(io->ctx, NGL_ST(park_req), NGST_PARK_REQUEST);
    for (i = 0; i < NGL_PARK_TRIES; i++) {
        if (io->rd32(io->ctx, NGL_ST(state)) == NGST_STATE_PARKED) {
            acked = 1;
            break;
        }
        io->sleep_ms(io->ctx, 1);
    }
    if (!acked)
        ngl_say(io, ngl_strerror(NGL_E_NO_PARK_ACK));
    rc = ngl_back_body(io, acked);
    if (rc)
        return rc;
    return acked ? NGL_OK : NGL_E_NO_PARK_ACK;
}

int ngl_cpu1_return(const struct ngl_io *io)
{
    // an unreadable or non-0 online means Linux owns cpu1: a reset there would kill a running cpu
    return ngl_online_is(io, '0') == 1 ? ngl_back_body(io, ngl_parked(io)) : NGL_E_CPU1_LINUX;
}

int ngl_mon_park(const struct ngl_io *io)
{
    return ngl_online_is(io, '0') == 1 ? ngl_park_body(io) : NGL_E_CPU1_LINUX;
}

int ngl_cpu1_recover(const struct ngl_io *io)
{
    int off = ngl_online_is(io, '0');

    if (off < 0)
        return NGL_E_SYSFS;
    if (!off)
        return NGL_OK;
    if (!(io->rd32(io->ctx, NGL_BRGMODRST) & NGL_BRG_RESET) && ngl_mon_live(io))
        return ngl_park_body(io);
    return ngl_back_body(io, ngl_parked(io));
}
