// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <time.h>

#include "ngl_ktime.h"

// TTime counts microseconds from 0000-01-01 (Symbian epoch); 62168256000 s is 1970-01-01 on that scale
#define NGK_EPOCH_S    62168256000
#define NGK_SPAN_LO    0x324f3cu
#define NGK_SPAN_END   0x324fa4u
#define NGK_SPAN_BYTES (NGK_SPAN_END - NGK_SPAN_LO)
#define NGK_OFF        0x01FFFFFFu
#define NGK_B_TO       0x324f64u
#define NGK_LIT_TO     0x50324f40u

enum { NGK_YEAR_LO = 2020, NGK_YEAR_HI = 2099 };

static const struct {
    uint32_t off;
    uint32_t word;
} ngk_stock[] = {
    { 0x324f3cu, 0xe59d1020u },
    { 0x324f40u, 0xe3a0300au },
    { 0x324f44u, 0xe58d3000u },
    { 0x324f60u, 0xeb008174u },
    { 0x324f9cu, 0x50000084u },
    { 0x324fa0u, 0x40000424u },
};

static int64_t ngk_days(int64_t y, int64_t m, int64_t d)
{
    int64_t era, yoe, doy, doe;

    if (m <= 2)
        y--;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static uint32_t ngk_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t ngl_ttime(const struct ngl_civil *c)
{
    int64_t s = ngk_days(c->year, c->mon, c->day) * 86400 + (int64_t)c->hour * 3600 + (int64_t)c->min * 60 + c->sec;

    return (uint64_t)(s + NGK_EPOCH_S) * 1000000u;
}

int ngl_civil_ok(const struct ngl_civil *c)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap, dim;

    if (c->year < NGK_YEAR_LO || c->year > NGK_YEAR_HI || c->mon < 1 || c->mon > 12)
        return 0;
    leap = (c->year % 4 == 0 && c->year % 100 != 0) || c->year % 400 == 0;
    dim = mdays[c->mon - 1] + (c->mon == 2 && leap);
    if (c->day < 1 || c->day > dim || c->hour < 0 || c->hour > 23 || c->min < 0 || c->min > 59)
        return 0;
    // 60 is a leap second from localtime
    return c->sec >= 0 && c->sec <= 60;
}

int ngl_civil_local(int64_t t, struct ngl_civil *c)
{
    time_t tm_t = (time_t)t;
    const struct tm *lt = localtime(&tm_t);

    if (!lt)
        return -1;
    c->year = lt->tm_year + 1900;
    c->mon = lt->tm_mon + 1;
    c->day = lt->tm_mday;
    c->hour = lt->tm_hour;
    c->min = lt->tm_min;
    c->sec = lt->tm_sec;
    return 0;
}

size_t ngl_ktime_build(ngl_rd_fn rd, void *ctx, uint32_t rom_size, const uint32_t *img, size_t nw,
                       const struct ngl_civil *now, struct ngl_pair *out, char *msg, size_t cap, int tkind)
{
    static const char kept[] = "time: ROM default kept, ";
    uint8_t rom[NGK_SPAN_BYTES];
    uint64_t tt;
    size_t i;

    if (msg && cap)
        msg[0] = 0;
    if (!now) {
        if (msg && cap)
            snprintf(msg, cap, "%sno clock", kept);
        return 0;
    }
    if (!ngl_civil_ok(now)) {
        if (msg && cap)
            snprintf(msg, cap, "%sclock %04d-%02d-%02d %02d:%02d:%02d out of range", kept, now->year, now->mon,
                     now->day, now->hour, now->min, now->sec);
        return 0;
    }
    if (rom_size < NGK_SPAN_END) {
        if (msg && cap)
            snprintf(msg, cap, "%sROM size 0x%08x", kept, (unsigned)rom_size);
        return 0;
    }
    if (rd(ctx, NGL_ROM_PA + NGK_SPAN_LO, rom, NGK_SPAN_BYTES)) {
        if (msg && cap)
            snprintf(msg, cap, "%sROM read failed", kept);
        return 0;
    }
    for (i = 0; i < sizeof ngk_stock / sizeof ngk_stock[0]; i++) {
        uint32_t w = ngk_le(rom + (ngk_stock[i].off - NGK_SPAN_LO));

        if (w != ngk_stock[i].word) {
            if (msg && cap)
                snprintf(msg, cap, "%sROM word %08x at %08x", kept, (unsigned)w, (unsigned)ngk_stock[i].off);
            return 0;
        }
    }
    // the entries are two words each, the head word carries the offset
    for (i = 1; i < nw; i += 2) {
        uint32_t off = img[i] & NGK_OFF;

        if (off >= NGK_SPAN_LO && off < NGK_SPAN_END) {
            if (msg && cap)
                snprintf(msg, cap, "%slist has %08x", kept, (unsigned)off);
            return 0;
        }
    }
    tt = ngl_ttime(now);
    out[0].off = NGK_SPAN_LO;
    out[0].orig = ngk_stock[0].word;
    out[0].repl = ((NGK_B_TO - NGK_SPAN_LO - 8u) >> 2) | 0xEA000000u;
    out[1].off = ngk_stock[1].off | (tkind ? NGL_PAIR_TLO : 0u);
    out[1].orig = ngk_stock[1].word;
    out[1].repl = (uint32_t)tt;
    out[2].off = ngk_stock[2].off | (tkind ? NGL_PAIR_THI : 0u);
    out[2].orig = ngk_stock[2].word;
    out[2].repl = (uint32_t)(tt >> 32);
    out[3].off = ngk_stock[4].off;
    out[3].orig = ngk_stock[4].word;
    out[3].repl = NGK_LIT_TO;
    if (msg && cap)
        snprintf(msg, cap, "time: %04d-%02d-%02d %02d:%02d:%02d local, ttime %016llx, 4 rewrites%s", now->year,
                 now->mon, now->day, now->hour, now->min, now->sec, (unsigned long long)tt,
                 tkind ? ", reset-aware" : "");
    return NGL_KTIME_PAIRS;
}
