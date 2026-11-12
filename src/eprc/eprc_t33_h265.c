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

static inline uint32_t clip_u(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
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
/* P-picture dQP thresholds, three per row (adjust_P_frame_dqp; the H.264 values) */
static const float t65_pdqp_row[5][3] = {
    { 0.2f, 0.4f, 0.65f }, { 0.3f, 0.5f, 0.75f }, { 0.3f, 0.5f, 0.75f },
    { 0.3f, 0.5f, 0.75f }, { 0.3f, 0.6f, 0.8f },
};

/* dQP of a P picture from its predicted size against the target
 * (adjust_P_frame_dqp, 0x19de88; the same code as the H.264 one at 0x18c328): @cfg mode at +0x4d, @st direction at
 * +0x94 and target bits (int64) at +0x48, @bits predicted bits, @row the
 * threshold row */
static int32_t e65_adjust_p_dqp(E33 *X, uint32_t cfg, uint32_t st, uint32_t u3, uint32_t u4, uint32_t bits_lo,
                         int32_t bits_hi, int8_t row)
{
    int8_t mode = LD_S8(cfg + 0x4d), dir = LD_S8(st + 0x94);
    int64_t v = (int64_t)(((uint64_t)(uint32_t)bits_hi << 32) | bits_lo);
    int64_t t = LD_S64(st + 0x48);
    int32_t r;

    (void)u3;
    (void)u4;
    if (mode == 1) {
        if (dir == 1)
            v = (v * 4) / 5;
        else if (dir == -1)
            v = (v * 6) / 5;
    }
    if (v <= 0)
        v = 1;
    if (v * 18 < t) {
        if (mode == 1)
            return 15;
        r = 15;
    } else if ((v * 6) / 5 < t) {
        r = (int32_t)((double)((float)t / (float)v) - 0.7) + 1;
    } else {
        float ft = (float)t, fv = (float)v;
        const float *th = t65_pdqp_row[row];

        r = -3;
        if (fv * th[0] <= ft) {
            r = -2;
            if (fv * th[1] <= ft)
                r = ft < fv * th[2] ? -1 : 0;
        }
    }
    if (mode == 2) {
        int8_t c = (int8_t)r;

        if (dir == 1)
            return r >= 0 ? c : r < -2 ? (int8_t)(c + 1) : 0;
        if (dir != -1)
            return c;
        if (r > 0) {
            if (r < 3)
                return 0;
            r--;
        }
    }
    return (int8_t)r;
}

/* CBR target scale (percent of the bit rate) by rate-quality level, and the
 * matching lower bound (the H.264 values) */
static const uint8_t t65_cbr_max_pct[9] = { 100, 110, 120, 130, 140, 150, 160, 170, 180 };
static const uint8_t t65_cbr_min_pct[9] = { 100, 90, 80, 80, 80, 80, 80, 80, 80 };

enum { T65_FIXQP, T65_CBR, T65_VBR, T65_SMART, T65_CVBR, T65_AVBR };

/* (re)derive the rate budgets of the mode (init_stat_param, 0x1a29a4; the same code as
 * the H.264 one at 0x18ef74):
 * @cfg controller parameters (+4 mode, +0x10 kbit/s), @inst instance
 * block, @st statistics block, @r rate block (+8 target, +0x10 maximum) */
static void e65_init_stat(E33 *X, uint32_t cfg, uint32_t inst, uint32_t st, uint32_t r)
{
    uint8_t mode = LD_U8(cfg + 4);
    uint32_t kbps;
    int64_t d;

    if (mode >= T65_CBR && mode <= T65_AVBR) {
        kbps = LD_U32(cfg + 0x10);
        ST_U32(cfg + 0x10, kbps < 2 ? 2 : kbps > 0x19000 ? 0x19000 : kbps);
    }
    kbps = LD_U32(cfg + 0x10);
    switch (mode) {
    case T65_CBR:
        d = LD_S64(r + 8);
        ST_U64(r + 8, (uint32_t)(kbps << 10));
        if (d != (int64_t)(uint32_t)(kbps << 10))
            ST_U64(st + 0x428, 0);
        break;
    case T65_VBR: {
        uint32_t bits = kbps << 10;

        ST_U64(r + 0x10, bits);
        ST_S64(r + 8, (int64_t)((uint64_t)kbps * 1024u * LD_U8(cfg + 0x20)) / 100);
        if (LD_S32(inst + 0x48) < 1) {
            int32_t q = 20;

            if (bits < 2000001u)
                q = bits < 1000001u ? (bits < 500001u ? 5 : 10) : 15;
            ST_S32(inst + 0x48, q);
        } else if (LD_S32(inst + 0x48) > 100) {
            ST_S32(inst + 0x48, 100);
        }
        break;
    }
    case T65_SMART:
    case T65_AVBR: {
        uint8_t pct = LD_U8(cfg + 0x20);
        int64_t t;

        ST_U64(r + 0x10, (uint32_t)(kbps << 10));
        ST_S64(r + 8, (int64_t)((uint64_t)kbps * 1024u * pct) / 100);
        t = (int64_t)((uint64_t)pct * (uint32_t)(kbps << 10)) / 100;
        ST_S64(inst + 0x68, (int64_t)((uint64_t)t * LD_U8(cfg + 0x26)) / 100);
        ST_U8(inst + 0x7d, LD_U8(cfg + 0x2a));
        ST_U8(inst + 0x7e, LD_U8(cfg + 0x2b));
        if (mode == T65_SMART) {
            t = (int64_t)(LD_U64(r + 0x10) * pct) / 100;
            ST_S64(inst + 0x70, (int64_t)((uint64_t)t * LD_U8(cfg + 0x2f)) / 100);
        }
        break;
    }
    case T65_CVBR:
        ST_U64(r + 0x10, (uint32_t)(kbps << 10));
        break;
    default:
        break;
    }
    switch (mode) {
    case T65_CBR: {
        uint64_t t = LD_U64(r + 8);
        uint8_t lvl = LD_U8(cfg + 0x14);

        ST_U64(st + 0x2e0, t);
        ST_S64(st + 0x2f0, (int64_t)(t * t65_cbr_max_pct[lvl]) / 100);
        ST_S64(st + 0x2f8, (int64_t)(t * t65_cbr_min_pct[lvl]) / 100);
        break;
    }
    case T65_VBR:
        ST_U64(st + 0x2e0, LD_U64(r + 8));
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, 0);
        break;
    case T65_SMART:
    case T65_AVBR:
        ST_U64(st + 0x2e0, LD_U64(r + 8));
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, LD_U64(inst + 0x68));
        break;
    case T65_CVBR:
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, LD_U64(st + 0x278));
        ST_U64(st + 0x2e0, LD_U64(st + 0x270));
        break;
    }
    if (mode >= T65_CBR && mode <= T65_AVBR)
        e65_vbv_init(X, LD_U32(cfg), cfg + 4, st);
    {
        int64_t sec = LD_S32(st + 0x2d8);

        ST_U64(st + 0x320, LD_U64(st + 0x2f0));
        ST_U64(st + 0x330, 0);
        ST_U64(st + 0x328, LD_U64(st + 0x2e0) * (uint64_t)sec);
        ST_U64(st + 0x318, LD_U64(st + 0x2f0) * (uint64_t)sec);
    }
}

/* ----------------------------------------------------- scene classes */

/* scene block (controller block + 0x3e8 ->): +0 -> class output byte,
 * +4 -> class history (32 bytes, newest first), +8 class in force,
 * +9 class of this picture, +0x1c activity, +0x28 motion level */
#define SC_OUT   0x0
#define SC_HIST  0x4
#define SC_CUR   0x8
#define SC_NEW   0x9

static void t65_hist_push(E33 *X, uint32_t sc, uint8_t v)
{
    uint32_t h = LD_U32(sc + SC_HIST);

    for (int i = 31; i > 0; i--)
        ST_U8(h + i, LD_U8(h + i - 1));
    ST_U8(h, v);
}

/* class of the picture from the engine's block-type statistics
 * (get_scene_frame_method1, 0x19faa8): @cfgp -> picture parameters (u16
 * width, height), @stat statistics block (u16 counts at +0x3c..+0x4a),
 * @pic -> picture info.  As H.264 with the counts of 16x16 blocks (four
 * per 32x32) and the level at sc+0x28. */
static void e65_scene_class(E33 *X, uint32_t cfgp, uint32_t stat, uint32_t pic, uint32_t sc)
{
    uint32_t cfg = LD_U32(cfgp), n, a, b, s, t, h0;
    uint32_t c8 = LD_U16(stat + 0x44), c9 = (LD_U16(stat + 0x46) + LD_U16(stat + 0x48)) & 0xffff;
    uint32_t still = LD_U16(stat + 0x4a);
    int k, m = -1;
    uint8_t lvl;

    n = ((((int32_t)LD_U16(cfg) + 0x1f) >> 5) * (((int32_t)LD_U16(cfg + 2) + 0x1f) >> 5) * 4) & 0xffff;
    if (n == 0)
        n = 1;
    a = (still * 100 / n) & 0xffff;
    ST_U8(sc + 0x1c, (((still + (c9 * 7 + c8 * 3) / 10) & 0xffff) * 100) / n);
    b = ((((uint32_t)LD_U16(stat + 0x3c) + LD_U16(stat + 0x3e) + LD_U16(stat + 0x40) + LD_U16(stat + 0x42)) & 0xffff)
         * 100 / n) & 0xffff;
    h0 = LD_U8(LD_U32(sc + SC_HIST));
    k = 4;
    if (a > 80) {
        k = 6;
    } else if (a + (s = (c9 * 100 / n) & 0xffff) >= 81) {
        k = a < 61 ? 4 : 5;
    } else {
        t = (c8 * 100 / n) & 0xffff;
        if (s + t < 81) {
            if (b + t > 90)
                m = b > 50 ? 0 : 2;     /* static (0) or still (2) candidate */
        } else if (s < 61) {
            m = 2;
        }
    }
    if (m < 0)
        m = h0 != 0 ? k : 1;
    else if (h0 >= 4 && m == 0)
        m = 3;
    t65_hist_push(X, sc, (uint8_t)m);
    ST_U8(sc + SC_NEW, m);
    lvl = 3;
    if (LD_S32(LD_U32(pic)) != 0)
        lvl = b >= 95 ? 0 : b >= 75 ? 1 : 2;
    ST_U8(sc + 0x28, lvl);
}

/* class map when the last ten pictures agree (as H.264) */
static const uint8_t t65_class_same[8] = { 0, 1, 2, 4, 3, 5, 6, 7 };

static void t65_class_set(E33 *X, uint32_t sc, uint8_t v)
{
    ST_U8(sc + SC_CUR, v);
    ST_U8(LD_U32(sc + SC_OUT), v);
}

/* scene class in force from the class history (IFA_get_frm_feature,
 * 0x19fd64; the H.264 code at 0x18e3b0 with the H.265 offsets) */
static void e65_frame_feature(E33 *X, uint32_t e)
{
    uint32_t fi = LD_U32(e + 0x3ec), sc = LD_U32(e + 0x3e8), h;
    uint8_t c, v[10];
    int rev = LD_S8(e + 0x34), i;

    if (LD_S8(LD_U32(fi + 8)) == 1)
        return;
    if (LD_S8(LD_U32(fi + 4) + 0x20) == 2) {
        t65_hist_push(X, sc, LD_U8(sc + SC_NEW));
        goto tail;
    }
    if (rev == 6 || rev == 7)
        e65_scene_class(X, e + 0x3e4, LD_U32(fi), fi + 4, sc);
    h = LD_U32(sc + SC_HIST);
    c = LD_U8(sc + SC_NEW);
    for (i = 1; i <= 9; i++)
        v[i] = LD_U8(h + i);
    for (i = 1; i <= 9 && v[i] == c; i++)
        ;
    if (i > 9) {
        if (c > 7)
            ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
        else if (c == 3 || c == 4)
            t65_class_set(X, sc, t65_class_same[c]);
        else {
            ST_U8(sc + SC_CUR, t65_class_same[c]);
            ST_U8(LD_U32(sc + SC_OUT), c);
        }
        goto tail;
    }
#define ANY(x) (v[1] == (x) || v[2] == (x) || v[3] == (x) || v[4] == (x) || v[5] == (x) || v[6] == (x) \
                || v[7] == (x) || v[8] == (x) || v[9] == (x))
    if (ANY(6)) {
        t65_class_set(X, sc, 6);
    } else if (ANY(5)) {
        t65_class_set(X, sc, 5);
    } else if (ANY(1)) {
        if (c != 0)
            ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
        else
            t65_class_set(X, sc, 1);
    } else if (ANY(3) || ANY(4)) {
        if (c == 0)
            t65_class_set(X, sc, 4);
        else
            ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
    } else {
        /* only 0 (static) and 2 (still) in the window */
        int two = ANY(2), zero_late = v[3] == 0 || v[4] == 0 || v[5] == 0 || v[6] == 0 || v[7] == 0 || v[8] == 0
                                      || v[9] == 0;
        int mixed;              /* 0x19e7e8: no static picture in the window */

        if (v[1] == 2)
            mixed = v[2] != 0;
        else if (v[2] != 2)
            mixed = !(v[1] == 0 || v[2] == 0);
        else
            mixed = v[1] != 0;
        if (v[1] != 2 && v[2] == 2 && v[1] == 0) {
            t65_class_set(X, sc, c != 0 ? 1 : 4);
        } else if (mixed && !zero_late) {
            if (c != 0)
                ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
            else
                t65_class_set(X, sc, two ? 4 : 0);
        } else if (c != 0) {
            t65_class_set(X, sc, 1);
        } else {
            t65_class_set(X, sc, two ? 4 : 0);
        }
    }
#undef ANY
tail:
    if (rev == 7) {
        uint32_t src = LD_U32(fi) + 0x98;

        for (i = 0; i < 32; i++)
            ST_U32(sc + 0x4c + 4 * i, LD_U32(src + 4 * i));
    }
}


/* ------------------------------------------------- reference QPs */

/* fractional steps of a size ratio in 1 dQP units (update_qp; the H.264 values) */
static const float t65_frac_step[5] = { 0.122f, 0.26f, 0.414f, 0.587f, 0.782f };

/* dQP for a size ratio @b / @a in steps of 6 per doubling (update_qp,
 * 0x1a4304): negative when @b is larger.  As the H.264 one (0x1960cc),
 * without its zero check: a size of 0 counts as 1. */
static int32_t e65_ratio_dqp(E33 *X, int32_t a, int32_t b)
{
    float q, fr;
    int32_t n, k;

    (void)X;
    if (a < 1)
        a = 1;
    if (a == 1)
        return 0;
    if (b < 1)
        b = 1;
    if (a < b) {
        q = (float)b / (float)a;
        n = (int32_t)q;
        if ((n - 1) * 6 > 51)
            return 51;
        fr = q - (float)n;
        for (k = 0; k < 5 && t65_frac_step[k] <= fr; k++)
            ;
        if (k == 5 && 1.0f <= fr)
            return 0;
        return (int8_t)(-((int8_t)((n - 1) * 6) + k));
    }
    if (b < a) {
        q = (float)a / (float)b;
        n = (int32_t)q;
        if ((n - 1) * -6 + 37 <= 0)
            return 0;
        fr = q - (float)n;
        for (k = 0; k < 5 && t65_frac_step[k] <= fr; k++)
            ;
        if (k == 5 && 1.0f <= fr)
            return 0;
        return (int8_t)((int8_t)((n - 1) * 6) + k);
    }
    return 0;
}

/* reference QP from the picture size and frame-rate ratio
 * (get_refer_qp_mode1, 0x1a4518; the H.264 code at 0x196398) */
static int32_t e65_refer_qp_size(E33 *X, uint32_t cfg, int32_t pixels, int32_t gop_pos, uint32_t unused,
                                 int32_t bits)
{
    int32_t a = e65_ratio_dqp(X, 1024000, bits), b = e65_ratio_dqp(X, 921600, pixels);
    int32_t num = LD_S32(cfg + 0x10), k;
    uint32_t den = LD_U32(cfg + 0x14);
    int32_t qp;

    (void)unused;
    k = den > (uint32_t)(num * 30) ? -2 : den > (uint32_t)(num * 25) ? -1 : den > (uint32_t)(num * 20) ? 0
      : den > (uint32_t)(num * 15) ? 1 : den > (uint32_t)(num * 10) ? 2 : 3;
    qp = (int8_t)a + (int8_t)b + k + (int8_t)(gop_pos < 3 ? gop_pos : 2) + 37;
    return (int8_t)clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18));
}

/* bits-per-pixel classes of the reference QP model, QP = slope * bpp + ofs
 * per class (the H.264 values) */
static const float t65_bpp_class[7] = { 0.32f, 0.122f, 0.0469f, 0.0213f, 0.0102f, 0.0049f, 0.0024f };
static const int16_t t65_bpp_slope[7] = { -10, -25, -66, -195, -450, -943, -1599 };
static const int16_t t65_bpp_ofs[7] = { 25, 30, 35, 41, 46, 51, 54 };

/* reference QP of an I picture from its bits per pixel (get_refer_qp_mode0,
 * 0x1a02f0): @cfg picture size, @pixels, @bits (int64 by value, lo / hi) */
static int32_t e65_refer_qp_bpp(E33 *X, uint32_t cfg, int32_t pixels, uint32_t bits_lo, uint32_t bits_hi)
{
    float fps_num = (float)(double)LD_U32(cfg + 0x10), fps_den = (float)(double)LD_U32(cfg + 0x14);
    float r, f, bpp;
    int32_t qp;
    int k;

    if (fps_num == 0.0f)
        return 0;
    r = fps_den / fps_num;
    f = 15.0f;
    if (f < r)
        f = (r - f) / 3.0f + f;
    f = (float)pixels * f;
    if (f == 0.0f)
        return 0;
    bpp = (float)(int64_t)((uint64_t)bits_hi << 32 | bits_lo) / f;
    for (k = 0; k < 7 && bpp <= t65_bpp_class[k]; k++)
        ;
    if (k == 7)
        qp = 51;
    else
        qp = (int32_t)((float)t65_bpp_slope[k] * bpp + (float)t65_bpp_ofs[k]);
    return (int8_t)clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18));
}

static int64_t t65_max64(int64_t a, int64_t b)
{
    return a < b ? b : a;
}

static int64_t t65_min64(int64_t a, int64_t b)
{
    return b < a ? b : a;
}

static int64_t t65_div64(int64_t a, int64_t b)
{
    return b ? a / b : 0;
}

/* I-QP correction by motion class (inst+0x7d 1/2), QP class and the
 * inst+0x7e level (0x1e9bfc: index qcls * 8 + rule * 4 + level); levels
 * 4..7 of the last row read on into the next vendor table (the bpp slopes
 * -10, -25 as bytes), kept for levels up to 7 */
static const int8_t t65_iadj[48] = {
    30, 40, 50, 60, 30, 45, 60, 75, 10, 20, 20, 20, 30, 40, 55, 70, 15, 25, 35, 45, 30, 40, 50, 65,
    20, 30, 40, 50, 35, 45, 55, 65, 30, 40, 50, 60, 10, 20, 20, 20, 30, 40, 50, 60, -10, -1, -25, -1,
};

static void t65_clip_u8(E33 *X, uint32_t a, uint8_t lo, uint8_t hi)
{
    uint8_t v = LD_U8(a);

    if (v < lo)
        ST_U8(a, lo);
    else if (hi < v)
        ST_U8(a, hi);
}

static int64_t t65_clip64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* the I-picture bit share of mode 3/5 (SMART/AVBR, init_inst_param
 * 0x1967d8): from the last ten I/P ratios (st+0x248..), the bit rate and
 * the motion rule; returns the 32-bit I target */
static int32_t t65_smart_ibits(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out, int32_t n)
{
    int64_t b = LD_S64(st + 0x2f0), c = LD_S64(st + 0x2f8), i, q, d;
    int32_t v0 = LD_S8(st + 0x248), sum = 0, mx = 0, avg, j, rule;
    float f = (float)(LD_U8(rc + 0x20) + 100) / 200.0f, f1, f2;

    for (j = 0; j < n; j++) {
        int32_t v = LD_S8(st + 0x248 + j);

        sum += v;
        if (mx < v)
            mx = v;
    }
    avg = sum / n;
    avg = avg < 5 ? 5 : avg > 10 ? 10 : avg;
    if (v0 < avg) {
        i = (int64_t)(((f * (float)b - (float)c) * 0.5f * (float)v0) / (float)avg + (float)c);
    } else {
        f1 = v0 + mx - 2 * avg < 2 ? 1.0f : (float)(v0 + mx - 2 * avg);
        f2 = mx - avg < 2 ? 1.0f : (float)(mx - avg);
        i = (int64_t)(((f * (float)b - (float)c) * 0.5f * f1) / f2 + (float)c);
    }
    /* high bit rates: at least C + 100/80/60/40 % of (B - C) */
    q = LD_S64(st + 0xa0) / 10000;
    d = b - c;
    if (q > 8000)
        i = t65_max64(i, (int64_t)(float)d + c);
    if (q > 6000)
        i = t65_max64(i, (int64_t)((float)d * 0.8f) + c);
    if (q > 4000)
        i = t65_max64(i, (int64_t)((float)d * 0.6f) + c);
    if (q > 2000)
        i = t65_max64(i, (int64_t)((float)d * 0.4f) + c);
    rule = LD_S8(inst + 0x7d);
    if (rule == 1 || rule == 2) {
        int32_t h = (LD_S8(st + 0xdc) + LD_S32(st + 0x58)) >> 1, cls, cnt, t, m;
        uint32_t r = LD_U32(LD_S32(out + 0x1c) + 0x88) / 0xfa000;
        int64_t s = 0, a;

        if (r > 100)
            r = 100;
        /* QP class; above 51 the vendor falls back to class 3 */
        cls = h < 11 ? 0 : h < 21 ? 1 : h < 33 ? 2 : h < 46 ? 3 : h > 51 ? 3 : 4;
        cnt = 10;
        for (j = 0; j < 10; j++) {
            int32_t v = LD_S8(st + 0x23e + j);

            if (v == -1)
                cnt = (int8_t)(cnt - 1);
            else
                s += v;
        }
        a = s / (cnt == 0 ? 1 : cnt);
        t = LD_S8(inst + 0x7e) + cls * 8 + rule * 4;
        t = t >= 0 && t < 48 ? t65_iadj[t] : 0;     /* levels above 7: not modelled */
        m = (int32_t)((uint32_t)t * r) / 100 + t / 2;
        a = rule == 1 ? a * m / -100 : (100 - a) * m / 100;
        i = (a + 100) * (int64_t)(int32_t)i / 100;
    }
    return (int32_t)i;
}

/* per-GOP instance targets (init_inst_param, 0x1a58ac; the H.264 code at 0x1964d8
 * with the H.265 offsets): clamp the QP
 * limits and bit rates, copy the mode QP range to @out, then set the I
 * (st+0x360), P (+0x370) and low (+0x378) picture targets of the mode */
static void e65_init_inst(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
{
    static const uint8_t qfield[6] = { 0, 0x22, 0x26, 0x28, 0x2a, 0x28 };
    uint32_t cfg = LD_U32(rc);
    uint8_t mode, mx, f;
    int64_t i, p, l, b, c;

    if (LD_U8(cfg + 0x18) > 51)
        ST_U8(cfg + 0x18, 51);
    if (LD_U8(cfg + 0x18) < LD_U8(cfg + 0x19))
        ST_U8(cfg + 0x19, LD_U8(cfg + 0x18));
    if (LD_U32(rc + 0x3c) > 0xc350000)
        ST_U32(rc + 0x3c, 0xc350000);
    if (LD_U32(rc + 0x40) > 0xc350000)
        ST_U32(rc + 0x40, 0xc350000);
    mode = LD_U8(rc + 4);
    if (mode == 0 || mode > 5)
        goto tail;
    /* the mode's max/min QP pair, max inside the picture limits, min up to max */
    f = qfield[mode];
    mx = LD_U8(cfg + 0x18);
    t65_clip_u8(X, rc + f + 1, LD_U8(cfg + 0x19), mx);
    t65_clip_u8(X, rc + f, LD_U8(rc + f + 1), mx);
    ST_U8(out + 1, LD_U8(rc + f + 1));
    ST_U8(out, LD_U8(rc + f));

    b = LD_S64(st + 0x2f0);
    c = LD_S64(st + 0x2f8);
    switch (mode) {
    case 1:
        i = LD_S64(st + 0x2e0);
        p = b;
        l = c;
        if (LD_S32(st + 0x14) != 0 && LD_S32(st + 0x344) != 0) {
            int32_t n = LD_S32(st + 0x344) * LD_S32(st + 0x358);
            int64_t off;

            if (n == 0)
                return;     /* the vendor asserts */
            off = LD_S64(st + 0x420) * (int64_t)LD_S32(st + 0x430) / -100;
            off = (off + LD_S64(st + 0x428)) / n;
            i -= off;
            p -= off;
            l -= off;
        }
        /* P and low target inside [C, B] of the unshifted targets, I inside [low, P] */
        p = t65_clip64(p, c, b);
        l = t65_clip64(l, c, b);
        i = i < l ? l : i > p ? p : i;
        ST_S64(st + 0x360, i);
        ST_S64(st + 0x370, p);
        ST_S64(st + 0x378, l);
        break;
    case 2:
        ST_S64(st + 0x360, LD_S64(st + 0x2e0));
        ST_S64(st + 0x370, b);
        ST_S64(st + 0x378, c);
        break;
    case 3:
    case 5: {
        int32_t n = 0, j;

        ST_S64(st + 0x370, b);
        ST_S64(st + 0x378, c);
        if (LD_S32(st + 0x14) == 0) {
            ST_S64(st + 0x360, LD_S64(st + 0x2e0));
            break;
        }
        for (j = 0; j < 10; j++)
            n += LD_S8(st + 0x248 + j) != -1;
        if (n == 0) {
            i = LD_S64(st + 0x2e0);
            ST_S64(st + 0x360, i);
        } else {
            i = t65_smart_ibits(X, rc, inst, st, out, n);
            ST_S64(st + 0x360, i);
            ST_U8(st + 0x98, 0);
            if (mode == 3 && LD_S8(inst + 0x7c) != 0 && LD_S8(st + 0x248) < LD_S8(inst + 0x81)
                && LD_U8(LD_U32(out + 0x20) + 8) == 0) {
                /* still scene: the fixed I target, no low target */
                i = LD_S64(inst + 0x70);
                c = 0;
                ST_S64(st + 0x378, 0);
                ST_U8(st + 0x98, 1);
                ST_S64(st + 0x360, i);
            }
        }
        i = i < c ? c : i > LD_S64(st + 0x370) ? LD_S64(st + 0x370) : i;
        ST_S64(st + 0x360, i);
        break;
    }
    case 4:
        ST_S64(st + 0x360, LD_S64(st + 0x2e0));
        ST_S64(st + 0x370, b);
        ST_S64(st + 0x378, c);
        if (LD_S32(st + 0x14) == 0) {
            if (LD_S8(inst + 0x54) == 0)
                ST_S32(st + 0x290, (int8_t)(e65_refer_qp_bpp(X, cfg, LD_S32(inst + 8), LD_U32(out + 8), LD_U32(out + 0xc)) + 6));
            else
                ST_S32(st + 0x290, (int8_t)(e65_refer_qp_size(X, cfg, LD_S32(inst + 8), LD_S32(st + 0x358), 16,
                                                             LD_S32(out + 8)) + 4));
        }
        break;
    }
tail:
    ST_U8(st + 0x95, 0);
    ST_S64(st + 0x380, 0);
    ST_S64(st + 0x388, 0);
    ST_U32(st + 0x3a0, 0);
    ST_U32(st + 0xe0, 0);
}

/* the bit budget of the picture (FRC_get_bitrate, 0x1a6c44): restart the
 * statistics after a change, refresh them, and give the instance (+0x30)
 * the larger of the maximum rate and the lower limit at @inst+0x1c -> +0x33c.
 * As H.264 (0x197898) with the limit at +0x33c. */
static void e65_frc_bitrate(E33 *X, uint32_t cfg, uint32_t out, uint32_t st, uint32_t inst)
{
    int32_t n = LD_S32(st + 0x398), lim;
    uint8_t mode = LD_U8(cfg + 4);
    int64_t rate;

    if (n != 0) {
        if (n >= 2) {
            if (LD_S32(st + 0x358) == 0)
                return;         /* the vendor divides by zero */
            ST_S64(st + 0x368, LD_S64(st + 0x380) / (int64_t)LD_S32(st + 0x358));
            ST_U64(st + 0x428, 0);
            ST_S8(st + 0xdc, (int32_t)(((double)LD_S32(st + 0xe0) + 0.5) / (double)(n - 1)));
        }
        ST_U32(st + 0x398, 0);
        ST_U32(st + 0x338, 0);
        ST_U32(st + 0x2b8, 0);
    }
    if (mode >= T65_CBR && mode <= T65_AVBR) {
        if (mode == T65_CVBR && LD_S32(st + 0x2b8) == 0) {
            ST_U64(st + 0x2b0, 0);
            ST_U64(st + 0x288, 0);
            ST_U64(st + 0x2a8, LD_U64(st + 0x270) * (uint64_t)(int64_t)LD_S32(st + 0x258));
        }
        if (LD_S32(st + 0x338) == 0)
            e65_init_stat(X, cfg, out, st, inst);
        if (LD_S32(st + 0x398) == 0)
            e65_init_inst(X, cfg, out, st, inst);
    }
    lim = LD_S32(LD_U32(inst + 0x1c) + 0x33c);
    rate = LD_S64(st + 0x2f0);
    if (lim == 0) {
        if ((int8_t)LD_S32(cfg + 4) == T65_CBR)
            ST_S64(out + 0x30, (int64_t)((double)(float)rate * 1.1));
        else
            ST_U64(out + 0x30, (uint64_t)rate * 2u);
        return;
    }
    if (rate < 0 || (rate >> 32 == 0 && (uint32_t)rate < (uint32_t)(lim << 10)))
        ST_U64(out + 0x30, (uint32_t)(lim << 10));
    else
        ST_S64(out + 0x30, rate);
}
/* ------------------------------------------------------ I-picture share */

/* share model of the I picture (percent of the GOP budget) over the mean
 * I/P complexity v: per bit-rate class a piece-wise linear curve with the
 * knots t65_ishare_knot (the H.264 values), value base - slope * (v - lo) / (hi - lo)
 * (rows of base[5] + slope[5]; the H.264 values) */
static const int8_t t65_ishare_base[4][5] = {
    { 20, 50, 60, 80, 90 }, { 20, 50, 50, 65, 85 }, { 15, 50, 50, 55, 80 }, { 15, 40, 40, 50, 70 },
};
static const int8_t t65_ishare_slope[4][5] = {
    { 10, 30, 10, 20, 10 }, { 10, 30, 0, 15, 20 }, { 7, 35, 0, 5, 25 }, { 7, 25, 0, 10, 20 },
};
static const int8_t t65_ishare_knot[5] = { 100, 50, 20, 10, 5 };
/* bit-rate classes (bit/s / 10000, 0x1e9c9c) */
static const int32_t t65_ishare_rate[4] = { 8000, 6000, 4000, 2000 };
/* SMART/AVBR still-scene cut in percent by motion class and level (the H.264 values) */
static const int8_t t65_ishare_still[5][8] = {
    { -30, -30, -30, -30, 20, 20, 20, 20 }, { -20, -20, -20, -20, 20, 20, 20, 20 },
    { -13, -13, -13, -13, 20, 20, 20, 20 }, { 0, 0, 0, 0, 10, 10, 10, 10 },
    { 10, 10, 10, 10, 30, 40, 50, 60 },
};

static int32_t t65_ishare_curve(int k, int seg, int32_t v)
{
    int32_t lo = seg < 4 ? t65_ishare_knot[seg + 1] : 0;

    return t65_ishare_base[k][seg] - t65_ishare_slope[k][seg] * (v - lo) / (t65_ishare_knot[seg] - lo);
}

/* share of the I picture in percent (decide_Iqp_proportion, 0x1a05f4; the H.264 code at 0x18f7f8 without its asserts):
 * @rc rate block (+0 -> cfg (+0xc GOP length), +4 mode, +0x3c bit budget
 * cap), @inst
 * instance block (+0x7d still mode, +0x7e level), @st statistics (+0x234
 * ten s8 I/P complexity history, +0xa0 bit/s, +0xdc last I QP, +0x2e0
 * target, +0x358 seconds), @pic picture state (+0x1c complexity, +0x1d
 * still strength) */
static int32_t e65_i_share(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t pic)
{
    uint32_t cfg = LD_U32(rc), gop, cap, n, share;
    int32_t avg = 0, v, rate, hi, lo, seg, a, b, qi;
    int interp = 1;

    if (LD_U32(cfg + 0x10) == 0)
        return 0;
    n = LD_U32(cfg + 0x14) / LD_U32(cfg + 0x10) / 3;
    if (n > 10)
        n = 10;
    if (n) {
        /* the vendor counts the valid entries among the first n and then
         * averages the first `count` positions */
        int32_t cnt = 0, sum = 0;

        for (uint32_t i = 0; i < n; i++)
            cnt += LD_S8(st + 0x234 + i) != -1;
        for (int32_t i = 0; i < cnt; i++)
            sum += LD_S8(st + 0x234 + i);
        if (cnt)
            avg = sum / cnt;
    }
    v = LD_S8(pic + 0x1c);
    if ((v + avg) / 2 > v)
        v = (v + avg) / 2;

    rate = (int32_t)(LD_S64(st + 0xa0) / 10000);
    if (rate > 8000) {
        hi = lo = 0;
        interp = 0;
    } else if (rate > 6000) {
        hi = 1, lo = 0;
    } else if (rate > 4000) {
        hi = 2, lo = 1;
    } else if (rate > 2000) {
        hi = 3, lo = 2;
    } else {
        hi = lo = 3;
        interp = 0;
    }
    seg = v > 50 ? 0 : v > 20 ? 1 : v > 10 ? 2 : v > 5 ? 3 : 4;
    if (v > 0) {
        a = t65_ishare_curve(hi, seg, v);
        b = t65_ishare_curve(lo, seg, v);
    } else {
        a = t65_ishare_base[hi][4];
        b = t65_ishare_base[lo][4];
    }
    share = (uint32_t)a;
    if (interp)
        share += (int32_t)((uint32_t)(b - a) * (uint32_t)(rate - t65_ishare_rate[hi]))
               / (t65_ishare_rate[lo] - t65_ishare_rate[hi]);

    /* long GOPs: a larger I share */
    gop = LD_U32(cfg + 0xc);
    if (gop >= 30)
        share = ((((gop - 30) >> 1) + 30) * share) / gop;
    qi = LD_S8(st + 0xdc);
    cap = LD_U32(rc + 0x3c);
    if (qi != -1 && qi < 37) {
        if ((int32_t)share < 6)
            goto cap;
        share -= (int32_t)((uint32_t)(37 - qi < 30 ? 37 - qi : 30) * (share - 5)) / 30;
    }
    if ((int32_t)share >= 21 && LD_S8(inst + 0x7d) != 0) {
        int8_t mode = (int8_t)LD_S32(rc + 4);
        int32_t m;
        int cls;

        if (mode != T65_SMART && mode != T65_AVBR)
            goto cap;
        m = (qi + LD_S32(st + 0x58)) >> 1;
        cls = m < 11 ? 0 : m < 21 ? 1 : m < 33 ? 2 : m < 46 ? 3 : m < 52 ? 4 : 3;
        if (LD_S8(inst + 0x7d) == 1)
            share = (int32_t)((100 - LD_S8(pic + 0x1d) * t65_ishare_still[cls][LD_S8(inst + 0x7e)] / 100)
                              * (int32_t)share) / 100;
        share = (int8_t)share < 20 ? 20 : (uint32_t)(int8_t)share;
    }
cap: {
        int32_t pct = 100;

        if (cap) {
            int64_t per = (LD_S64(st + 0x2e0) * LD_S32(st + 0x358)) >> 10;
            int64_t q = per ? (int64_t)(uint32_t)((cap >> 10) * 100) / per : 0;

            pct = q < 1 ? 1 : q > 100 ? 100 : (int32_t)q;
        }
        if ((int32_t)share < 0)
            return 0;
        return (int8_t)((int32_t)share <= pct ? (int32_t)share : pct);
    }
}

