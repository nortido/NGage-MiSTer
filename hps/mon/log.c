// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include "../common/platform.h"
#include "../common/status.h"
#include "../common/evlog.h"
#include "nor.h"

#ifndef MON_VERSION
#define MON_VERSION "b14-dev"
#endif

#define ST      ((volatile struct ngst *)VA_OF(PL_STATUS_BASE))
#define EVH     ((volatile struct evhdr *)VA_OF(PL_EVRING_BASE))
#define EVR     ((volatile struct evrec *)(VA_OF(PL_EVRING_BASE) + sizeof(struct evhdr)))
#define EV_N    65536u

#define PK(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum {
    PR_PREF, PR_RENT, PR_HALT, PR_PCNT, PR_PBAD, PR_FILL, PR_OPEN, PR_FA, PR_FB, PR_FWDN, PR_TOUT, PR_MILE,
    PR_TMIL, PR_IRQN, PR_TICK, PR_SDSC, PR_LCDW, PR_FLSH, PR_KICK, PR_IRQO, PR_RSTR, PR_PCSM, PR_FWDA, PR_FWDU,
    PR_G74S, PR_GFRM, PR_GFPS, PR_GFHI, PR_GH00, PR_MCFG = PR_GH00 + GF_NHIST / 4u, PR_SWOP, PR_SWFL, PR_SWSF,
    PR_MK0, PR_EXIT = PR_MK0 + PATCH_NMARK, PR_CDGN, PR_NORA, PR_NORP, PR_NORE, PR_NORX, PR_FIQC, PR_GPRC, PR_NROW
};

#define GH(a, b) PK('G', 'H', a, b)
#define MK(n) PK('M', 'K', '0', '0' + (n))

_Static_assert(PR_NROW <= NGST_NPROBE, "probe rows");

static const uint32_t pr_key[PR_NROW] = {
    PK('P', 'R', 'E', 'F'), PK('R', 'E', 'N', 'T'), PK('H', 'A', 'L', 'T'), PK('P', 'C', 'N', 'T'),
    PK('P', 'B', 'A', 'D'), PK('F', 'I', 'L', 'L'), PK('O', 'P', 'E', 'N'), PK('F', 'A', 'T', 'A'),
    PK('F', 'A', 'T', 'B'), PK('F', 'W', 'D', 'N'), PK('T', 'O', 'U', 'T'), PK('M', 'I', 'L', 'E'),
    PK('T', 'M', 'I', 'L'), PK('I', 'R', 'Q', 'N'), PK('T', 'I', 'C', 'K'), PK('S', 'D', 'S', 'C'),
    PK('L', 'C', 'D', 'W'), PK('F', 'L', 'S', 'H'), PK('K', 'I', 'C', 'K'), PK('I', 'R', 'Q', 'O'),
    PK('R', 'S', 'T', 'R'), PK('P', 'C', 'S', 'M'), PK('F', 'W', 'D', 'A'), PK('F', 'W', 'D', 'U'),
    PK('G', '7', '4', 'S'), PK('G', 'F', 'R', 'M'), PK('G', 'F', 'P', 'S'), PK('G', 'F', 'H', 'I'),
    GH('0', '0'), GH('0', '1'), GH('0', '2'), GH('0', '3'), GH('0', '4'), GH('0', '5'), GH('0', '6'), GH('0', '7'),
    GH('0', '8'), GH('0', '9'), GH('1', '0'), GH('1', '1'), GH('1', '2'), GH('1', '3'), GH('1', '4'), GH('1', '5'),
    PK('M', 'C', 'F', 'G'), PK('S', 'W', 'O', 'P'), PK('S', 'W', 'F', 'L'), PK('S', 'W', 'S', 'F'),
    MK(0), MK(1), MK(2), MK(3), MK(4), MK(5), MK(6), MK(7), PK('E', 'X', 'I', 'T'),
    PK('C', 'D', 'G', 'N'), PK('N', 'O', 'R', 'A'), PK('N', 'O', 'R', 'P'), PK('N', 'O', 'R', 'E'),
    PK('N', 'O', 'R', 'X'), PK('F', 'I', 'Q', 'C'), PK('G', 'P', 'R', 'C')
};

enum {
    XR_DRTT, XR_DRCS, XR_DRMM, XR_DRTA, XR_DRMV, XR_DRSP, XR_RWLK, XR_FSEC, XR_FLRG, XR_FSML, XR_MCYC, XR_FCYC,
    XR_CCYC, XR_ECYC, XR_WTIM, XR_CKI0, XR_CKI1, XR_CKDM, XR_DALL, XR_SWCI, XR_SWIF, XR_SWCO, XR_SWOF, XR_CKTL,
    XR_CKDA, XR_CKOT, XR_UCYC, XR_CYTL, XR_CYDA, XR_WGCY, XR_CQLC, XR_CCAN, XR_CLMX, XR_CL10, XR_CL50, XR_LDMX,
    XR_LDNC, XR_DCPR, XR_SLOG, XR_SLOV, XR_SCCY, XR_ENCY, XR_ENCN, XR_WKCY, XR_WKCN, XR_GFCY, XR_GFCN, XR_EMIO,
    XR_EMLS, XR_EMOT, XR_PMCY, XR_DMIN, XR_DMOT, XR_DMUK, XR_DMOC, XR_WTSZ, XR_WTOV,
    XR_DUAL, XR_CIV2, XR_L2OP, XR_BTMS, XR_PKMS, XR_FLMS, 
    XR_LMEV, XR_LMOD, XR_LGIP, XR_LSTL, XR_LSMX, XR_LSGT, XR_LARM, XR_LNWM, XR_LWAK,
    XR_LILM, XR_LFLR, XR_LZMX, XR_NROW = XR_LZMX + 1 + XC_NROW + 3 + 5 + XC_NDROW + 4 + 5 + XC_NLROW + 7 + XC_NCROW + 2 + 1 +
        XC_NTROW + XC_NEROW + 4
#if NGH_PMU_CENSUS
        + PMUC_NROW
#else
#if NGH_LIM_COST
        + 1
#endif
#endif
#if NGH_LIM_TALIGN
        + 3
#endif
#if NGH_GF_DIAG
        + 2
#endif
};

// the exit capture rows follow the limiter rows, in the xcap.h order
enum {
    XR_XTYP = XR_LZMX + 1, XR_XRSN, XR_XCT0, XR_XCT1, XR_XCT2, XR_XCT3, XR_XTHR, XR_XTIM, XR_XLVC, XR_XLVD, XR_XFRA,
    XR_XFRE
};

// the re-check rows follow the exit capture rows
enum { XR_LRCK = XR_XFRE + 1, XR_LRST, XR_LWDT };

// the wake class rows follow the re-check rows; halves are lim_pk16 counters that wrap
enum { XR_LWK1 = XR_LWDT + 1, XR_LWK2, XR_LWK3, XR_LHLD, XR_LFMT };

// the exit identity and exception rows follow the wake class rows; the live fork does not read them
enum { XR_XDLR = XR_LFMT + 1, XR_XEPC, XR_XELR, XR_XEFA, XR_XEFS };

// the FIQ deferral class rows and the guest CPSR row follow the exit identity rows
enum { XR_FDCL = XR_XEFS + 1, XR_FDMW, XR_GPSR, XR_FDLR };

// the write-xor-execute rows follow the FIQ deferral rows; the DACR transiton row after them
enum { XR_WXF = XR_FDLR + 1, XR_WXT, XR_WXP, XR_WXM, XR_WXD };

// the leave origin rows (callers, then threads, newest first) follow the DACR transition row
enum { XR_XLC0 = XR_WXD + 1, XR_XLC1, XR_XLC2, XR_XLT0, XR_XLT1, XR_XLT2 };

// the DACR class rows follow the leave origin rows, each a raw 32-bit counter but XWDM (two 16-bit masks)
enum { XR_WDC = XR_XLT2 + 1, XR_WMC, XR_WCM, XR_WNA, XR_WRS, XR_WLE, XR_WDM };

// the card-identity rows (the fork reads them by key) follow the DACR class rows
enum { XR_KCN = XR_WDM + 1, XR_KCT };

// the re-emit rows (raw 32-bit) follow the card-identity rows
enum { XR_REE = XR_KCT + 1, XR_REN };

// the boot write-back switch row: switches since the monitor start << 24 | the limiter tick at the last one
enum { XR_BWSW = XR_REN + 1 };

// the exit trace rows (XC_T_* order), then EX1b's close the table; the fork does not read them, the harness
// takes them by key
enum { XR_TRSP = XR_BWSW + 1 };

// EX1b: TRCH TRCF at the Die hit, then the DThread::Die entry capture's thread, chunk, its base and the read address
enum { XR_TRCH = XR_TRSP + XC_NTROW };

// the rumble rows close the table: RMBL writes << 16 | max << 8 | last pad byte, VBGW the guest vib word
enum { XR_RMBL = XR_TRCH + XC_NEROW, XR_VBGW };

// LGHT writes << 16 | last K, LIDL activity count << 16 | idle s (0xFFFF: the ROM verify failed)
enum { XR_LGHT = XR_VBGW + 1, XR_LIDL };

#if NGH_PMU_CENSUS
// FWm: guest instructions in the 16 s window, seconds since the guest boot, then events per 1000 instructions in 1/64
// (0xFFFF saturated), two per row in PMUC_EVS order, high half first
enum { XR_PMIN = XR_LIDL + 1, XR_PMSE, XR_PMR0, XR_PMR1, XR_PMR2 };
#else
#if NGH_LIM_COST
// the guest cost per ms of the last second; a census build has no room for it within NGST_NXPROBE
enum { XR_LGCC = XR_LIDL + 1 };
#endif
#endif

#if NGH_LIM_TALIGN
// the align rows follow LIDL, or the cost row of a cost build; an align build has no census
#if NGH_LIM_COST
enum { XR_LTAK = XR_LIDL + 2, XR_LTAT, XR_LTAW };
#else
enum { XR_LTAK = XR_LIDL + 1, XR_LTAT, XR_LTAW };
#endif
#endif

#if NGH_GF_DIAG
// the frame detector rows close the table; a census build and a cost build with the align rows have no room
#if NGH_PMU_CENSUS
#error "the frame detector rows need a build without the census"
#endif
#if NGH_LIM_TALIGN
#if NGH_LIM_COST
#error "the frame detector rows need a build without the cost row or without the align rows"
#endif
#endif
enum { XR_GFAN = XR_NROW - 2, XR_GFCC };
#endif

_Static_assert(XR_XTYP + XC_R_FRE == XR_XFRE, "exit capture rows");
_Static_assert(XR_LRCK == XR_XFRE + 1 && XR_LWDT + 1 == XR_LWK1, "re-check rows");
#if NGH_GF_DIAG
#if NGH_LIM_TALIGN
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 160, "wake class and frame detector rows");
#else
#if NGH_LIM_COST
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 158, "wake class and frame detector rows");
#else
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 157, "wake class and frame detector rows");
#endif
#endif
#else
#if NGH_LIM_TALIGN
#if NGH_LIM_COST
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 159, "wake class rows");
#else
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 158, "wake class rows");
#endif
#else
#if NGH_PMU_CENSUS
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 155 + PMUC_NROW, "wake class rows");
#else
#if NGH_LIM_COST
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 156, "wake class rows");
#else
_Static_assert(XR_LFMT + 1 == XR_XDLR && XR_NROW == 155, "wake class rows");
#endif
#endif
#endif
#endif
_Static_assert(XR_XDLR + XC_D_EFS == XR_XEFS && XR_XEFS + 1 == XR_FDCL, "exit identity and exception rows");
_Static_assert(XR_FDLR + 1 == XR_WXF, "fiq deferral rows");
_Static_assert(XR_WXM + 1 == XR_WXD && XR_WXD + 1 == XR_XLC0, "write-xor-execute rows");
_Static_assert(XR_XLC0 + XC_L_T2 == XR_XLT2 && XR_XLT2 + 1 == XR_WDC, "leave origin rows");
_Static_assert(XR_WDM + 1 == XR_KCN, "dacr class rows");
_Static_assert(XR_KCN + XC_C_CT == XR_KCT && XR_KCT + 1 == XR_REE, "card identity rows");
_Static_assert(XR_REN + 1 == XR_BWSW, "re-emit rows");
_Static_assert(XR_BWSW + 1 == XR_TRSP, "boot write-back row");
_Static_assert(XR_TRSP + XC_NTROW == XR_TRCH, "exit trace rows");
_Static_assert(XR_TRCH + XC_NEROW == XR_RMBL, "exit trace hit and entry rows");
_Static_assert(XR_VBGW + 1 == XR_LGHT, "rumble rows");
#if NGH_GF_DIAG
#if NGH_LIM_TALIGN
_Static_assert(XR_LTAW + 1 == XR_GFAN && XR_GFCC + 1 == XR_NROW, "align and frame detector rows");
#else
#if NGH_LIM_COST
_Static_assert(XR_LIDL + 2 == XR_GFAN && XR_GFCC + 1 == XR_NROW, "light, cost and frame detector rows");
#else
_Static_assert(XR_LIDL + 1 == XR_GFAN && XR_GFCC + 1 == XR_NROW, "light and frame detector rows");
#endif
#endif
#else
#if NGH_LIM_TALIGN
#if NGH_LIM_COST
_Static_assert(XR_LIDL + 2 == XR_LTAK && XR_LTAW + 1 == XR_NROW, "light, cost and align rows");
#else
_Static_assert(XR_LIDL + 1 == XR_LTAK && XR_LTAW + 1 == XR_NROW, "light and align rows");
#endif
#else
#if NGH_PMU_CENSUS
_Static_assert(XR_LIDL + 1 == XR_PMIN && XR_PMR2 + 1 == XR_NROW, "light and census rows");
#else
#if NGH_LIM_COST
_Static_assert(XR_LIDL + 1 == XR_LGCC && XR_LGCC + 1 == XR_NROW, "light and cost rows");
#else
_Static_assert(XR_LIDL + 1 == XR_NROW, "light rows");
#endif
#endif
#endif
#endif

_Static_assert(XR_NROW <= NGST_NXPROBE, "xprobe rows");

static const uint32_t xr_key[XR_NROW] = {
    PK('D', 'R', 'T', 'T'), PK('D', 'R', 'C', 'S'), PK('D', 'R', 'M', 'M'), PK('D', 'R', 'T', 'A'),
    PK('D', 'R', 'M', 'V'), PK('D', 'R', 'S', 'P'), PK('R', 'W', 'L', 'K'), PK('F', 'S', 'E', 'C'),
    PK('F', 'L', 'R', 'G'), PK('F', 'S', 'M', 'L'), PK('M', 'C', 'Y', 'C'), PK('F', 'C', 'Y', 'C'),
    PK('C', 'C', 'Y', 'C'), PK('E', 'C', 'Y', 'C'), PK('W', 'T', 'I', 'M'), PK('C', 'K', 'I', '0'),
    PK('C', 'K', 'I', '1'), PK('C', 'K', 'D', 'M'), PK('D', 'A', 'L', 'L'), PK('S', 'W', 'C', 'I'),
    PK('S', 'W', 'I', 'F'), PK('S', 'W', 'C', 'O'), PK('S', 'W', 'O', 'F'), PK('C', 'K', 'T', 'L'),
    PK('C', 'K', 'D', 'A'), PK('C', 'K', 'O', 'T'), PK('U', 'C', 'Y', 'C'),
    PK('C', 'Y', 'T', 'L'), PK('C', 'Y', 'D', 'A'), PK('W', 'G', 'C', 'Y'),
    PK('C', 'Q', 'L', 'C'), PK('C', 'C', 'A', 'N'), PK('C', 'L', 'M', 'X'), PK('C', 'L', '1', '0'),
    PK('C', 'L', '5', '0'), PK('L', 'D', 'M', 'X'), PK('L', 'D', 'N', 'C'), PK('D', 'C', 'P', 'R'),
    PK('S', 'L', 'O', 'G'), PK('S', 'L', 'O', 'V'), PK('S', 'C', 'C', 'Y'),
    PK('E', 'N', 'C', 'Y'), PK('E', 'N', 'C', 'N'), PK('W', 'K', 'C', 'Y'), PK('W', 'K', 'C', 'N'),
    PK('G', 'F', 'C', 'Y'), PK('G', 'F', 'C', 'N'), PK('E', 'M', 'I', 'O'), PK('E', 'M', 'L', 'S'),
    PK('E', 'M', 'O', 'T'), PK('P', 'M', 'C', 'Y'), PK('D', 'M', 'I', 'N'), PK('D', 'M', 'O', 'T'),
    PK('D', 'M', 'U', 'K'), PK('D', 'M', 'O', 'C'), PK('W', 'T', 'S', 'Z'), PK('W', 'T', 'O', 'V'),
    PK('D', 'U', 'A', 'L'), PK('C', 'I', 'V', '2'), PK('L', '2', 'O', 'P'), PK('B', 'T', 'M', 'S'),
    PK('P', 'K', 'M', 'S'), PK('F', 'L', 'M', 'S'),
    PK('L', 'M', 'E', 'V'), PK('L', 'M', 'O', 'D'), PK('L', 'G', 'I', 'P'), PK('L', 'S', 'T', 'L'),
    PK('L', 'S', 'M', 'X'), PK('L', 'S', 'G', 'T'), PK('L', 'A', 'R', 'M'), PK('L', 'N', 'W', 'M'),
    PK('L', 'W', 'A', 'K'), PK('L', 'I', 'L', 'M'), PK('L', 'F', 'L', 'R'), PK('L', 'Z', 'M', 'X'),
    XC_K_XTYP, XC_K_XRSN, XC_K_XCT0, XC_K_XCT1, XC_K_XCT2, XC_K_XCT3, XC_K_XTHR, XC_K_XTIM, XC_K_XLVC, XC_K_XLVD,
    XC_K_XFRA, XC_K_XFRE,
    PK('L', 'R', 'C', 'K'), PK('L', 'R', 'S', 'T'), PK('L', 'W', 'D', 'T'),
    PK('L', 'W', 'K', '1'), PK('L', 'W', 'K', '2'), PK('L', 'W', 'K', '3'), PK('L', 'H', 'L', 'D'),
    PK('L', 'F', 'M', 'T'),
    XC_K_XDLR, XC_K_XEPC, XC_K_XELR, XC_K_XEFA, XC_K_XEFS,
    PK('F', 'D', 'C', 'L'), PK('F', 'D', 'M', 'W'), PK('G', 'P', 'S', 'R'), PK('F', 'D', 'L', 'R'),
    PK('X', 'W', 'X', 'F'), PK('X', 'W', 'X', 'T'), PK('X', 'W', 'X', 'P'), PK('X', 'W', 'X', 'M'),
    PK('X', 'W', 'X', 'D'),
    XC_K_XLC0, XC_K_XLC1, XC_K_XLC2, XC_K_XLT0, XC_K_XLT1, XC_K_XLT2,
    PK('X', 'W', 'D', 'C'), PK('X', 'W', 'M', 'C'), PK('X', 'W', 'C', 'M'), PK('X', 'W', 'N', 'A'),
    PK('X', 'W', 'R', 'S'), PK('X', 'W', 'L', 'E'), PK('X', 'W', 'D', 'M'),
    XC_K_XKCN, XC_K_XKCT, PK('X', 'W', 'R', 'E'), PK('X', 'W', 'R', 'N'), PK('B', 'W', 'S', 'W'),
    PK('T', 'R', 'S', 'P'), PK('T', 'R', 'P', 'C'), PK('T', 'R', 'L', 'R'), PK('T', 'R', 'F', 'L'),
    PK('T', 'R', 'W', '0'), PK('T', 'R', 'W', '1'), PK('T', 'R', 'W', '2'), PK('T', 'R', 'W', '3'),
    PK('T', 'R', 'W', '4'), PK('T', 'R', 'W', '5'), PK('T', 'R', 'W', '6'), PK('T', 'R', 'W', '7'),
    PK('T', 'R', 'O', '0'), PK('T', 'R', 'O', '1'), PK('T', 'R', 'I', '0'), PK('T', 'R', 'I', '1'),
    PK('T', 'R', 'I', '2'), PK('T', 'R', 'I', '3'),
    PK('T', 'R', 'C', 'H'), PK('T', 'R', 'C', 'F'), PK('T', 'R', 'K', 'T'), PK('T', 'R', 'K', 'C'),
    PK('T', 'R', 'K', 'B'), PK('T', 'R', 'E', 'A'),
    PK('R', 'M', 'B', 'L'), PK('V', 'B', 'G', 'W'),
    PK('L', 'G', 'H', 'T'), PK('L', 'I', 'D', 'L'),
#if NGH_PMU_CENSUS
    PK('P', 'M', 'I', 'N'), PK('P', 'M', 'S', 'E'), PK('P', 'M', 'R', '0'), PK('P', 'M', 'R', '1'),
    PK('P', 'M', 'R', '2')
#else
#if NGH_LIM_COST
    PK('L', 'G', 'C', 'C')
#endif
#endif
#if NGH_LIM_TALIGN
// the cost key has no trailing comma
#if NGH_LIM_COST
    ,
#endif
    PK('L', 'T', 'A', 'K'), PK('L', 'T', 'A', 'T'), PK('L', 'T', 'A', 'W')
#endif
#if NGH_GF_DIAG
// the cost and align keys have no trailing comma
#if NGH_LIM_TALIGN
    ,
#else
#if NGH_LIM_COST
    ,
#endif
#endif
    PK('G', 'F', 'A', 'N'), PK('G', 'F', 'C', 'C')
#endif
};

static uint32_t g_evhead;
static uint32_t g_irqn, g_irqo, g_rstr, g_fwda, g_fwdu;
static uint32_t g_mk[PATCH_NMARK];
static struct irq_tick g_tk;
static struct dma g_dma;
static struct gf g_gf;
static struct nor g_nor;
static uint32_t g_nora;
static struct trap_sw g_sw;
static uint32_t g_drc[TDC_N], g_ck[TCK_N];
static uint32_t g_mcyc, g_fcyc, g_ccyc, g_ecyc, g_ucyc, g_cytl, g_cyda;
static uint32_t g_slog, g_slov, g_sccy;
static uint32_t g_ency, g_encn, g_gfcy, g_gfcn, g_em[TEM_N], g_pmcy;
static uint32_t g_minst, g_ient, g_lwcy;
#if NGH_PMU_EV
static struct pmuc g_pmc;
static uint32_t g_pce[PMUC_N];
#endif
#if NGH_LIM_COST
static struct lcost g_lc;
#endif
static struct lim g_lim;
static struct fiqd g_fqd;
static uint32_t g_wxe, g_wxw;
static uint32_t g_bwsw;

static void publish(void)
{
    dsb();
    wr32(DEV(PL_L2_BASE + PL_L2_CACHE_SYNC), 0);
    dsb();
}

static void prow(uint32_t i, uint32_t v)
{
    ST->probe[i].val = v;
}

static void xrow(uint32_t i, uint32_t v)
{
    ST->xprobe[i].val = v;
}

static uint64_t gtime(void)
{
    uint32_t hi, lo, hi2;

    hi = rd32(GTIMER + 4u);
    lo = rd32(GTIMER);
    hi2 = rd32(GTIMER + 4u);
    if (hi != hi2)
        lo = rd32(GTIMER);
    return (uint64_t)hi2 << 32 | lo;
}

static void st_init(void)
{
    const char *v = MON_VERSION;
    uint32_t i;

    ST->magic = 0;
    dsb();
    ST->layout = NGST_LAYOUT;
    for (i = 0; i < 16; i++)
        ST->version[i] = 0;
    for (i = 0; i < 15 && v[i]; i++)
        ST->version[i] = v[i];
    ST->state = NGST_STATE_BOOT;
    ST->park_reason = NGST_PARK_NONE;
    ST->heartbeat = 0;
    for (i = 0; i < NGST_NTRAP; i++)
        ST->traps[i] = 0;
    ST->forwards = 0;
    ST->fills = 0;
    ST->flushes = 0;
    ST->irqs = 0;
    ST->ticks = 0;
    ST->fatal = 0;
    ST->cdrv_gen = 0;
    ST->exit_seen = 0;
    ST->gproc = 0;
    for (i = 0; i < NGST_NPROBE; i++) {
        ST->probe[i].key = i < PR_NROW ? pr_key[i] : 0;
        ST->probe[i].val = 0;
    }
    for (i = 0; i < NGST_NXPROBE; i++) {
        ST->xprobe[i].key = i < XR_NROW ? xr_key[i] : 0;
        ST->xprobe[i].val = 0;
    }
    dsb();
    ST->magic = NGST_MAGIC;
    publish();
}

static void st_version(uint32_t mc)
{
    char v[16];
    uint32_t i;

    trap_ver(v, MON_VERSION, mc);
    for (i = 0; i < 16u; i++)
        ST->version[i] = v[i];
}

static void ev_init(void)
{
    uint32_t i;

    EVH->magic = 0;
    dsb();
    EVH->rec_size = sizeof(struct evrec);
    EVH->nrec = EV_N;
    EVH->head = 0;
    EVH->dropped = 0;
    for (i = 0; i < 3; i++)
        EVH->reserved[i] = 0;
    g_evhead = 0;
    dsb();
    EVH->magic = EV_MAGIC;
    publish();
}

static void ev_put(uint32_t kind, uint32_t flags, uint32_t pc, uint32_t a, uint32_t b, uint32_t c)
{
    volatile struct evrec *r;
    uint32_t h = g_evhead, c0 = pmu_cyc();

    if (ST->log_mode != NGST_LOG_RING && h >= EV_N) {
        EVH->dropped = EVH->dropped + 1u;
        publish();
        g_ecyc += pmu_cyc() - c0;
        return;
    }
    r = &EVR[h & (EV_N - 1u)];
    r->seq = h;
    r->kind = (uint16_t)kind;
    r->flags = (uint16_t)flags;
    r->pc = pc;
    r->a = a;
    r->b = b;
    r->c = c;
    r->t64 = gtime();
    dsb();
    g_evhead = h + 1u;
    EVH->head = g_evhead;
    publish();
    g_ecyc += pmu_cyc() - c0;
}

static void st_counters(void)
{
    ST->fills = g_fills;
    prow(PR_PREF, g_prefills);
    prow(PR_FILL, g_fills);
    prow(PR_OPEN, g_open);
    prow(PR_FWDN, g_fwdn);
    prow(PR_IRQN, g_irqn);
    prow(PR_TICK, g_tk.ticks);
    prow(PR_SDSC, g_dma.card_bytes / 512u);
    prow(PR_LCDW, g_dma.lcd_kicks);
    prow(PR_FLSH, g_dma.flushes);
    prow(PR_KICK, g_dma.kicks);
    prow(PR_IRQO, g_irqo);
    prow(PR_RSTR, g_rstr);
    prow(PR_FWDA, g_fwda);
    prow(PR_FWDU, g_fwdu);
    prow(PR_CDGN, ST->cdrv_gen);
    prow(PR_NORA, g_nora);
    prow(PR_NORP, g_nor.program);
    prow(PR_NORE, g_nor.erase);
    prow(PR_NORX, g_nor.stray);
    prow(PR_SWOP, g_sw.ops);
    prow(PR_SWFL, g_sw.full);
    prow(PR_SWSF, g_sw.safe);
    xrow(XR_DRTT, g_drc[TDC_TTBR]);
    xrow(XR_DRCS, g_drc[TDC_CSR]);
    xrow(XR_DRMM, g_sw.drop_mmu);
    xrow(XR_DRTA, g_sw.drop_all);
    xrow(XR_DRMV, g_sh.drop_mva);
    xrow(XR_DRSP, g_sh.drop_sec);
    xrow(XR_RWLK, g_sh.rewalks);
    xrow(XR_FSEC, g_sh.f_sec);
    xrow(XR_FLRG, g_sh.f_large);
    xrow(XR_FSML, g_sh.f_small);
    xrow(XR_MCYC, g_mcyc);
    xrow(XR_FCYC, g_fcyc);
    xrow(XR_CCYC, g_ccyc);
    xrow(XR_ECYC, g_ecyc);
    xrow(XR_WTIM, g_wtim);
    xrow(XR_CKI0, g_ck[TCK_I0]);
    xrow(XR_CKI1, g_ck[TCK_I1]);
    xrow(XR_CKDM, g_ck[TCK_DM]);
    xrow(XR_DALL, g_ck[TCK_DALL]);
    xrow(XR_SWCI, g_sw.ci);
    xrow(XR_SWIF, g_sw.ci_full);
    xrow(XR_SWCO, g_sw.co);
    xrow(XR_SWOF, g_sw.co_full);
    xrow(XR_CKTL, g_ck[TCK_TL]);
    xrow(XR_CKDA, g_ck[TCK_DA]);
    xrow(XR_CKOT, g_ck[TCK_OT]);
    xrow(XR_UCYC, g_ucyc);
    xrow(XR_CYTL, g_cytl);
    xrow(XR_CYDA, g_cyda);
    xrow(XR_WGCY, g_wgcy);
    xrow(XR_CQLC, g_dma.cqlc);
    xrow(XR_CCAN, g_dma.ccan);
    xrow(XR_CLMX, g_dma.clmx);
    xrow(XR_CL10, g_dma.cl10);
    xrow(XR_CL50, g_dma.cl50);
    xrow(XR_LDMX, g_dma.ldmx);
    xrow(XR_LDNC, g_dma.ldnc);
    xrow(XR_DCPR, g_dma.dcpr);
    xrow(XR_SLOG, g_slog);
    xrow(XR_SLOV, g_slov);
    xrow(XR_SCCY, g_sccy);
    xrow(XR_ENCY, g_ency);
    xrow(XR_ENCN, g_encn);
    xrow(XR_WKCY, g_wkcy);
    xrow(XR_WKCN, g_wkcn);
    xrow(XR_GFCY, g_gfcy);
    xrow(XR_GFCN, g_gfcn);
    xrow(XR_EMIO, g_em[TEM_IO]);
    xrow(XR_EMLS, g_em[TEM_LS]);
    xrow(XR_EMOT, g_em[TEM_OT]);
    xrow(XR_PMCY, g_pmcy);
    xrow(XR_DMIN, g_dma.dmin);
    xrow(XR_DMOT, g_dma.dmot);
    xrow(XR_DMUK, g_dma.dmuk);
    xrow(XR_DMOC, g_dma.dmoc);
    xrow(XR_WTSZ, g_sh.nguard);
    xrow(XR_WTOV, g_sh.wt_all);
    xrow(XR_DUAL, g_sh.dual);
    xrow(XR_CIV2, g_sw.civ2);
    xrow(XR_L2OP, g_dc.l2ops);
    xrow(XR_BTMS, g_btms);
    xrow(XR_PKMS, g_pkms);
    xrow(XR_FLMS, g_flms);
    ST->irqs = g_irqn;
    ST->ticks = g_tk.ticks;
    ST->flushes = g_dma.kicks + g_dma.flushes;
}

static uint32_t lim_min(uint32_t v, uint32_t m)
{
    return v < m ? v : m;
}

static uint32_t wx_pk(uint32_t hi, uint32_t lo)
{
    return lim_pk16(lim_min(hi, 0xFFFFu), lim_min(lo, 0xFFFFu));
}

static void lim_rows(const struct lim *l)
{
    xrow(XR_LMEV, l->mev);
    xrow(XR_LMOD, l->ipm << 2 | l->armed << 1 | l->turbo);
#if NGH_LIM_COST
    xrow(XR_LGIP, g_lc.gip);
#if !NGH_PMU_CENSUS
    xrow(XR_LGCC, l->gip);
#endif
#else
    xrow(XR_LGIP, l->gip);
#endif
    xrow(XR_LSTL, l->stl);
    xrow(XR_LSMX, l->smx);
    xrow(XR_LSGT, l->sgt);
    xrow(XR_LARM, l->cause << 28 | lim_min(l->kick, 4095u) << 16 | lim_min(l->tarm / 10u, 65535u));
    xrow(XR_LNWM, l->nwmax << 16 | l->nwarm);
    xrow(XR_LWAK, l->wak);
    xrow(XR_LILM, l->ilm);
    xrow(XR_LFLR, l->flr);
    xrow(XR_LZMX, l->zmx);
#if NGH_LIM_TALIGN
    xrow(XR_LTAK, lim_pk16(l->tans, l->tars));
    xrow(XR_LTAT, wx_pk(l->tat, l->tmr));
#if NGH_LIM_PRE_US
    xrow(XR_LTAW, l->tam << 24 | lim_min(l->tprs, 255u) << 16 | l->tahs);
#else
    xrow(XR_LTAW, lim_pk16(l->tam, l->tahs));
#endif
#endif
#if NGH_GF_DIAG
    xrow(XR_GFAN, g_gf.an);
    xrow(XR_GFCC, g_gf.cc);
#endif
    xrow(XR_LRCK, l->rck_n);
    xrow(XR_LRST, l->rst_n);
    xrow(XR_LWDT, l->rck << 31 | (l->wdt0 & 0xFFFFu));
    xrow(XR_LWK1, lim_pk16(l->wki, l->wkf));
    xrow(XR_LWK2, lim_pk16(l->wkm, l->wkn));
    xrow(XR_LWK3, lim_pk16(l->slm, l->flk));
    xrow(XR_LHLD, l->hld);
    xrow(XR_LFMT, lim_pk16(l->fmp, l->fms));
    xrow(XR_FDCL, lim_pk16(g_fqd.dg_und, g_fqd.dg_abt));
    xrow(XR_FDMW, lim_pk16(g_fqd.d_mon, g_fqd.carry + l->flk));
    xrow(XR_FDLR, g_fqd.last_glr);
    xrow(XR_WXF, wx_pk(g_sh.wx_exec - g_wxe, g_sh.wx_write - g_wxw));
    g_wxe = g_sh.wx_exec;
    g_wxw = g_sh.wx_write;
    xrow(XR_WXT, wx_pk(g_sh.wx_multi, g_sh.wx_exec + g_sh.wx_write));
    xrow(XR_WXP, g_sh.wx_pages);
    xrow(XR_WXM, wx_pk(g_sh.wx_mgr, g_sh.wx_widen));
    xrow(XR_WXD, g_sh.drop_dacr);
    xrow(XR_WDC, g_sh.dacr_writes);
    xrow(XR_WMC, g_sh.dacr_m2c);
    xrow(XR_WCM, g_sh.dacr_c2m);
    xrow(XR_WNA, g_sh.dacr_skip);
    xrow(XR_WRS, g_sh.dacr_rsv);
    xrow(XR_WLE, g_sh.dacr_live);
    xrow(XR_WDM, g_sh.dacr_mdom << 16 | g_sh.dacr_sdom);
    xrow(XR_REE, g_sh.reemits);
    xrow(XR_REN, g_sh.reemit_n);
    xrow(XR_BWSW, g_bwsw);
}

#if NGH_PMU_CENSUS
static void pmc_rows(void)
{
    uint32_t r[PMUC_NROW], i;

    pmuc_rows(&g_pmc, r);
    for (i = 0; i < PMUC_NROW; i++)
        xrow(XR_PMIN + i, r[i]);
}
#endif

static void irq_hw_park(void);
static void aud_hw_park(uint32_t reason);

static void __attribute__((noreturn)) park(uint32_t reason)
{
    __asm__ volatile("cpsid if" ::: "memory");
    irq_hw_park();
    st_counters();
    dcache_clean_all();
    // registers only (PL310 by PA), valid with the bridges gone
    g_pkms = l2_ci();
    xrow(XR_PKMS, g_pkms);
    reason = trap_l2_reason(reason, g_l2abt);
    ST->park_reason = reason;
    dsb();
    // park's one window access: after the existing writes up to the reason, before PARKED, which the fork waits for
    // before it takes the FPGA away
    aud_hw_park(reason);
    ST->state = NGST_STATE_PARKED;
    publish();
    for (;;)
        __asm__ volatile("wfe" ::: "memory");
}
