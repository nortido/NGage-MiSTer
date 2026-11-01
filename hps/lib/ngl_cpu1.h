// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NGL_CPU1_H
#define NGL_CPU1_H

#include <stddef.h>
#include <stdint.h>

#include "../common/platform.h"
#include "../common/status.h"
#include "ngl_types.h"

enum {
    NGL_OK = 0,
    NGL_E_ARG = -1,
    NGL_E_LIVE = -2,
    NGL_E_AFFINITY = -3,
    NGL_E_SYSFS = -4,
    NGL_E_REG = -5,
    NGL_E_READBACK = -6,
    NGL_E_ONLINE = -7,
    NGL_E_NO_PARK_ACK = -8,
    NGL_E_MEM = -9,
    NGL_E_MON_READBACK = -10,
    NGL_E_PATCH_READBACK = -11,
    NGL_E_CORE = -12,
    NGL_E_BRIDGES = -13,
    NGL_E_MAGIC = -14,
    NGL_E_NOT_READY = -15,
    NGL_E_NO_CARD = -16,
    NGL_E_CPU1_LINUX = -17,
    NGL_E_FPGA = -18
};
#define NGL_NERR 19

#define NGL_CPU1_ONLINE "/sys/devices/system/cpu/cpu1/online"
#define NGL_CORENAME    "/tmp/CORENAME"
// the board scripts and the monitor (hps/mon/main.c BRGMODRST) treat any of bits 0-2 as bridges in reset
#define NGL_BRGMODRST   PL_RSTMGR_BRGMODRST
#define NGL_BRG_RESET   PL_BRGMODRST_RESET
#define NGL_ST(f)       (PL_STATUS_BASE + (uint32_t)offsetof(struct ngst, f))

const char *ngl_strerror(int e);
int ngl_mon_live(const struct ngl_io *io);
int ngl_cpu1_take(const struct ngl_io *io, uint32_t entry, int force);
int ngl_cpu1_return(const struct ngl_io *io);
int ngl_mon_park(const struct ngl_io *io);
int ngl_cpu1_recover(const struct ngl_io *io);

#endif
