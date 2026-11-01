// SPDX-License-Identifier: GPL-3.0-or-later
#include "ngl_patch.h"

#define NGP_RW  0x80000000u
#define NGP_RWV 0x40000000u
#define NGP_MK  0x20000000u
#define NGP_MKX 0x10000000u
#define NGP_CDRV 0x08000000u
#define NGP_LRN 0x04000000u
#define NGP_XIF 0x02000000u
#define NGP_OFF 0x01FFFFFFu
#define NGP_PCM (NGP_LRN | NGP_XIF | NGP_CDRV)

static size_t ngp_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int ngp_num(const char *s, size_t len, int base, uint32_t *v)
{
    uint64_t acc = 0;
    size_t i;
    if (!len)
        return -1;
    for (i = 0; i < len; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return -1;
        acc = acc * (uint64_t)base + (uint64_t)d;
        if (acc > 0xFFFFFFFFu)
            return -1;
    }
    *v = (uint32_t)acc;
    return 0;
}

static int ngp_tok(const char *t, size_t n, size_t *pos, const char **tok, size_t *tlen)
{
    size_t i = *pos;
    while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r'))
        i++;
    *pos = i;
    if (i >= n || t[i] == '\n')
        return 0;
    *tok = t + i;
    while (i < n && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' && t[i] != '\n')
        i++;
    *tlen = (size_t)(t + i - *tok);
    *pos = i;
    return 1;
}

static int ngp_tokeq(const char *tok, size_t tlen, const char *s)
{
    size_t i;
    if (tlen != ngp_len(s))
        return 0;
    for (i = 0; i < tlen; i++)
        if (tok[i] != s[i])
            return 0;
    return 1;
}

static int ngp_rom_word(const uint8_t *rom, size_t rom_len, uint32_t off, uint32_t kind, uint32_t *v)
{
    uint32_t sel = kind ? off & 3u : 0;
    size_t a = off & ~1u, w = sel ? 2 : 4, i;
    uint32_t acc = 0;

    if (!rom || a > rom_len || rom_len - a < w)
        return NGL_P_ROM;
    for (i = w; i-- > 0;)
        acc = acc << 8 | rom[a + i];
    *v = acc;
    return 0;
}

int ngl_patch_parse(const char *t, size_t n, uint32_t *img, size_t capw, size_t *nw)
{
    return ngl_patch_parse_rom(t, n, NULL, 0, img, capw, nw);
}

int ngl_patch_parse_rom(const char *t, size_t n, const uint8_t *rom, size_t rom_len, uint32_t *img, size_t capw,
                        size_t *nw)
{
    size_t pos = 0;
    const char *tk[5];
    size_t tl[5];
    uint32_t count, got = 0, pairs = 0, off, orig, repl = 0, prev = 0, kind, flag, par[2], np, pcm = 0;
    uint32_t want = 0, sum = 0;
    int i, has_sum = 0;

    for (i = 0; i < 5; i++)
        if (!ngp_tok(t, n, &pos, &tk[i], &tl[i]))
            return NGL_P_HEADER;
    {
        const char *x;
        size_t xl;
        if (ngp_tok(t, n, &pos, &x, &xl)) {
            if (!ngp_tokeq(x, xl, "sum") || !ngp_tok(t, n, &pos, &x, &xl) || xl > 8 || ngp_num(x, xl, 16, &want) ||
                ngp_tok(t, n, &pos, &x, &xl))
                return NGL_P_HEADER;
            has_sum = 1;
        }
    }
    if (!ngp_tokeq(tk[0], tl[0], "#") || !ngp_tokeq(tk[1], tl[1], "md5") || !ngp_tokeq(tk[3], tl[3], "count") ||
        ngp_num(tk[4], tl[4], 10, &count))
        return NGL_P_HEADER;
    if (has_sum && !rom)
        return NGL_P_ROM;
    if (count > (capw - 1) / 2)
        return NGL_P_TOOMANY;
    while (pos < n) {
        const char *a, *b, *c, *x;
        size_t al, bl, cl, xl;
        int rw;
        if (t[pos] == '\n') {
            pos++;
            continue;
        }
        if (!ngp_tok(t, n, &pos, &a, &al))
            continue;
        if (!ngp_tok(t, n, &pos, &b, &bl) || al > 8 || bl > 8 || ngp_num(a, al, 16, &off) ||
            (has_sum ? !ngp_tokeq(b, bl, "-") : ngp_num(b, bl, 16, &orig)))
            return NGL_P_LINE;
        if (has_sum)
            orig = 0;
        // third column: a rewrite entry (flags in offset bits 31 and 30) or the word mark or exit, a monitor site (flags
        // 29 and 28). Site offset low bits: 00 ARM word, 10 Thumb halfword there, 01 Thumb halfword at offset-1, 11 bad
        // An optional fourth column `cdrv` flags a rewrite or a mark (bit 27). Other marks: `learn VA FIELD` (bits 29 and
        // 26, then two parameter pairs with bit 30) and `exitif` (bits 29 and 25), no cdrv. `pcm`: bits 29, 27, 26 and
        // 25, at most one, no further column
        kind = 0;
        flag = 0;
        rw = 0;
        np = 0;
        if (ngp_tok(t, n, &pos, &c, &cl)) {
            if (ngp_tokeq(c, cl, "mark"))
                kind = NGP_MK;
            else if (ngp_tokeq(c, cl, "exit"))
                kind = NGP_MKX;
            else if (ngp_tokeq(c, cl, "exitif"))
                kind = NGP_MK | NGP_XIF;
            else if (ngp_tokeq(c, cl, "learn"))
                kind = NGP_MK | NGP_LRN;
            else if (ngp_tokeq(c, cl, "pcm"))
                kind = NGP_MK | NGP_PCM;
            else if (cl > 8 || ngp_num(c, cl, 16, &repl))
                return NGL_P_LINE;
            else
                rw = 1;
            if ((kind & (NGP_LRN | NGP_XIF)) == NGP_LRN)
                for (; np < 2; np++)
                    if (!ngp_tok(t, n, &pos, &x, &xl) || xl > 8 || ngp_num(x, xl, 16, &par[np]))
                        return NGL_P_LINE;
            if (ngp_tok(t, n, &pos, &x, &xl)) {
                if ((kind & (NGP_MKX | NGP_LRN | NGP_XIF)) || !ngp_tokeq(x, xl, "cdrv") || ngp_tok(t, n, &pos, &x, &xl))
                    return NGL_P_LINE;
                flag = NGP_CDRV;
            }
        }
        if ((off & ~NGP_OFF) || (kind ? (off & 3u) == 3u || ((off & 3u) && orig > 0xFFFFu) : (off & 2u) != 0))
            return NGL_P_LINE;
        if (has_sum) {
            int rc = ngp_rom_word(rom, rom_len, off, kind, &orig);
            if (rc)
                return rc;
            sum += orig;
        }
        if (kind == (NGP_MK | NGP_PCM) && pcm++)
            return NGL_P_LINE;
        if (got >= count)
            return NGL_P_COUNT;
        if (got && off <= prev)
            return NGL_P_ORDER;
        if (1 + 2 * (size_t)(pairs + (rw ? 2u : 1u) + np) > capw)
            return NGL_P_TOOMANY;
        prev = off;
        off |= flag;
        if (rw) {
            img[1 + 2 * pairs] = off | NGP_RW;
            img[2 + 2 * pairs] = orig;
            pairs++;
            off |= NGP_RW | NGP_RWV;
            orig = repl;
        }
        img[1 + 2 * pairs] = off | kind;
        img[2 + 2 * pairs] = orig;
        pairs++;
        for (i = 0; i < (int)np; i++) {
            img[1 + 2 * pairs] = off | kind | NGP_RWV;
            img[2 + 2 * pairs] = par[i];
            pairs++;
        }
        got++;
    }
    if (got != count)
        return NGL_P_COUNT;
    if (has_sum && sum != want)
        return NGL_P_ROM;
    img[0] = pairs;
    *nw = 1 + 2 * (size_t)pairs;
    return 0;
}

long ngl_patch_expand(const char *t, size_t n, const uint8_t *rom, size_t rom_len, uint32_t *img, size_t capw, char *out,
                      size_t cap)
{
    size_t pos = 0, nw = 0, i;
    const char *tk;
    size_t tl;
    char md5[64];
    long ln;
    int rc;

    for (i = 0; i < 3; i++)
        if (!ngp_tok(t, n, &pos, &tk, &tl))
            return NGL_P_HEADER;
    if (tl > 63)
        return NGL_P_HEADER;
    for (i = 0; i < tl; i++)
        md5[i] = tk[i];
    md5[tl] = 0;
    rc = ngl_patch_parse_rom(t, n, rom, rom_len, img, capw, &nw);
    if (rc)
        return rc;
    ln = ngl_patch_text(img, nw, md5, out, cap);
    return ln < 0 ? NGL_P_TOOMANY : ln;
}

static int ngp_has_tt(const uint32_t *img, size_t nw)
{
    size_t j;

    for (j = 1; j < nw; j += 2)
        if ((img[j] & (NGP_RW | NGP_RWV)) == NGP_RW && (img[j] & (NGL_PAIR_TLO | NGL_PAIR_THI)))
            return 1;
    return 0;
}

int ngl_patch_append(uint32_t *img, size_t capw, size_t *nw, const struct ngl_pair *pairs, size_t n)
{
    size_t i, j, src, dst, k, grp = 0;
    uint32_t prev = 0;

    if (*nw == 0) {
        if (capw < 1)
            return NGL_P_TOOMANY;
        img[0] = 0;
        *nw = 1;
    }
    if ((*nw & 1u) == 0 || img[0] != (*nw - 1) / 2)
        return NGL_P_COUNT;
    if (*nw > capw)
        return NGL_P_TOOMANY;
    if (n > (capw - *nw) / 4)
        return NGL_P_TOOMANY;
    for (i = 0; i < n; i++) {
        uint32_t fl = pairs[i].off & ~NGP_OFF, off = pairs[i].off & NGP_OFF;

        if ((fl && fl != NGL_PAIR_TLO && fl != NGL_PAIR_THI) || (off & 2u))
            return NGL_P_LINE;
        if (fl == NGL_PAIR_TLO &&
            (grp++ || i + 1 >= n || pairs[i + 1].off != ((off + 4u) | NGL_PAIR_THI) || ngp_has_tt(img, *nw)))
            return NGL_P_LINE;
        if (fl == NGL_PAIR_THI && (!i || pairs[i - 1].off != ((off - 4u) | NGL_PAIR_TLO)))
            return NGL_P_LINE;
        if (i && off <= prev)
            return NGL_P_ORDER;
        // an entry inside the group would split it, and the monitor takes a split group as bad
        for (j = 1; j < *nw; j += 2) {
            uint32_t e = img[j] & NGP_OFF;

            if (e == off || (fl == NGL_PAIR_TLO && e > off && e < off + 8u))
                return NGL_P_ORDER;
        }
        prev = off;
    }
    // any entry above the new pairs moves up (bt and sb lists carry the BT blob above the Starter slots): the result
    // stays sorted by masked offset
    src = *nw;
    dst = *nw + 4 * n;
    for (k = n; k-- > 0;) {
        while (src > 1) {
            size_t len = 2;
            // a rewrite has one RWV pair after its head, a learn entry two
            while (len + 1 < src && (img[src - len] & NGP_RWV))
                len += 2;
            if ((img[src - 2] & NGP_OFF) < (pairs[k].off & NGP_OFF))
                break;
            for (j = 0; j < len; j++)
                img[dst - 1 - j] = img[src - 1 - j];
            src -= len;
            dst -= len;
        }
        dst -= 4;
        img[dst] = pairs[k].off | NGP_RW;
        img[dst + 1] = pairs[k].orig;
        img[dst + 2] = pairs[k].off | NGP_RW | NGP_RWV;
        img[dst + 3] = pairs[k].repl;
    }
    img[0] += (uint32_t)(2 * n);
    *nw += 4 * n;
    return 0;
}

static int ngp_is_pcm(uint32_t off)
{
    return (off & (NGP_RW | NGP_RWV | NGP_MK | NGP_MKX | NGP_PCM)) == (NGP_MK | NGP_PCM);
}

struct ngp_out {
    char *p;
    size_t n;
    size_t cap;
};

static void ngp_c(struct ngp_out *o, char c)
{
    if (o->n < o->cap)
        o->p[o->n] = c;
    o->n++;
}

static void ngp_s(struct ngp_out *o, const char *s)
{
    while (*s)
        ngp_c(o, *s++);
}

static void ngp_hex(struct ngp_out *o, uint32_t v)
{
    int i;
    for (i = 7; i >= 0; i--)
        ngp_c(o, "0123456789abcdef"[(v >> (4 * i)) & 15]);
}

static void ngp_dec(struct ngp_out *o, uint32_t v)
{
    char d[10];
    int k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        ngp_c(o, d[--k]);
}

long ngl_patch_text(const uint32_t *img, size_t nw, const char *md5, char *out, size_t cap)
{
    struct ngp_out o = { out, 0, cap };
    size_t i, lines = 0;

    if (nw == 0 || (nw & 1u) == 0 || img[0] != (nw - 1) / 2)
        return -1;
    for (i = 1; i < nw; i += 2) {
        uint32_t off = img[i];
        int pcm = ngp_is_pcm(off);
        if ((off & NGP_RWV) || ((off & NGP_MK) && (off & NGP_MKX)) || ((off & (NGP_MK | NGP_MKX)) && (off & NGP_RW)) ||
            ((off & NGP_CDRV) && !(off & (NGP_RW | NGP_MK))) ||
            (!pcm && (off & (NGP_LRN | NGP_XIF)) &&
             (!(off & NGP_MK) || (off & NGP_CDRV) || (off & (NGP_LRN | NGP_XIF)) == (NGP_LRN | NGP_XIF))))
            return -1;
        if (off & NGP_RW) {
            if (i + 2 >= nw || img[i + 2] != (off | NGP_RWV))
                return -1;
            i += 2;
        }
        if ((off & NGP_LRN) && !pcm) {
            if (i + 4 >= nw || img[i + 2] != (off | NGP_RWV) || img[i + 4] != (off | NGP_RWV))
                return -1;
            i += 4;
        }
        lines++;
    }
    ngp_s(&o, "# md5 ");
    ngp_s(&o, md5);
    ngp_s(&o, " count ");
    ngp_dec(&o, (uint32_t)lines);
    ngp_c(&o, '\n');
    for (i = 1; i < nw; i += 2) {
        uint32_t off = img[i];
        int pcm = ngp_is_pcm(off);
        ngp_hex(&o, off & NGP_OFF);
        ngp_c(&o, ' ');
        ngp_hex(&o, img[i + 1]);
        if (off & NGP_RW) {
            ngp_c(&o, ' ');
            ngp_hex(&o, img[i + 3]);
            i += 2;
        } else if (pcm) {
            ngp_s(&o, " pcm");
        } else if (off & NGP_LRN) {
            ngp_s(&o, " learn ");
            ngp_hex(&o, img[i + 3]);
            ngp_c(&o, ' ');
            ngp_hex(&o, img[i + 5]);
            i += 4;
        } else if (off & NGP_XIF) {
            ngp_s(&o, " exitif");
        } else if (off & NGP_MK) {
            ngp_s(&o, " mark");
        } else if (off & NGP_MKX) {
            ngp_s(&o, " exit");
        }
        if ((off & NGP_CDRV) && !pcm)
            ngp_s(&o, " cdrv");
        ngp_c(&o, '\n');
    }
    if (o.n >= cap)
        return -1;
    out[o.n] = 0;
    return (long)o.n;
}

int ngl_patch_cdrv_slot(const uint32_t *img, size_t nw)
{
    size_t i;
    int slot = 0;

    // the monitor numbers mark and exit entries in list order
    for (i = 1; i + 1 < nw; i += 2) {
        if (!(img[i] & (NGP_MK | NGP_MKX)) || (img[i] & NGP_RWV))
            continue;
        if ((img[i] & (NGP_MK | NGP_CDRV)) == (NGP_MK | NGP_CDRV) && !ngp_is_pcm(img[i]))
            return slot;
        slot++;
    }
    return -1;
}
