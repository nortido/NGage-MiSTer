// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/hyb_regs.h"
#include "ngl_fork.h"
#include "ngl_patch.h"
#include "ngl_rsc.h"

#define FK_PATCH_WORDS (PL_PATCH_SIZE / 4u)

static int fk_ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (x != *b)
            return 0;
    }
    return *a == *b;
}

const char *ngl_confstr_skip(const char *entry)
{
    // user_io.cpp:880: Main skips Hide/Disable mask pairs before the entry type
    while (entry && (entry[0] == 'H' || entry[0] == 'D' || entry[0] == 'h' || entry[0] == 'd') && entry[1])
        entry += 2;
    return entry;
}

uint32_t ngl_confstr_load_addr(const char *entry)
{
    char f[32];
    size_t n = 0;
    int field = 0;
    unsigned long v;

    entry = ngl_confstr_skip(entry);
    if (!entry || entry[0] != 'F')
        return 0;
    // menu.cpp:2441-2452: field 3 of the F entry, hex, 0x20000000..0x3FFFFFFF, else the SPI path
    for (; *entry && *entry != ';'; entry++) {
        if (*entry == ',') {
            if (field == 3)
                break;
            field++;
        } else if (field == 3) {
            if (n + 1 >= sizeof f)
                return 0;
            f[n++] = *entry;
        }
    }
    if (!n)
        return 0;
    f[n] = 0;
    v = strtoul(f, NULL, 16);
    if (v < 0x20000000ul || v >= 0x40000000ul)
        return 0;
    return (uint32_t)v;
}

int ngl_core_ok(const char *name, const char *(*entry)(void *ctx, int i), void *ctx)
{
    int i;

    if (!name || !entry || !fk_ci_eq(name, "ngage"))
        return 0;
    // the ngage_init scan; an old FPGA-CPU rbf has the same name but no F address field
    for (i = 2;; i++) {
        const char *p = entry(ctx, i);
        if (!p)
            return 0;
        if (ngl_confstr_skip(p)[0] == 'F')
            return ngl_confstr_load_addr(p) != 0;
    }
}

void ngl_fork_init(struct ngl_fork *f)
{
    memset(f, 0, sizeof *f);
    f->phase = NGL_FK_IDLE;
    f->cdrv_slot = -1;
}

int ngl_fork_image(struct ngl_fork *f, const void *mon, uint32_t mon_len, const char *patch, size_t patch_len,
                   const char *path)
{
    if (!path)
        path = "";
    if (!mon || !mon_len || mon_len > PL_MON_SIZE || !patch || !patch_len || strlen(path) > NGL_SLOT_CHARS)
        return -1;
    f->mon = mon;
    f->mon_len = mon_len;
    f->patch = patch;
    f->patch_len = patch_len;
    strcpy(f->path, path);
    f->image = 1;
    f->since_ms = 0;
    f->phase = NGL_FK_IDLE;
    f->err[0] = 0;
    return 0;
}

void ngl_fork_eject(struct ngl_fork *f)
{
    f->image = 0;
}

static int fk_mon_tt(const struct ngl_fork *f)
{
    const uint8_t *b = f->mon;

    // image word 2, a monitor that re-applies time pairs on a restart; older images hold an instruction there
    return b && f->mon_len >= 12u &&
           ((uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24) ==
               (uint32_t)PL_TT_MARK;
}

// never fails the start; no I/O unless the hook gave a valid clock
static void fk_time(const struct ngl_io *io, struct ngl_fork *f, uint32_t *w, size_t *nw)
{
    struct ngl_pair tp[NGL_KTIME_PAIRS];
    struct ngl_civil c;
    const struct ngl_civil *now = NULL;
    uint32_t rom_size = 0;
    size_t n;
    int rc;

    if (f->now && !f->now(&c))
        now = &c;
    if (now && ngl_civil_ok(now))
        rom_size = io->rd32(io->ctx, HYB_WIN_PHYS + HYB_REG_OFF(HYB_R_ROM_SIZE));
    n = ngl_ktime_build(io->mem_read, io->ctx, rom_size, w, *nw, now, tp, f->tmsg, sizeof f->tmsg, fk_mon_tt(f));
    if (n && (rc = ngl_patch_append(w, FK_PATCH_WORDS, nw, tp, n)))
        snprintf(f->tmsg, sizeof f->tmsg, "time: ROM default kept, append code %d", rc);
}

int ngl_fork_start(const struct ngl_io *io, struct ngl_fork *f, char *err, size_t errcap)
{
    struct ngl_pair pairs[NGL_FORK_PAIRS];
    struct ngl_start s;
    size_t nw = 0, np = 0;
    uint32_t *w;
    int rc;

    f->cdrv_slot = -1;
    w = calloc(FK_PATCH_WORDS, 4);
    if (!w) {
        snprintf(err, errcap, "out of memory");
        return NGL_E_ARG;
    }
    rc = ngl_patch_parse(f->patch, f->patch_len, w, FK_PATCH_WORDS, &nw);
    if (rc || nw < 3) {
        if (rc == NGL_P_ROM)
            snprintf(err, errcap, "wrong base.rom");
        else
            snprintf(err, errcap, "bad patch list (code %d)", rc);
        free(w);
        return NGL_E_ARG;
    }
    // the Starter slots are read from the ROM Main has just loaded into DDR
    if (f->path[0]) {
        if (ngl_rsc_entries(io->mem_read, io->ctx, f->path, &ngl_rsc_k3, pairs, NGL_FORK_PAIRS, &np, err, errcap)) {
            free(w);
            return NGL_E_ARG;
        }
        rc = ngl_patch_append(w, FK_PATCH_WORDS, &nw, pairs, np);
        if (rc) {
            snprintf(err, errcap, "cannot append the Starter rewrite (code %d)", rc);
            free(w);
            return NGL_E_ARG;
        }
    }
    fk_time(io, f, w, &nw);
    s.mon = f->mon;
    s.mon_len = f->mon_len;
    s.patch = w;
    s.patch_nw = (uint32_t)nw;
    s.memcfg = 0;
    s.mile = 0;
    s.log_mode = NGST_LOG_RING;
    s.cdrv = f->cdrv ? NGST_CDRV_MAGIC : 0;
    rc = ngl_mon_start(io, &s, 0);
    if (rc)
        snprintf(err, errcap, "%s", ngl_strerror(rc));
    else
        f->cdrv_slot = ngl_patch_cdrv_slot(w, nw);
    free(w);
    return rc;
}

static int fk_quiet_step(struct ngl_fork *f, const struct ngl_io *io, uint32_t dt_ms, uint32_t *ms, uint32_t *quiet,
                         uint32_t *gen, uint32_t *mmc, uint32_t cap_ms)
{
    uint32_t g = io->rd32(io->ctx, NGL_ST(cdrv_gen));

    *ms += dt_ms;
    if (g != *gen || f->mmc_writes != *mmc) {
        *gen = g;
        *mmc = f->mmc_writes;
        *quiet = 0;
    } else {
        *quiet += dt_ms;
    }
    return *quiet >= NGL_FORK_EXIT_QUIET_MS || *ms >= cap_ms;
}

static void fk_exit_look(struct ngl_fork *f, const struct ngl_io *io)
{
    f->exit_seen = io->rd32(io->ctx, NGL_ST(exit_seen));
    if (!f->exit_seen)
        return;
    f->exit_ms = 0;
    f->exit_quiet = 0;
    f->exit_gen = io->rd32(io->ctx, NGL_ST(cdrv_gen));
    f->exit_mmc = f->mmc_writes;
}

// the file server may still write C: or the MMC after the game process died; park once both stay quiet
static int fk_exit_step(struct ngl_fork *f, const struct ngl_io *io, uint32_t dt_ms)
{
    if (!f->exit_seen) {
        fk_exit_look(f, io);
        return NGL_FE_NONE;
    }
    if (!fk_quiet_step(f, io, dt_ms, &f->exit_ms, &f->exit_quiet, &f->exit_gen, &f->exit_mmc, NGL_FORK_EXIT_CAP_MS))
        return NGL_FE_NONE;
    f->stop_phase = f->phase;
    f->rc = ngl_mon_park(io);
    f->stop_state = io->rd32(io->ctx, NGL_ST(state));
    f->stop_reason = io->rd32(io->ctx, NGL_ST(park_reason));
    f->phase = NGL_FK_DONE;
    return NGL_FE_EXIT;
}

// a select waits by the same rule; the select body parks, so an exit seen meanwhile is only carried into the log
static int fk_select_step(struct ngl_fork *f, const struct ngl_io *io, uint32_t dt_ms)
{
    if (!f->exit_seen)
        fk_exit_look(f, io);
    if (!f->sel_ms) {
        f->sel_gen0 = f->sel_gen = io->rd32(io->ctx, NGL_ST(cdrv_gen));
        f->sel_mmc0 = f->sel_mmc = f->mmc_writes;
    }
    if (!fk_quiet_step(f, io, dt_ms, &f->sel_ms, &f->sel_quiet, &f->sel_gen, &f->sel_mmc, NGL_FORK_SEL_CAP_MS))
        return NGL_FE_NONE;
    f->sel_capped = f->sel_quiet < NGL_FORK_EXIT_QUIET_MS;
    f->sel_pending = 0;
    return NGL_FE_SELECT;
}

int ngl_fork_step(struct ngl_fork *f, const struct ngl_io *io, int mgl_done, uint32_t dt_ms)
{
    int rc;

    if (f->phase == NGL_FK_RUN) {
        if (ngl_watch_step(io, &f->w)) {
            f->phase = NGL_FK_DONE;
            f->rc = f->w.rc;
            return NGL_FE_PARK;
        }
        if (f->sel_pending)
            return fk_select_step(f, io, dt_ms);
        return fk_exit_step(f, io, dt_ms);
    }
    // a guest parked during the wait is quiet: the request goes out on the next step
    if (f->sel_pending) {
        f->sel_pending = 0;
        return NGL_FE_SELECT;
    }
    if (f->phase != NGL_FK_IDLE || !f->image)
        return NGL_FE_NONE;
    f->since_ms = f->since_ms > UINT32_MAX - dt_ms ? UINT32_MAX : f->since_ms + dt_ms;
    // an MGL may still deliver the ROM or the image: start only after its last item
    if (!mgl_done || ngl_fpga_wait(io, 0, 0) != NGL_OK)
        return NGL_FE_NONE;
    // h02 has no CARD bit: settle after our own mount, as the CLI does after Main's fd shows up
    if (f->since_ms < NGL_FORK_SETTLE_MS && ngl_fpga_wait(io, NGL_FW_CARD, 0) != NGL_OK)
        return NGL_FE_NONE;
    f->err[0] = 0;
    f->rc2 = NGL_OK;
    rc = ngl_fork_start(io, f, f->err, sizeof f->err);
    if (rc == NGL_OK) {
        memset(&f->w, 0, sizeof f->w);
        f->w.phase = NGL_W_RUN;
        f->exit_seen = 0;
        f->exit_ms = 0;
        f->phase = NGL_FK_RUN;
        f->rc = NGL_OK;
        f->rom_used = 1;
        return NGL_FE_START;
    }
    f->rc = rc;
    f->phase = NGL_FK_DONE;
    if (rc == NGL_E_LIVE)
        return NGL_FE_LIVE;
    // a take that failed half way leaves cpu1 offline
    f->rc2 = ngl_cpu1_recover(io);
    return NGL_FE_FAIL;
}

int ngl_fork_stop(struct ngl_fork *f, const struct ngl_io *io)
{
    int rc;

    f->stop_phase = f->phase;
    f->stop_state = NGL_FORK_UNREAD;
    f->stop_reason = NGL_FORK_UNREAD;
    if (f->phase == NGL_FK_RUN) {
        rc = ngl_mon_park(io);
        f->stop_state = io->rd32(io->ctx, NGL_ST(state));
        f->stop_reason = io->rd32(io->ctx, NGL_ST(park_reason));
    } else {
        rc = ngl_cpu1_recover(io);
    }
    f->phase = NGL_FK_IDLE;
    return rc;
}

// the monitor patches the ROM in DDR in place: a ROM load is pristine until the next start
void ngl_fork_rom(struct ngl_fork *f)
{
    f->rom_used = 0;
}

int ngl_fork_reload(const struct ngl_fork *f)
{
    return f->rom_used;
}

int ngl_fork_select_req(struct ngl_fork *f, int index, const char *path)
{
    if (f->phase != NGL_FK_RUN)
        return 0;
    // a newer request replaces the path, the wait already running goes on
    if (!f->sel_pending) {
        f->sel_pending = 1;
        f->sel_ms = 0;
        f->sel_quiet = 0;
        f->sel_capped = 0;
    }
    f->sel_index = index;
    snprintf(f->sel_path, sizeof f->sel_path, "%s", path ? path : "");
    return 1;
}

void ngl_fork_select_drop(struct ngl_fork *f)
{
    f->sel_pending = 0;
}
