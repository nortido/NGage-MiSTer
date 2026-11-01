// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_FORK_H
#define NGL_FORK_H

#include <stddef.h>
#include <stdint.h>

#include "ngl_ktime.h"
#include "ngl_mon.h"
#include "ngl_rsc.h"

#define NGL_FORK_SETTLE_MS 300u
#define NGL_FORK_ERR       128
#define NGL_FORK_PAIRS     (5u * NGL_RSC_MAX_WORDS)
#define NGL_FORK_UNREAD    0xFFFFFFFFu
#define NGL_FORK_EXIT_QUIET_MS 2000u
#define NGL_FORK_EXIT_CAP_MS   10000u
#define NGL_FORK_SEL_CAP_MS    8000u
#define NGL_FORK_SEL_PATH      1024

enum { NGL_FK_IDLE = 0, NGL_FK_RUN = 1, NGL_FK_DONE = 2 };
enum { NGL_FE_NONE = 0, NGL_FE_START = 1, NGL_FE_LIVE = 2, NGL_FE_FAIL = 3, NGL_FE_PARK = 4, NGL_FE_EXIT = 5, NGL_FE_SELECT = 6 };

struct ngl_fork {
    int phase;
    int image;
    uint32_t since_ms;
    const void *mon;
    uint32_t mon_len;
    const char *patch;
    size_t patch_len;
    char path[NGL_SLOT_CHARS + 1];
    struct ngl_watch w;
    int rc;
    int rc2;
    char err[NGL_FORK_ERR];
    int rom_used;
    int stop_phase;
    uint32_t stop_state;
    uint32_t stop_reason;
    int cdrv;
    int cdrv_slot;
    uint32_t mmc_writes;
    uint32_t exit_seen;
    uint32_t exit_ms;
    uint32_t exit_quiet;
    uint32_t exit_gen;
    uint32_t exit_mmc;
    int sel_pending;
    int sel_index;
    char sel_path[NGL_FORK_SEL_PATH];
    uint32_t sel_ms;
    uint32_t sel_quiet;
    uint32_t sel_gen0;
    uint32_t sel_gen;
    uint32_t sel_mmc0;
    uint32_t sel_mmc;
    int sel_capped;
    int (*now)(struct ngl_civil *c);
    char tmsg[NGL_KTIME_MSG];
};

const char *ngl_confstr_skip(const char *entry);
uint32_t ngl_confstr_load_addr(const char *entry);
int ngl_core_ok(const char *name, const char *(*entry)(void *ctx, int i), void *ctx);
void ngl_fork_init(struct ngl_fork *f);
int ngl_fork_image(struct ngl_fork *f, const void *mon, uint32_t mon_len, const char *patch, size_t patch_len,
                   const char *path);
void ngl_fork_eject(struct ngl_fork *f);
int ngl_fork_step(struct ngl_fork *f, const struct ngl_io *io, int mgl_done, uint32_t dt_ms);
int ngl_fork_stop(struct ngl_fork *f, const struct ngl_io *io);
int ngl_fork_start(const struct ngl_io *io, struct ngl_fork *f, char *err, size_t errcap);
void ngl_fork_rom(struct ngl_fork *f);
int ngl_fork_reload(const struct ngl_fork *f);
int ngl_fork_select_req(struct ngl_fork *f, int index, const char *path);
void ngl_fork_select_drop(struct ngl_fork *f);

#endif
