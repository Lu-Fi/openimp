/*
 * Ingenic "eprc" picture rate control, T33 revision, H.265 (#295, planned):
 * reimplemented from the vendor T32 libimp 2.2.0 (eprc_default_set_T33,
 * JZ_VPU_RC_*_T33 and their helpers at 0x19d550..0x1b1db0), which the vendor
 * uses for H.265 on the T32 (HERA).  The H.265 controller is a second build
 * of the H.264 one (src/eprc/eprc_t33_h264.c) with its own state layout and
 * HEVC rules; the state keeps the vendor layout in the arena of
 * eprc_t33_mem.h.  Work in progress: the helpers below are checked call by
 * call against the vendor controller (tools/t32/eprc33/diff265.py with the
 * swap build of swap265.py).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("wrapv")
#endif
#include "eprc/eprc_t33_mem.h"

#include <math.h>

/* ------------------------------------------------------------ constants */

/* I/P ratio model: slope / offset per 20 %-step of the I share (as H.264) */
static const int8_t t65_iratio_slope[5] = { 40, 25, 20, 15, 10 };
static const int8_t t65_iratio_ofs[5] = { 0, 3, 5, 8, 12 };

/* --------------------------------------------------------------- helpers */

static float t65_iratio_qp(int32_t share)
{
    float f;
    int k;

    if (share < 100) {
        k = share < 20 ? 0 : share < 40 ? 1 : share < 60 ? 2 : share < 80 ? 3 : 4;
        f = (float)((double)(t65_iratio_slope[k] * share) / 100.0 + (double)t65_iratio_ofs[k]);
    } else {
        f = 22.0f;
    }
    if (share > 100)
        f = share < 170 ? (float)((double)((share - 100) / 10) + 22.0) : 29.0f;
    return f;
}

/* QP difference of the I picture between the I shares @a and @b (percent),
 * corrected by the bit rate (@bits: int64 bit/s at that address)
 * (get_iratio_idqp, 0x1a0d70; the H.264 one at 0x18e93c is the same code) */
static int32_t e65_iratio_idqp(E33 *X, uint32_t bits, int32_t a, int32_t b)
{
    float fa = t65_iratio_qp(a), fb = t65_iratio_qp(b), diff, mb;
    int32_t m = (int32_t)(LD_S64(bits) / 1000000) - 30;
    double corr;

    if (m < 0)
        m = 0;
    mb = (float)m / 70.0f;
    corr = mb >= 0.0f ? (mb > 2.0f ? 2.0 : (double)mb) : 0.0;
    diff = fa - fb;
    if (diff <= 0.0f)
        return (int8_t)(int32_t)((double)diff + (corr - 0.5));
    return (int8_t)(int32_t)((double)diff + (0.5 - corr));
}

/* I-picture QP bounds from the size history of the last ten I pictures
 * (int64 bits at st+0x1d0, s8 QPs at st+0x22a): the QP of the picture
 * closest above @lo_pct percent of the budget (@bits * seconds at st+0x358)
 * is a lower bound, the QP of the one closest below @hi_pct an upper bound
 * (get_history_iqp, 0x19d550; the same code as the H.264 one at 0x18b9f0) */
static int32_t e65_history_iqp(E33 *X, uint32_t st, int32_t lo_pct, int32_t hi_pct, int32_t qp, int32_t bits)
{
    int32_t budget = (int32_t)((uint32_t)bits * (uint32_t)LD_S32(st + 0x358));
    int64_t lo = (int64_t)budget * lo_pct / 100, hi = (int64_t)budget * hi_pct / 100;
    int64_t best_lo = INT64_MAX, best_hi = INT64_MAX;
    int32_t q_lo = 1, q_hi = 51;

    for (int i = 0; i < 10; i++) {
        int64_t s = LD_S64(st + 0x1d0 + 8 * i);

        if (s == 0)
            continue;
        if (s > lo && s - lo < best_lo) {
            best_lo = s - lo;
            q_lo = LD_S8(st + 0x22a + i);
        }
        if (s < hi && hi - s < best_hi) {
            best_hi = hi - s;
            q_hi = LD_S8(st + 0x22a + i);
        }
    }
    if (qp >= q_hi)
        qp = (int8_t)(q_hi - 1);
    if (qp <= q_lo)
        qp = (int8_t)(q_lo + 1);
    return qp;
}

static inline int32_t clip_s(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* clamp the picture QP (st+0x90) to the window of its kind and to the QP
 * limits of @cfg (limit_frm_qp, 0x1a2fc0): as H.264, with the HEVC slice
 * type (P = 1).  @win: s8 window table (I max / I min at 0/1, P half-widths
 * at 0x18..0x1a), @frames: int (pictures). */
static void e65_limit_qp(E33 *X, uint32_t cfg, uint32_t frames, uint32_t st, uint32_t win)
{
    int8_t kind = LD_S8(st + 0x7b);
    int32_t qp = LD_S32(st + 0x90);

    if (kind == 2 || kind == 6) {
        qp = clip_s(qp, LD_S8(win + 1), LD_S8(win));
    } else if (kind == 1) {
        int32_t n = LD_S32(frames);
        int32_t hi = LD_S32(st + 0x4c);
        int apply = 1;

        if (n != 0 && (hi > 0 || (hi == 0 && (uint32_t)(n * 3) / 5u < LD_U32(st + 0x48))))
            apply = 0;
        if (apply) {
            int8_t src = LD_S8(st + 0x30), ref = LD_S8(st + 0x60);
            int8_t d = LD_S8(win + (src == 2 ? 0x18 : src == 6 ? 0x1a : 0x19));

            qp = clip_s(qp, (int8_t)(ref - d), (int8_t)(ref + d));
        }
    }
    ST_S32(st + 0x90, clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18)));
}

/* picture-level outputs of the controller (RC_output_param, 0x1a3118):
 * QP, slice type (2 I, 1 P; an intra-refresh picture 6 counts as P),
 * sizes, MB-QP range */
static void e65_output_param(E33 *X, uint32_t e, int32_t mb_qp)
{
    uint32_t pic = LD_U32(e + 0x3dc);
    uint32_t qp = LD_U32(pic + 8);
    int8_t kind = LD_S8(pic + 0x20);

    ST_U8(e + 0x380, qp);
    ST_U8(e + 0x38c, LD_U8(LD_U32(e + 0x3e8) + 9));
    ST_U32(e + 0x384, LD_U32(pic + 0x10));
    ST_U32(e + 0x388, LD_U32(pic + 0x18));
    if (kind == 6)
        kind = 1;
    ST_S8(e + 0x381, kind);
    if (LD_U32(pic + 8) != (qp & 0xff) && LD_S8(e + 0x53) != kind)
        return;                 /* the vendor prints and exits */
    if (mb_qp == 0) {
        ST_U8(e + 0x38d, 0);
        ST_U8(e + 0x38e, 0);
    } else {
        ST_U8(e + 0x38d, LD_U8(LD_U32(e + 0x42c) + 2));
        ST_U8(e + 0x38e, LD_U8(LD_U32(e + 0x42c) + 3));
    }
}

/* VBV sizes (vbv_init, 0x1a04ec): @cfg frame rate, @mode byte, @st */
static void e65_vbv_init(E33 *X, uint32_t cfg, uint32_t mode, uint32_t st)
{
    uint64_t rate;
    uint32_t den = LD_U32(cfg + 0x14);

    if (LD_S32(st + 0x14) == 0) {
        ST_U32(st + 0x428, 0);
        ST_U32(st + 0x42c, 0);
        ST_U32(st + 0x430, 0);
        /* buffer = 2 * rate * seconds (64-bit wrap as the vendor) */
        ST_U64(st + 0x420, (LD_U64(st + 0x2f0) * 2u) * (uint64_t)(int64_t)LD_S32(st + 0x2d8));
    }
    if (LD_U8(mode) == 1)
        rate = LD_U64(st + 0x2e0);
    else if ((uint8_t)(LD_U8(mode) - 2) <= 3)
        rate = LD_U64(st + 0x2f0);
    else
        return;
    ST_S64(st + 0x438, den ? (int64_t)(rate * LD_U32(cfg + 0x10)) / (int64_t)den : 0);
}
