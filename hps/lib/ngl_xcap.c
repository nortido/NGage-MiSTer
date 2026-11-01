// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "ngl_cpu1.h"
#include "ngl_xcap.h"

static const uint32_t xcap_key[XC_NROW] = {
    XC_K_XTYP, XC_K_XRSN, XC_K_XCT0, XC_K_XCT1, XC_K_XCT2, XC_K_XCT3, XC_K_XTHR, XC_K_XTIM, XC_K_XLVC, XC_K_XLVD,
    XC_K_XFRA, XC_K_XFRE
};

static const uint32_t cid_key[XC_NCROW] = { XC_K_XKCN, XC_K_XKCT };

int ngl_xcap_read(const struct ngl_io *io, struct ngl_xcap *out)
{
    int n = 0;

    memset(out, 0, sizeof(*out));
    out->gproc = io->rd32(io->ctx, NGL_ST(gproc));
    // by key, not by index: a monitor of another build may place the rows elsewhere or have none
    for (uint32_t i = 0; i < NGST_NXPROBE; i++) {
        uint32_t pa = NGL_ST(xprobe) + 8u * i, key = io->rd32(io->ctx, pa);

        for (uint32_t r = 0; r < XC_NROW; r++) {
            if (key != xcap_key[r] || (out->have & 1u << r)) continue;
            out->row[r] = io->rd32(io->ctx, pa + 4u);
            out->have |= 1u << r;
            n++;
            break;
        }
        for (uint32_t r = 0; r < XC_NCROW; r++) {
            if (key != cid_key[r] || (out->have & 1u << (XC_NROW + r))) continue;
            out->cid[r] = io->rd32(io->ctx, pa + 4u);
            out->have |= 1u << (XC_NROW + r);
            n++;
            break;
        }
        if (key == XC_K_LARM && !(out->have & 1u << (XC_NROW + XC_NCROW))) {
            out->larm = io->rd32(io->ctx, pa + 4u);
            out->have |= 1u << (XC_NROW + XC_NCROW);
            n++;
        }
    }
    return n;
}

static void xcap_code(char *b, size_t n, uint32_t raw)
{
    raw &= 0xFFFFu;
    if (raw == 0x8000u) snprintf(b, n, " <=-32768");
    else if (raw == 0x7FFFu) snprintf(b, n, " >=32767");
    else snprintf(b, n, " %d", (int)(int16_t)(uint16_t)raw);
}

static int xcap_cid_rows(const struct ngl_xcap *x)
{
    uint32_t m = 1u << XC_NROW | 1u << (XC_NROW + 1u);

    return (x->have & m) == m;
}

static void xcap_cid_line(char *b, const struct ngl_xcap *in)
{
    uint32_t g = in->cid[XC_C_CN] >> 16, te = in->row[XC_R_TIM] & XC_TIM_EXIT, tc = in->cid[XC_C_CT], d;
    const char *v = ngl_xcap_cid(in) ? "card-identity" : "none";
    int k;

    if (!xcap_cid_rows(in)) {
        snprintf(b, NGL_XCAP_LINE, "exit cid: no rows, %s", v);
        return;
    }
    k = snprintf(b, NGL_XCAP_LINE, "exit cid: reads %u (all %u), last ", (unsigned)g,
        (unsigned)(in->cid[XC_C_CN] & 0xFFFFu));
    if (k <= 0 || k >= NGL_XCAP_LINE) return;
    if (!g) {
        snprintf(b + k, NGL_XCAP_LINE - (size_t)k, "-, %s", v);
        return;
    }
    d = tc <= te ? te - tc : tc - te;
    snprintf(b + k, NGL_XCAP_LINE - (size_t)k, "%u.%u s %s the exit, %s", (unsigned)(d / 10u), (unsigned)(d % 10u),
        tc <= te ? "before" : "after", v);
}

static void xcap_frame_line(char *b, const struct ngl_xcap *in)
{
    uint32_t a = in->larm, c = a >> XC_LARM_CAUSE_SHIFT & XC_LARM_CAUSE_MASK, t = a & XC_LARM_T_MASK;
    uint32_t te = in->row[XC_R_TIM] & XC_TIM_EXIT;
    const char *v = ngl_xcap_preframe(in) ? "pre-frame" : "none";
    char at[48];

    if (!(in->have & 1u << (XC_NROW + XC_NCROW))) {
        snprintf(b, NGL_XCAP_LINE, "exit frame: no row, %s", v);
        return;
    }
    if (c == 0u)
        snprintf(at, sizeof(at), "none");
    else if (c == XC_LARM_FRAME)
        snprintf(at, sizeof(at), "frame %u.%02u s (kick %u)", (unsigned)(t / 100u), (unsigned)(t % 100u),
            (unsigned)(a >> XC_LARM_KICK_SHIFT & XC_LARM_KICK_MASK));
    else if (c == XC_LARM_TIMER)
        snprintf(at, sizeof(at), "timer %u.%02u s", (unsigned)(t / 100u), (unsigned)(t % 100u));
    else
        snprintf(at, sizeof(at), "cause %u %u.%02u s", (unsigned)c, (unsigned)(t / 100u), (unsigned)(t % 100u));
    snprintf(b, NGL_XCAP_LINE, "exit frame: %s, exit %u.%u s, %s", at, (unsigned)(te / 10u), (unsigned)(te % 10u), v);
}

static void xcap_kb(char *b, size_t n, const struct ngl_xcap *in, int r, int hi)
{
    uint32_t v = (in->row[r] >> (hi ? 16 : 0)) & 0xFFFFu;

    if (!(in->have & 1u << r) || v == XC_KB_NONE) snprintf(b, n, "-");
    else snprintf(b, n, "%u", (unsigned)v);
}

int ngl_xcap_fmt(const struct ngl_xcap *in, char (*line)[NGL_XCAP_LINE], int n)
{
    uint32_t typ = in->row[XC_R_TYP], tim = in->row[XC_R_TIM], lv, cnt;
    char cat[XC_CAT_MAX + 1], code[XC_LV_NCODE][12], kb[4][8];
    int k = 0;

    if (n <= 0) return 0;
    if (!(in->have & 1u << XC_R_TYP)) {
        snprintf(line[0], NGL_XCAP_LINE, "exit: cause - (no capture rows)");
        return 1;
    }
    memset(cat, 0, sizeof(cat));
    for (uint32_t i = 0; i < XC_CAT_MAX; i++) {
        char c = (char)(in->row[XC_R_CT0 + i / 4u] >> (24u - 8u * (i % 4u)));

        if (!c) break;
        cat[i] = c;
    }
    k = snprintf(line[0], NGL_XCAP_LINE,
        "exit: cause type %u reason %d cat \"%s\" thr 0x%08x by %s gproc 0x%08x t %u.%u s flags 0x%x",
        (unsigned)(typ & XC_TYP_TYPE), (int)in->row[XC_R_RSN], cat, (unsigned)in->row[XC_R_THR],
        (typ & XC_F_FOREIGN) ? "other" : "self", (unsigned)in->gproc, (unsigned)((tim & XC_TIM_EXIT) / 10u),
        (unsigned)((tim & XC_TIM_EXIT) % 10u), (unsigned)(typ & XC_F_ALL));
    if (typ >> XC_TYP_FOL_SHIFT & XC_TYP_FOL_MAX && k > 0 && k < NGL_XCAP_LINE)
        snprintf(line[0] + k, NGL_XCAP_LINE - (size_t)k, " follow %u",
            (unsigned)(typ >> XC_TYP_FOL_SHIFT & XC_TYP_FOL_MAX));
    if (n < 2) return 1;

    cnt = (in->have & 1u << XC_R_LVD) ? in->row[XC_R_LVD] & 0xFFFFu : 0;
    if (!cnt) {
        snprintf(line[1], NGL_XCAP_LINE, "exit: leaves 0");
    } else {
        xcap_code(code[0], sizeof(code[0]), in->row[XC_R_LVC] >> 16);
        xcap_code(code[1], sizeof(code[1]), in->row[XC_R_LVC]);
        xcap_code(code[2], sizeof(code[2]), in->row[XC_R_LVD] >> 16);
        lv = (in->have & 1u << XC_R_TIM) ? tim >> XC_TIM_LV_SHIFT : XC_TIM_LV_NONE;
        k = snprintf(line[1], NGL_XCAP_LINE, "exit: leaves %u last%s%s%s", (unsigned)cnt, code[0], cnt > 1 ? code[1] : "",
            cnt > 2 ? code[2] : "");
        if (k > 0 && k < NGL_XCAP_LINE) {
            if (lv == XC_TIM_LV_NONE) snprintf(line[1] + k, NGL_XCAP_LINE - (size_t)k, " none timed");
            else snprintf(line[1] + k, NGL_XCAP_LINE - (size_t)k, " newest %u.%u s before the exit", (unsigned)(lv / 10u),
                (unsigned)(lv % 10u));
        }
    }
    if (n < 3) return 2;

    if (!(in->have & (1u << XC_R_FRA | 1u << XC_R_FRE))) {
        snprintf(line[2], NGL_XCAP_LINE, "exit: free -");
    } else {
        xcap_kb(kb[0], sizeof(kb[0]), in, XC_R_FRA, 1);
        xcap_kb(kb[1], sizeof(kb[1]), in, XC_R_FRA, 0);
        xcap_kb(kb[2], sizeof(kb[2]), in, XC_R_FRE, 1);
        xcap_kb(kb[3], sizeof(kb[3]), in, XC_R_FRE, 0);
        snprintf(line[2], NGL_XCAP_LINE, "exit: free KB app %s leave %s exit %s min %s", kb[0], kb[1], kb[2], kb[3]);
    }
    if (n < 4) return 3;

    xcap_cid_line(line[3], in);
    if (n < 5) return 4;

    xcap_frame_line(line[4], in);
    return 5;
}

int ngl_xcap_cid(const struct ngl_xcap *x)
{
    uint32_t typ = x->row[XC_R_TYP], te = x->row[XC_R_TIM] & XC_TIM_EXIT, tc = x->cid[XC_C_CT];

    // the fixed hybrid CID fails every original card's check, so the game process ending itself (Kill,
    // Terminate or Panic) right after its own read is that check; no title data enters the rule
    if (!(x->have & 1u << XC_R_TYP) || !(x->have & 1u << XC_R_TIM) || !xcap_cid_rows(x)) return 0;
    if ((typ & XC_TYP_TYPE) > 2u || (typ & XC_F_FOREIGN)) return 0;
    if (!(x->cid[XC_C_CN] >> 16)) return 0;
    return tc <= te && te - tc <= NGL_XCAP_CID_WIN;
}

int ngl_xcap_preframe(const struct ngl_xcap *x)
{
    uint32_t typ = x->row[XC_R_TYP], te = x->row[XC_R_TIM] & XC_TIM_EXIT, a = x->larm;
    uint32_t c = a >> XC_LARM_CAUSE_SHIFT & XC_LARM_CAUSE_MASK, t = a & XC_LARM_T_MASK;

    // a game that ends itself before its first content frame never ran (the card check of a protected title);
    // LARM's 10 ms field and XTIM's 100 ms field share g_lim.ticks, so an arm inside the exit's step is before it
    if (!(x->have & 1u << XC_R_TYP) || !(x->have & 1u << XC_R_TIM) || !(x->have & 1u << (XC_NROW + XC_NCROW))) return 0;
    if ((typ & XC_TYP_TYPE) > 1u || (typ & XC_F_FOREIGN)) return 0;
    // xc_boot publishes zero rows with their keys; no game exits within 100 ms of its boot
    if (!te) return 0;
    if (c == 0u) return 1;
    if (c != XC_LARM_FRAME && c != XC_LARM_TIMER) return 0;
    return t >= 10u * te + 10u;
}
