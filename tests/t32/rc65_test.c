/*
 * T32 H.265 rate control, eprc T33 revision (#295) host test: OpenIMP's
 * port (src/eprc/eprc_t33_h265.c behind the i265e glue t32_rc65_* of
 * src/t32/openimp_t32_rc33.c, the IMP mapping t32_rc65_params) against vendor
 * 2.2.0 under tools/eprc_t33_h265_oracle.py:
 *
 *   rc65_test [-v] FILE ...
 *
 * 'A' + 'P' lines (oracle --params): a random attribute and the i265e
 * parameter block the vendor makes of it (channel_i265e_encoder_init +
 * I265e_configure); every word the rate control reads (t32_rc65_param_words)
 * must be equal.
 * 'S' + 'F' lines (oracle --scenarios): 'S k mode kbps width height gop fps
 * initqp pictures', the channel from IMP_Encoder_SetDefaultParam (OpenIMP:
 * t32_rc_default_attr), then per picture 'F picture type qp bytes' on the
 * oracle's synthetic size and statistics model (scene complexity seed k + 1):
 * the QP of every picture must be the vendor's; with a fifth 'F' field
 * (oracle --rows) the I pictures carry per-row sizes and their sum, which
 * the controller keeps (rc+0x224), must be equal too.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"

static int verbose;

static int unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;

        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return s[2 * n] == '\0' || s[2 * n] == '\n' ? 0 : -1;
}

static uint32_t w32(const uint8_t *p, uint32_t o)
{
    return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24;
}

static unsigned bad_off[T65_PSIZE / 4];

/* the attribute's skip part (IMPEncoderChnAttr +0x98, 28 bytes) */
static void hskip_of(const uint8_t *at, IMPEncoderAttrInitHSkip *hs)
{
    hs->hSkipAttr.skipType = (IMPSkipType)w32(at, 0x98);
    hs->hSkipAttr.m = (int)w32(at, 0x9c);
    hs->hSkipAttr.n = (int)w32(at, 0xa0);
    hs->hSkipAttr.maxSameSceneCnt = (int)w32(at, 0xa4);
    hs->hSkipAttr.bEnableScenecut = (int)w32(at, 0xa8);
    hs->hSkipAttr.bBlackEnhance = (int)w32(at, 0xac);
    hs->maxHSkipType = (IMPSkipType)w32(at, 0xb0);
}

/* one 'A' + 'P' pair */
static int check_param(const char *aline, char *pline, int *bad, int *skipped)
{
    unsigned seed, seed2, ret;
    int n = 0;
    uint8_t at[0xb8], want[T65_PSIZE];
    IMPEncoderAttrRcMode m;
    IMPEncoderAttrInitHSkip hs;
    T32Rc65 c;
    const uint16_t *words;
    unsigned nw, diff = 0, pw, ph;

    if (sscanf(aline, "A %u %n", &seed, &n) != 1 || !n || unhex(aline + n, at, sizeof(at)) != 0)
        return -1;
    n = 0;
    if (sscanf(pline, "P %u %x %n", &seed2, &ret, &n) != 2 || !n || seed2 != seed)
        return -1;
    if (ret != 0 || pline[n] == '-') {
        (*skipped)++;
        return 0;
    }
    if (unhex(pline + n, want, sizeof(want)) != 0)
        return -1;
    memcpy(&m, at + 0x3c, sizeof(m));
    hskip_of(at, &hs);
    pw = w32(at, 0xc) & 0xffffu;
    ph = w32(at, 0x10) & 0xffffu;
    if (t32_rc65_open(&c, pw, ph) != 0)
        return -1;
    if (t32_rc65_params(&c, &m, pw, ph, w32(at, 4), w32(at, 0x38), w32(at, 0x30), w32(at, 0x34), &hs) != 0) {
        t32_rc65_close(&c);
        (*bad)++;
        if (verbose)
            printf("seed %u mode %d: refused\n", seed, (int)m.rcMode);
        return 0;
    }
    nw = t32_rc65_param_words(&words);
    for (unsigned i = 0; i < nw; i++) {
        uint32_t o = words[i], v = w32(want, o), g = t32_rc65_get(&c, o);

        if (v != g) {
            if (!diff && verbose)
                printf("seed %u mode %d:", seed, (int)m.rcMode);
            if (verbose)
                printf(" %03x v %d o %d", o, (int)v, (int)g);
            bad_off[o / 4]++;
            diff++;
        }
    }
    if (diff) {
        (*bad)++;
        if (verbose)
            printf("\n");
    }
    t32_rc65_close(&c);
    return 0;
}

/* the oracle's model (tools/eprc_t33_h265_oracle.py model_bytes / model_stats) */
static uint32_t model_bytes(int qp, int idr, uint32_t ctus, double cplx)
{
    double bits = cplx * 4.0 * ctus * pow(2.0, (26.0 - qp) / 6.0) * (idr ? 5.0 : 1.0);

    return (uint32_t)(int64_t)(bits / 8.0) + 16u;
}

static void model_stats(int qp, uint32_t nb, uint32_t ctus, uint32_t st[T65_RC_STATS])
{
    memset(st, 0, T65_RC_STATS * sizeof(st[0]));
    st[0] = (uint32_t)qp * ctus * 16u;
    st[1] = ctus * 16u;
    st[2] = nb * 3u;
    st[3] = (nb / 2u) | (nb / 3u) << 16;
    st[4] = ctus * 4u;
    for (int j = 5; j <= 8; j++)
        st[j] = (ctus * 2u) | (ctus * 3u) << 16;
    st[29] = nb * 6u;
    st[30] = nb * 2u;
    st[32] = (ctus * 16u) << 11 | 3u << 30;
    st[33] = (uint32_t)qp * ctus * 4u;
}

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

struct scene {
    uint32_t x;
    double lvl;
    int still;
};

static double scene_next(struct scene *s)
{
    static const double levels[5] = { 5.0, 20.0, 60.0, 120.0, 250.0 };
    uint32_t u;

    s->x = u = xorshift32(s->x);
    if (u % 100 < 3)
        s->lvl = levels[(u >> 8) % 5];
    if ((u >> 16) % 100 < 2)
        s->still = 10 + (int)((u >> 20) % 50);
    if (s->still > 0) {
        s->still--;
        return 1.0 + ((u >> 4) & 255) / 256.0;
    }
    {
        double v = s->lvl * (0.8 + 0.4 * ((u >> 12) & 1023) / 1024.0);

        return v > 0.5 ? v : 0.5;
    }
}

/* one 'S' scenario and its 'F' lines; the pictures whose QP differs */
static int run(char *sline, FILE *f, int *pics, int *faults)
{
    int k, mode, kbps, w, h, gop, fps, iqp, n, bad = 0;
    IMPEncoderAttrRcMode m;
    T32Rc65 c;
    struct scene sc;
    char line[256];
    uint32_t ctus, st[T65_RC_STATS];

    if (sscanf(sline, "S %d %d %d %d %d %d %d %d %d", &k, &mode, &kbps, &w, &h, &gop, &fps, &iqp, &n) != 9)
        return -1;
    memset(&m, 0, sizeof(m));
    m.rcMode = (IMPEncoderRcMode)mode;
    t32_rc_default_attr(&m, PT_H265, iqp, (uint32_t)kbps);
    if (t32_rc65_open(&c, (uint32_t)w, (uint32_t)h) != 0)
        return -1;
    if (t32_rc65_params(&c, &m, (uint32_t)w, (uint32_t)h, 0, (uint32_t)gop, (uint32_t)fps, 1, NULL) != 0 ||
        t32_rc65_init(&c) != 0) {
        t32_rc65_close(&c);
        return -1;
    }
    gop = (int)t32_rc65_get(&c, T65P_GOP);
    ctus = ((t32_rc65_get(&c, T65P_WIDTH) + 63) / 64) * ((t32_rc65_get(&c, T65P_HEIGHT) + 63) / 64);
    sc.x = (uint32_t)(k + 1) ? (uint32_t)(k + 1) : 1u;
    sc.lvl = 60.0;
    sc.still = 0;
    for (int i = 0; i < n; i++) {
        double cplx = scene_next(&sc);
        int idr = gop ? i % gop == 0 : i == 0, ri, rt, rq, qp, nf;
        unsigned long rb, rs = 0;
        uint32_t nb, rows[T33_RC_ROWS];
        unsigned nrows = 0;

        if (!fgets(line, sizeof(line), f) || (nf = sscanf(line, "F %d %d %d %lu %lu", &ri, &rt, &rq, &rb, &rs)) < 4 ||
            ri != i) {
            printf("FAIL: reference ends at picture %d\n", i);
            bad += n - i;
            break;
        }
        (void)t32_rc65_start(&c, idr);
        qp = c.qp;
        nb = model_bytes(qp, idr, ctus, cplx);
        model_stats(qp, nb, ctus, st);
        if (nf == 5) {
            /* the per-row IDR sizes (oracle --rows model_rows) */
            nrows = t32_rc65_idr_rows(&c);
            for (unsigned r = 0; r < nrows; r++)
                rows[r] = (nb * 8u / nrows) * (6u + (r * 5u + (unsigned)qp) % 7u) / 9u;
        }
        t32_rc65_end_rows(&c, nb, st, rows, nrows);
        if (rq != qp || rt != (idr ? 2 : 1) || rb != nb || c.type != rt ||
            (nf == 5 && rs != t32_rc65_rc_word(&c, 0x224))) {
            if (bad < 3 && verbose)
                printf("  S %d picture %d: vendor type %d qp %d size %lu, OpenIMP type %d qp %d size %u\n", k, i, rt,
                       rq, rb, c.type, qp, nb);
            bad++;
        }
    }
    *pics += n;
    *faults += c.X.fault;
    t32_rc65_close(&c);
    return bad;
}

int main(int argc, char **argv)
{
    int bad = 0, skipped = 0, params = 0, runs = 0, runs_bad = 0, pics = 0, faults = 0;
    static char aline[1024], line[4096];

    for (int a = 1; a < argc; a++) {
        FILE *f;

        if (!strcmp(argv[a], "-v")) {
            verbose = 1;
            continue;
        }
        f = fopen(argv[a], "r");
        if (!f) {
            printf("FAIL: %s\n", argv[a]);
            return 1;
        }
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == 'A') {
                memcpy(aline, line, sizeof(aline) - 1);
                if (!fgets(line, sizeof(line), f) || check_param(aline, line, &bad, &skipped) != 0) {
                    printf("FAIL: %s: bad A/P pair\n", argv[a]);
                    fclose(f);
                    return 1;
                }
                params++;
            } else if (line[0] == 'S') {
                int r = run(line, f, &pics, &faults);

                if (r < 0) {
                    printf("FAIL: %s: %s", argv[a], line);
                    runs_bad++;
                } else if (r > 0) {
                    printf("FAIL %s: %s  %d pictures differ\n", argv[a], line, r);
                    runs_bad++;
                }
                runs++;
            }
        }
        fclose(f);
    }
    if (params) {
        printf("i265e parameters vs vendor 2.2.0: %d channels (%d not started), %d differ\n", params, skipped, bad);
        if (bad)
            for (unsigned o = 0; o < T65_PSIZE / 4; o++)
                if (bad_off[o])
                    printf("  +0x%03x: %u\n", o * 4, bad_off[o]);
    }
    if (runs)
        printf("eprc T33 H.265 vs vendor 2.2.0: %d runs, %d pictures, %d runs differ\n", runs, pics, runs_bad);
    if (faults) {
        printf("FAIL: %d controller accesses outside the arena\n", faults);
        runs_bad++;
    }
    return bad || runs_bad;
}
