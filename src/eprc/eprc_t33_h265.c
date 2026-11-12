/*
 * Ingenic "eprc" picture rate control, T33 revision, H.265 (#295, planned):
 * reimplemented from the vendor T32 libimp 2.2.0 (eprc_default_set_T33,
 * JZ_VPU_RC_*_T33 and their helpers at 0x19d550..0x1b1db0), which the vendor
 * uses for H.265 on the T32 (HERA).  The H.265 controller is a second build
 * of the H.264 one (src/eprc/eprc_t33_h264.c) with its own state layout and
 * HEVC rules; the state keeps the vendor layout in the arena of
 * eprc_t33_mem.h.  Work in progress (not built yet): the functions below
 * are checked call by call against the vendor controller
 * (tools/t32/eprc33/diff265.py and rand265p.py with the swap build of
 * swap265.py).  Most of them are the H.264 code on the H.265 handle layout:
 * the rate block e+0x820.. of H.264 sits at e+0x390.. (e+0x88c.. at
 * e+0x400..), the inner state blocks (rc, inst, st, out) keep the H.264
 * layout.  Still to do: JZ_VPU_RC_FRAME_START_T33 and VPI_set_sliceinfo
 * (vpi_emc_*, pad_maproi).
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

static void t65_iqp_reset(E33 *X, uint32_t st)
{
    ST_S8(st + 0x111, 52);
    ST_S8(st + 0x110, 0);
    for (int i = 0; i < 0x50; i++)
        ST_U8(st + 0x1d0 + i, 0);
    for (int i = 0; i < 10; i++)
        ST_S8(st + 0x22a + i, 0);
}

/* the vendor skips QP 39 in two places: one step in the direction of dQP */
static int32_t t65_skip39(int32_t q, int32_t dqp)
{
    if ((int8_t)q != 39)
        return (int8_t)q;
    return dqp < 1 ? (int8_t)(q - (dqp != 0)) : (int8_t)(q + 1);
}

static uint32_t t65_gop_share(uint32_t gop, uint32_t share)
{
    return gop < 30 ? share : share * gop / (((gop - 30) >> 1) + 30);
}

/* budgets of the I picture and of a P picture for the GOP (+0x80, +0x88) */
static void t65_iqp_budgets(E33 *X, uint32_t inst, uint32_t st, int64_t share)
{
    int32_t sec = LD_S32(st + 0x358);

    ST_S64(st + 0x80, (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)share * (uint64_t)(int64_t)sec) / 100);
    ST_S64(st + 0x88, (int64_t)((uint64_t)LD_S64(st + 0x2e0) * (uint64_t)(int64_t)LD_S32(inst + 0x10)
                                * (uint64_t)(int64_t)sec) / 100);
}

/* QP of an I picture (get_I_frame_qp, 0x1a4658; the H.264 code at 0x187b78).  @rc rate block (+0 ->
 * cfg, +4 mode, +5 fixed I QP or -1), @inst instance parameters (+0x10 /
 * +0x14 I share limits in percent, +0x28 maximum QP step, +0x54 reference
 * model, +0x7c history use, +0x78 SMART/AVBR I QP cap), @st statistics,
 * @out picture block (+8 int64 bit rate, +0x20 -> picture state) */
static int32_t e65_iqp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
{
    uint32_t cfg = LD_U32(rc), pic, bits_a = st + 0xa0;
    int32_t sec = LD_S32(st + 0x358), gop, share, qp_p, last, base, refqp, qcls, qcls8, stable, low_motion;
    int32_t q_lo, q_mid, qa, qb, qlim, d_hi, qp, cap, lo_pct, hi_pct, h1;
    int64_t bits, prev, est, est100, per10, per_out, r;
    int keep = 0, stable_scene;
    float ratio;

    if (LD_S32(st + 0x14) == 0) {
        /* first picture */
        ST_S8(st + 0x111, 52);
        ST_S8(st + 0xdc, -1);
        ST_S8(st + 0x110, 0);
        t65_iqp_budgets(X, inst, st, 60);
        if (LD_S8(rc + 5) != -1)
            return LD_S8(rc + 5);
        if (LD_S8(inst + 0x54) == 0)
            return e65_refer_qp_bpp(X, cfg, LD_S32(inst + 8), LD_U32(out + 8), LD_U32(out + 0xc));
        return e65_refer_qp_size(X, cfg, LD_S32(inst + 8), sec, 0, LD_S32(out + 8));
    }
    gop = LD_S32(cfg + 0xc);
    if (gop == 1) {
        int64_t v = (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)(int64_t)sec);

        ST_S64(st + 0x80, v);
        ST_S64(st + 0x88, v);
        return (int8_t)((int8_t)LD_U32(st + 0x58) + (int8_t)e65_iratio_idqp(X, bits_a, LD_S32(st + 0x68), 100));
    }
    pic = LD_U32(out + 0x20);
    share = e65_i_share(X, rc, inst, st, pic);

    /* QP class of the last P picture against the P budget */
    {
        int8_t dir = LD_S8(st + 0x94);
        int32_t t = dir == 1 ? (int32_t)((double)LD_S64(st + 0x48) * 1.2)
                  : dir == -1 ? (int32_t)((double)LD_S64(st + 0x48) * 0.8) : (int32_t)LD_S64(st + 0x48);

        qp_p = LD_S32(st + 0x5c);
        qcls = (int8_t)qp_p;
        if (LD_S8(st + 0x7a) == 0) {
            int64_t pb = (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)(int64_t)(100 - share)
                                   * (uint64_t)(int64_t)sec);
            float p, ft = (float)t;
            int c;

            pb = (gop - 1) ? pb / (int64_t)(gop - 1) : 0;
            p = (float)(pb / 100);
            if (ft < p * 0.2f)
                c = 0;
            else if (ft < p * 0.5f)
                c = 1;
            else if (ft < p)
                c = 2;
            else if (ft < p + p)
                c = 3;
            else if (ft < p * 3.5f)
                c = 4;
            else if (ft < p * 6.0f)
                c = 5;
            else
                c = 6;
            qcls = (int8_t)((int8_t)qp_p - 3 + c);
        }
        if (LD_S8(st + 0x96) != 0)
            qcls = (int8_t)((int8_t)qcls * 2 - (int8_t)qp_p);
        qcls8 = qcls & 0xff;
    }

    bits = LD_S64(st + 0xa0);
    prev = LD_S64(st + 0x70);
    if (LD_S32(st + 0x28) == 0)
        return 0;
    ST_S8(st + 0x1d, (int8_t)(LD_S64(st + 0x20) / LD_S32(st + 0x28)));
    low_motion = LD_S8(st + 0x1d) < 10;
    stable = 1;
    if (!low_motion || LD_S8(st + 0x1c) > 14) {
        int64_t d = bits > prev ? bits - prev : prev - bits;

        stable = !(bits / 10 < d * 2);
    }
    ST_S8(st + 0xaa, stable);

    ratio = (float)(int32_t)(bits <= 0 ? 1 : (uint32_t)bits) / (float)(int32_t)(prev <= 0 ? 1 : (uint32_t)prev);
    est = (int64_t)(ratio * (float)(LD_S64(st + 0x38) >> 10));
    est100 = est * 100;
    per10 = (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)(int64_t)sec) >> 10;
    if (per10 <= 0)
        per10 = 1;
    refqp = e65_refer_qp_bpp(X, cfg, LD_S32(inst + 8), LD_U32(out + 8), LD_U32(out + 0xc));

    stable_scene = LD_S8(inst + 0x7c) != 0 && LD_S8(pic + 8) == 0 && LD_S8(st + 0x98) != 0;
    ST_S8(st + 0x99, stable_scene);
    if (!stable_scene) {
        if (!stable) {
            uint32_t s;

            t65_iqp_reset(X, st);
            s = t65_gop_share((uint32_t)gop, (uint32_t)share);
            qp = qcls;
            if ((int32_t)s > 19) {
                int32_t d = e65_iratio_idqp(X, bits_a, (int32_t)(est100 / per10), share), m;

                qp = t65_skip39((int8_t)((int8_t)LD_U32(st + 0x58) + 2 + (int8_t)d), d);
                m = LD_S8(st + 0xdc) < qp_p ? LD_S8(st + 0xdc) : qp_p;
                if (LD_S32(inst + 0x28) < m - qp)
                    qp = (int8_t)((int8_t)m - (int8_t)LD_S32(inst + 0x28));
                if (qcls8 <= qp)
                    qp = qcls8;
                qp = (int8_t)qp;
                if (LD_S8(inst + 0x18) <= 5) {
                    if (qcls8 > 32)
                        qp = qp < 33 ? 33 : qp;
                    else
                        qp = qcls;
                }
            }
            goto out;
        }
    } else {
        if (!stable) {
            t65_iqp_reset(X, st);
            qp = LD_S8(st + 0x60);
            goto out;
        }
        est100 = (est < 1 ? 1 : est) * 100;
        share = 95;
        keep = 1;
    }

    /* the estimated size of this I picture against the bounds */
    per_out = (int64_t)((uint64_t)LD_S64(out + 8) * (uint64_t)(int64_t)sec) >> 10;
    if (per_out <= 0)
        per_out = 1;
    last = LD_S32(st + 0x58);
    r = est100 / per_out;
    d_hi = e65_iratio_idqp(X, bits_a, (int32_t)r, LD_S32(inst + 0x14));
    q_lo = (int8_t)e65_iratio_idqp(X, bits_a, (int32_t)r, LD_S32(inst + 0x10));
    base = (int8_t)last;
    {
        int32_t qwin;
        int64_t s8 = LD_S64(st + 8);

        if (est100 < per_out * (int64_t)LD_S32(inst + 0x14))
            ST_S8(st + 0x111, base);
        else if (est100 > per_out * (int64_t)LD_S32(inst + 0x10))
            ST_S8(st + 0x110, base);
        if (s8 == 0 || s8 > LD_S64(st + 0x38)) {
            qwin = LD_S8(st + 0x110);
        } else {
            qwin = base;
            ST_S8(st + 0x110, base);
        }
        cap = qwin;
    }
    r = est100 / per10;
    if (share < 11) {
        qb = (int8_t)(base + (int8_t)e65_iratio_idqp(X, bits_a, (int32_t)r, 0));
        hi_pct = share + 10;
    } else {
        qb = (int8_t)(base + (int8_t)e65_iratio_idqp(X, bits_a, (int32_t)r, share - 10));
        hi_pct = share < 90 ? share + 10 : 100;
    }
    qa = (int8_t)(base + (int8_t)e65_iratio_idqp(X, bits_a, (int32_t)r, hi_pct));
    q_mid = e65_iratio_idqp(X, bits_a, (int32_t)r, share);
    {
        int32_t sum = q_mid + last, cand = qcls;
        int use_mid = 0;  /* candidate: the QP of the I share (else the P class) */

        if (keep) {
            qa = qcls;
        } else {
            if (refqp < sum) {
                int32_t q2 = e65_iratio_idqp(X, bits_a, (int32_t)(est100 / per_out), share);

                if (q2 + last <= refqp) {
                    q_mid = (int8_t)((int8_t)refqp - base);
                    qa = refqp;
                    qb = refqp;
                }
                use_mid = low_motion && LD_S8(st + 0x1c) < 15 && !(q_mid + last < 27);
            } else {
                use_mid = low_motion && LD_S8(st + 0x1c) <= 14 && !(sum < 27);
            }
            if (use_mid)
                cand = t65_skip39((int8_t)(base + (int8_t)q_mid), q_mid);
            if (!(cand < qa)) {
                qa = cand;
                if (qb > 26) {
                    if (cand < qb)
                        qb = cand;
                    qa = (int8_t)qb;
                }
            }
        }
    }
    /* bound by the history of I pictures */
    lo_pct = share < 90 ? share + 10 : 100;
    hi_pct = share < 90 && share < 11 ? 0 : share - 10;
    h1 = e65_history_iqp(X, st, lo_pct, hi_pct, qa, (int32_t)LD_U32(st + 0x360));
    if (refqp < h1 && !keep) {
        int32_t h2 = e65_history_iqp(X, st, lo_pct, hi_pct, qa, LD_S32(out + 8));

        h1 = refqp < h2 ? (int8_t)(h2 < h1 ? h2 : h1) : refqp;
    }
    {
        uint32_t s = t65_gop_share(LD_U32(cfg + 0xc), (uint32_t)share);

        qp = qcls;
        if ((int32_t)s > 19) {
            qp = h1;
            if (LD_S8(inst + 0x18) < 6) {
                qp = qcls;
                if (qcls8 > 32)
                    qp = (int8_t)(h1 < 33 ? 33 : h1);
            }
        }
    }
    if (!keep) {
        int32_t m = LD_S8(st + 0xdc) <= qp_p ? LD_S8(st + 0xdc) : qp_p, c;
        int64_t b2;

        if (LD_S32(inst + 0x28) < m - qp)
            qp = (int8_t)((int8_t)m - (int8_t)LD_S32(inst + 0x28));
        qlim = qp < qcls8 ? qp : qcls8;
        qp = (int8_t)qlim;
        if (qp < (int8_t)(base + q_lo)) {
            qp = (int8_t)(base + q_lo);
        } else {
            c = (int8_t)(base + (int8_t)d_hi);
            if (c > 26)
                qp = qp < c ? (int8_t)qlim : c;
        }
        b2 = (int64_t)((uint64_t)LD_S64(st + 0x2f0) + (uint64_t)LD_S64(inst + 0x30)) / 2;
        if (b2 < LD_S64(st + 0x368) && qp <= last + 1)
            qp = (int8_t)(base + 2);
    }
    if (cap < qp) {
        if (LD_S8(st + 0x111) <= qp)
            qp = LD_S8(st + 0x111) - 1;
        qp = (int8_t)qp;
    } else {
        qp = (int8_t)(cap + 1);
    }
out:
    t65_iqp_budgets(X, inst, st, (int64_t)share);
    qp = (int8_t)(qp + LD_S8(st));
    if (LD_S8(st + 0x248) > 4)
        return qp;
    {
        int8_t mode = (int8_t)LD_S32(rc + 4);

        if (mode != T65_SMART && mode != T65_AVBR)
            return qp;
    }
    return LD_S32(inst + 0x78) < qp ? (int8_t)LD_S32(inst + 0x78) : qp;
}

/* ---------------------------------------------------------- P picture QP */

/* I-share curve over the complexity when no I picture follows (0x1e9cd4) */
static const int8_t t65_pshare_base[5] = { 10, 40, 60, 70, 80 };
static const int8_t t65_pshare_slope[5] = { 5, 30, 20, 10, 10 };

static int32_t t65_pdqp(E33 *X, uint32_t inst, uint32_t st, uint32_t out, int64_t bits, int32_t row)
{
    return e65_adjust_p_dqp(X, inst, st, out, 0, (uint32_t)bits, (int32_t)(bits >> 32), (int8_t)row);
}

/* QP of a P picture from the remaining GOP budget (estimate_qp, 0x1a1068; the H.264 code at 0x18ff78):
 * @rc rate block (+0 -> cfg, +4 mode), @inst instance parameters (+0x18
 * budget rule 0..7, +0x20 int64 minimum bits, +0x55/+0x58 QP floor rule,
 * +0x60 CVBR long-term bits), @st statistics (+0x2c position in the GOP,
 * +0x190.. five position thresholds with their budgets (+0x140) and
 * estimates (+0x118), +0x5c last P QP, +0x97 hold counter), @out picture
 * block (+0x20 -> picture state, +9 scene cut).  Returns the QP; the
 * budgets go to st+0x80.., the direction to st+0x94. */
static int32_t e65_estimate_qp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
{
    uint32_t cfg = LD_U32(rc), gop = LD_U32(cfg + 0xc), pic;
    int64_t sec = (int64_t)LD_S32(st + 0x358), a, s, v1, v2, d, l3, l5, l6, l59, l60, c59, c60, imin;
    int32_t pos, cls, a4, dqp, q_p, c43, share, pct, mode;
    uint8_t b;
    int8_t c17;
    float f57;

    if (LD_S64(st + 0x2f0) * (int64_t)LD_S32(st + 0x2d8) < LD_S64(st + 0x380) + LD_S64(st + 0x330)
        && LD_S8(inst + 0x18) < 7) {
        int32_t v = 4 - LD_S8(inst + 0x18);
        int8_t c;

        ST_U8(st + 0x96, 1);
        if (v < LD_S8(st + 0x97))
            v = LD_S8(st + 0x97);
        c = (int8_t)v;
        ST_S8(st + 0x97, c < 0 ? 0 : c < 5 ? c : 4);
    }
    pos = LD_S32(st + 0x2c);
    ST_U8(st + 0x96, 0);    /* the vendor clears it in a delay slot (H.264 0x190038) */
    cls = pos >= LD_S32(st + 0x1a0) ? 4 : pos >= LD_S32(st + 0x19c) ? 3 : pos >= LD_S32(st + 0x198) ? 2
        : pos >= LD_S32(st + 0x194) ? 1 : 0;
    if (LD_S8(st + 0xab) != 0 || LD_S8(st + 0x7a) != 0) {
        q_p = LD_S32(st + 0x5c);
        ST_U32(st + 0x1a4, 0);
        ST_S8(st + 0x94, 0);
        return (int8_t)q_p;
    }

    /* the bits left for the rest of the GOP, per picture */
    mode = LD_S8(rc + 4);
    a = LD_S64(st + 0x50);
    v1 = LD_S64(st + 0x370) * sec - a;
    v2 = LD_S64(st + 0x378) * sec - a;
    if (mode == T65_CVBR && LD_S64(inst + 0x60) <= LD_S64(st + 0x288))
        v1 = LD_S64(st + 0x270) * sec - a;
    s = LD_S64(st + 0x168) + LD_S64(st + 0x170) + LD_S64(st + 0x178) + LD_S64(st + 0x180) + LD_S64(st + 0x188);
    v1 -= s;
    v2 -= s;
    d = (int32_t)(gop - 1) - pos;
    if (d < 1)
        d = 1;
    imin = LD_S64(inst + 0x20);
    l3 = t65_max64(imin, (v1 < 1 ? 1 : v1) / d);
    if (gop == 0 || (float)(uint32_t)(gop * 100 - 100) == 0.0f)
        return 0;           /* the vendor asserts */
    f57 = (float)(uint32_t)(gop * 100 - 100);

    /* budget and size estimate of the position class */
    pic = LD_U32(out + 0x20);
    if (cls == 4) {
        int32_t m;

        share = e65_i_share(X, rc, inst, st, pic) & 0xff;
        if (LD_S8(st + 0x99) != 1 && gop > 29)
            share = ((((gop - 30) >> 1) + 30) * (uint32_t)share / gop) & 0xff;
        m = (int32_t)gop - LD_S32(st + 0x1a0);
        f57 = (float)(int32_t)(100 - share) / f57;
        if (m == 1)
            return 0;       /* the vendor asserts */
        c59 = t65_div64(LD_S64(st + 0x160), (int64_t)(uint32_t)(m - 1));
        c60 = (int64_t)((float)LD_S64(st + 0x138) * f57);
        a4 = 4;
    } else {
        int32_t v = LD_S8(pic + 0x1c), lo, seg, t_hi, t_lo;

        if (v > 50)
            lo = 50, seg = 0;
        else if (v > 20)
            lo = 20, seg = 1;
        else if (v > 10)
            lo = 10, seg = 2;
        else if (v > 5)
            lo = 5, seg = 3;
        else
            lo = 0, seg = 4;
        share = 80;
        if (v > 0)
            share = t65_pshare_base[seg] - t65_pshare_slope[seg] * (v - lo) / (t65_ishare_knot[seg] - lo);
        {
            int32_t qi = LD_S8(st + 0xdc);

            if (qi != -1 && qi < 37 && share > 5)
                share -= (int32_t)((uint32_t)(37 - qi < 30 ? 37 - qi : 30) * (uint32_t)(share - 5)) / 30;
        }
        pct = 95;
        if (LD_S8(st + 0x99) != 1) {
            if (share < 0)
                share = 0, pct = 0;
            else {
                if (share > 100)
                    share = 100;
                pct = share & 0xff;
            }
            if (gop > 29)
                pct = ((((gop - 30) >> 1) + 30) * (uint32_t)share / gop) & 0xff;
        }
        t_hi = LD_S32(st + cls * 4 + 0x194);
        t_lo = LD_S32(st + cls * 4 + 0x190);
        f57 = (float)(int32_t)(100 - pct) / f57;
        if (t_hi == t_lo)
            return 0;       /* the vendor asserts */
        c59 = t65_div64(LD_S64(st + cls * 8 + 0x140), t_hi - t_lo < 1 ? 1 : t_hi - t_lo);
        c60 = (int64_t)((float)LD_S64(st + cls * 8 + 0x118) * f57);
        a4 = cls;
    }
    l59 = c59;
    l60 = c60;
    b = LD_U8(inst + 0x18);

    if (mode == T65_CBR && b >= 8) {
        l59 = imin < 0 ? 0 : imin;
        dqp = t65_pdqp(X, inst, st, out, l59, a4);
        if (dqp >= 0)
            goto p0904;
        goto p0f60;
    }
    /* the budget rule of inst+0x18 (jump tables 0x1e96d8 / 0x1e96f8) */
    l6 = mode == T65_CBR ? t65_min64(l59, l3) : t65_min64(t65_max64(l60, l59), l3);
    if (b >= 8) {
        l6 = 0;
    } else {
        l59 = l6;
        switch (b) {
        case 0: {
            int64_t x = LD_S64(st + 0x360) * sec - LD_S64(st + 0x380);

            l59 = t65_div64(x < 1 ? 1 : x, (int64_t)(uint32_t)(gop - LD_S32(st + 0x398)));
            l60 = t65_max64(l59, t65_min64(l60, l6) / 2);
            l6 = t65_min64(l60, (l6 + l3) / 2);
            break;
        }
        case 1:
            l6 = t65_min64(l6, (l6 + l3) / 2);
            break;
        case 2: {
            int64_t r;

            l5 = t65_max64(l6, l60 < 1 ? 1 : l60);
            l59 = l5;
            if (cls == 4) {
                r = t65_div64((l6 < l60 ? l60 - l6 : l6 - l60) * 100, l5);
                l59 = r < 50 ? l60 : l5;
            }
            l6 = t65_min64(l59, (l3 + l6) / 2);
            break;
        }
        case 3:
        case 4: {
            int8_t c = LD_S8(pic + 9);

            l5 = l59;
            if (c == 0) {
                if (l60 < l59) {
                    l59 = (l59 + l60) / 2;
                    goto tail34;
                }
            } else if (l59 < l60) {
                int64_t r;

                if (l59 < 1)
                    l5 = 1;
                r = t65_div64((l5 < l60 ? l60 - l5 : l5 - l60) * 100, l5);
                l59 = r < 50 ? (l60 + l5) / 2 : t65_div64(l60 + l5 * 4, 5);
            }
            if (cls == 4) {
                if (l60 < l5)
                    l59 = (l5 + l60) / 2;
                else
                    l59 = c == 0 ? l5 : l60;
            }
tail34:
            l6 = t65_min64(l59, (l3 + l5) / 2);
            break;
        }
        case 5:
            b = 5;
            l6 = t65_min64(t65_max64(l60, l6), (l3 + l6) / 2);
            break;
        case 6:
            l6 = t65_min64(t65_max64(l60, l6), (l6 + l3) / 2);
            break;
        case 7:
            l6 = t65_max64(l6, l60);
            break;
        }
    }
    l59 = t65_max64(l6, imin);
    dqp = t65_pdqp(X, inst, st, out, l59, a4);
    if (mode == T65_CVBR) {
        int32_t lim = LD_S32(st + 0x290);

        q_p = LD_S32(st + 0x5c);
        if (lim < dqp + q_p) {
            int64_t l62;

            if (LD_S64(inst + 0x60) <= LD_S64(st + 0x288)) {
                int64_t x = LD_S64(st + 0x270) * sec - a - s;

                l3 = t65_max64((x < 1 ? 1 : x) / d, imin);
            }
            {
                double d59 = (double)l59, d3 = (double)l3;

                l62 = d59 < d3 * 0.8 ? (int64_t)(d3 * 0.8) : (int64_t)d59;
            }
            dqp = t65_pdqp(X, inst, st, out, l62, a4);
            if (dqp + q_p < lim)
                dqp = (int8_t)((int8_t)lim - (int8_t)q_p);
        }
        if (dqp < 0)
            goto p0f6c;
        if (dqp == 0) {
            if (LD_S8(st + 0x97) < 1)
                goto p0d88;
        } else {
            goto p0920;
        }
        goto p118c;
    }
    if (dqp < 0) {
        if (mode == T65_CBR)
            goto p0f60;
        goto p0f6c;
    }
p0904:
    q_p = LD_S32(st + 0x5c);
    if (dqp < 1)
        goto p1114;
    if (mode != T65_CBR)
        goto p0920;
    if ((int8_t)b >= 5 && LD_S8(pic + 9) != 0)
        goto p0920;
    c43 = LD_S8(st + 0x97);
    goto p0954;
p0920:
    q_p = LD_S32(st + 0x5c);
    if (t65_pdqp(X, inst, st, out, l3, a4) > 0) {
        c43 = LD_S8(st + 0x97);
        goto p0954;
    }
p118c:
    q_p = LD_S32(st + 0x5c);
    c43 = 0;
    dqp = 0;
    goto p0964;
p0954:
    (void)c43;
p0d88:
    c43 = dqp;
    goto p0964;
p0f60:
    if ((int8_t)b > 4)
        goto p0f6c;
    q_p = LD_S32(st + 0x5c);
    goto p1114;
p0f6c:
    q_p = LD_S32(st + 0x5c);
    if (LD_S8(pic + 9) == 0) {
        /* no scene cut: the QP the size of the last I picture asks for */
        int64_t a0 = LD_S64(st + 0xa0), a7 = LD_S64(st + 0x70), u61, per;
        int32_t n = a0 <= 0 ? 1 : (int32_t)a0, den;
        int32_t x, q;

        if (a7 <= 0)
            den = 1;
        else if ((den = (int32_t)a7) == 0)
            return 0;       /* the vendor asserts */
        u61 = (int64_t)((float)(LD_S64(st + 0x38) >> 10) * ((float)n / (float)den));
        per = (LD_S64(st + 0x360) * sec) >> 10;
        x = (int32_t)t65_div64(u61 * 100, per);
        q = e65_iratio_idqp(X, st + 0xa0, x, 50);
        if (q_p <= q + LD_S32(st + 0x58)) {
            if (t65_pdqp(X, inst, st, out, (v2 < 1 ? 1 : v2) / d, a4) >= 0)
                dqp = 0;
        }
    }
p1114:
    if (LD_S8(st + 0x97) < 1) {
        if (dqp >= 0) {
            c43 = dqp;
            goto p0964;
        }
    } else {
        if (dqp == 0 || q_p < (int32_t)LD_U8(cfg + 0x18) - 1)
            goto p118c;
        dqp = -1;
    }
    if (LD_S8(inst + 0x55) == 0)
        goto p0d88;
    {
        int32_t m = LD_S32(inst + 0x58);

        if (q_p < m) {
            c43 = -1;
            dqp = -1;
        } else {
            if (m <= dqp + q_p)
                goto p0d88;
            c43 = (int8_t)((int8_t)m - 1 - (int8_t)q_p);
            dqp = c43;
        }
    }
p0964:
    ST_S64(st + 0x80, l59);
    ST_S64(st + 0xc0, c59);
    ST_S64(st + 0xb8, c60);
    ST_S64(st + 0x88, l3);
    ST_S64(st + 0xb0, l3);
    ST_S32(st + 0xd8, a4);
    ST_S64(st + 0xd0, LD_S64(st + a4 * 8 + 0x140));
    ST_S64(st + 0xc8, LD_S64(st + a4 * 8 + 0x118));
    ST_S32(st + 0x1a4, dqp);
    c17 = dqp >= 1 ? 1 : dqp != 0 ? -1 : 0;
    ST_S8(st + 0x94, c17);
    return (int8_t)((int8_t)q_p + (int8_t)c43);
}

/* P budget share per position class, row 1 when st+0x68 < 51 (0x1e9c44);
 * the size estimate of class k scales by row[k - 1] */
static const uint8_t t65_pclass_share[2][5] = { { 80, 95, 100, 95, 100 }, { 100, 100, 100, 100, 100 } };
/* GOP-2 (st+0x30 == 2) dQP steps over the motion level (0x1e9718) */
static const uint8_t t65_g2_level[11] = { 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

static int64_t t65_pos1(int64_t v)
{
    return v < 1 ? 1 : v;
}

/* QP of a P picture (get_P_frame_qp, 0x1a31ec; the H.264 code at 0x191960): refresh the class budgets
 * (st+0x140..) and size estimates (st+0x118..) at a class boundary, then
 * the fixed QP before the first I picture (st+0x398 == 0), the GOP-2 level
 * step (st+0x30 == 2) or estimate_qp. */
static int32_t e65_p_qp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
{
    uint32_t cfg = LD_U32(rc), gop = LD_U32(cfg + 0xc), pic = LD_U32(out + 0x20);
    int64_t sec = (int64_t)LD_S32(st + 0x358), r360 = LD_S64(st + 0x360), a = LD_S64(st + 0x50), imin;
    int64_t t1 = r360 * sec;
    int32_t pos = LD_S32(st + 0x2c), row = LD_S32(st + 0x68) < 51, k, idx;
    uint32_t lv;

    ST_U8(st + 0x99, LD_S8(inst + 0x7c) != 0 && LD_U8(pic + 8) == 0 && LD_S8(st + 0x98) != 0);
    for (k = 0; k < 5; k++) {
        int64_t s = 0, b, e, tot, lim1, lim2, pk;
        int32_t j, dd;
        uint32_t d = gop - 1 - (uint32_t)pos;

        if (LD_S32(st + 0x190 + k * 4) != pos)
            continue;
        if (k == 0) {
            if (gop == 1)
                return 0;   /* the vendor asserts */
            b = t65_pos1(t1 - a) * (int64_t)LD_S32(st + 0x194) / (int64_t)(gop - 1);
            ST_S64(st + 0x140, t65_pos1((int64_t)((uint64_t)b * t65_pclass_share[row][0]) / 100));
            ST_S64(st + 0x118, t1);
            continue;
        }
        for (j = 0; j < k; j++)
            s += LD_S64(st + 0x168 + j * 8);
        b = t65_pos1(t1 - a - s);
        if (k < 4)
            b = b * (int64_t)(LD_S32(st + 0x194 + k * 4) - pos) / (int64_t)d;
        ST_S64(st + 0x140 + k * 8, t65_pos1((int64_t)((uint64_t)b * t65_pclass_share[row][k]) / 100));
        e = (int64_t)((uint64_t)LD_S64(st + 0x160 + k * 8) * 100) / t65_pclass_share[row][k - 1];
        e = (int64_t)((uint64_t)e * d);
        dd = pos - LD_S32(st + 0x18c + k * 4);
        e /= dd < 1 ? 1 : dd;
        tot = e + s + a;
        lim1 = (r360 + LD_S64(st + 0x370)) / 2 * sec;
        lim2 = (r360 + LD_S64(st + 0x378)) / 2 * sec;
        pk = LD_S64(st + 0x110 + k * 8);
        if (k == 4) {
            if (tot > lim1 && pk < t1)
                ;
            else if (tot < lim2)
                pk = pk > t1 ? pk : t1;
            else
                pk = t1;
        } else if (tot > lim1) {
            pk = (int64_t)((double)pk * 0.98);
        } else if (tot < lim2) {
            pk = (int64_t)((double)pk * 1.02);
        }
        ST_S64(st + 0x118 + k * 8, pk);
    }
    if (LD_S8(inst + 0x7f) == 3)
        ST_U8(st + 0x9a, LD_S8(pic + 0x1c) < 51 ? 2 : 1);

    imin = LD_S64(inst + 0x20);
    if (LD_S32(st + 0x398) == 0) {
        /* no I picture yet: the per-picture budgets and the start QP */
        int32_t q;
        int8_t c;

        if (gop == 1)
            return 0;       /* the vendor asserts */
        ST_S64(st + 0x88, t65_max64(t65_pos1(LD_S64(st + 0x370) * sec) / (int64_t)gop, imin));
        ST_S64(st + 0x80, t65_max64(t65_pos1(t1) / (int64_t)gop, imin));
        c = LD_S8(st + 0xdc);
        if (c > 0)
            return c;
        q = LD_S32(st + 0x5c);
        return q < 1 ? LD_S8(st + 0x60) : (int8_t)q;
    }
    if (LD_S8(st + 0x30) != 2)
        return e65_estimate_qp(X, rc, inst, st, out);
    lv = gop < 30 ? LD_U32(st + 0x68) : gop * LD_U32(st + 0x68) / (((gop - 30) >> 1) + 30);
    if (LD_S32(st + 0x14) != 1 && (LD_S8(inst + 0x4c) != 0 || LD_S8(pic + 0x1c) > 9))
        return e65_estimate_qp(X, rc, inst, st, out);
    for (idx = 0; idx < 11 && (int32_t)lv >= t65_g2_level[idx]; idx++)
        ;
    idx = idx < 11 ? idx - 3 : 8;
    if (gop == 1)
        return 0;           /* the vendor asserts */
    {
        int64_t i38 = LD_S64(st + 0x38);

        ST_S64(st + 0x88, t65_max64(t65_pos1(LD_S64(st + 0x370) * sec - i38) / (int64_t)(gop - 1), imin));
        ST_S64(st + 0x80, t65_max64(t65_pos1(t1 - i38) / (int64_t)(gop - 1), imin));
    }
    return (int8_t)(LD_S32(st + 0x58) + idx);
}

/* default controller settings (eprc_default_set_T33, 0x1ad230): @e is
 * the controller block, its +4 the configuration (1280x720, 25 fps, QP
 * 10..48, HEVC QP offsets at +0x28..+0x2e); the per-mode block at +0x39c is
 * filled for the mode at +0x394 (the H.264 layout at +0x820 moved to +0x390) */
static void e65_default_set(E33 *X, uint32_t e)
{
    uint32_t cfg = e + 4;

    ST_U32(e + 0x3e4, cfg);
    ST_U32(e + 0x390, cfg);
    ST_U32(e + 0x3f0, cfg);
    ST_U32(e + 0x408, cfg);
    ST_U8(e + 0x28, 75);
    ST_U8(e + 0x29, 75);
    ST_U8(e + 0x2a, 75);
    ST_U8(e + 0x2b, 50);
    ST_U8(e + 0x2c, 50);
    ST_U8(e + 0xc, 1);
    ST_U16(e + 4, 1280);
    ST_U16(e + 6, 720);
    ST_U16(e + 8, 120);
    ST_U16(e + 10, 80);
    ST_U32(e + 0x10, 25);
    ST_U32(e + 0x18, 25);
    ST_U32(e + 0x14, 1);
    ST_U8(e + 0x1c, 48);
    ST_U8(e + 0x1d, 10);
    ST_U8(e + 0x1e, 0);
    ST_U8(e + 0x2d, 20);
    ST_U8(e + 0x2e, 30);
    ST_U8(e + 0x2f, 0);
    ST_U8(e + 0x30, 0);
    ST_U8(e + 0x395, 0x24);
    ST_U8(e + 0x396, 0);
    ST_U8(e + 0x397, 0);
    ST_U8(e + 0x398, 0);
    ST_U32(e + 0x3cc, 0);
    ST_U32(e + 0x3d0, 0);
    ST_U8(e + 0x3c0, 8);
    ST_U8(e + 0x3c1, 3);
    switch (LD_U8(e + 0x394)) {
    case 0:
        ST_U8(e + 0x39c, 30);
        ST_U8(e + 0x39d, 33);
        ST_U8(e + 0x39e, 33);
        break;
    case 1:
        ST_U32(e + 0x39c, 6);
        ST_U32(e + 0x3a0, 1000);
        ST_U8(e + 0x3a4, 2);
        ST_U8(e + 0x3b0, 24);
        ST_U8(e + 0x3b1, 1);
        ST_U8(e + 0x3b4, 6);
        ST_U8(e + 0x3b5, 3);
        ST_U8(e + 0x3b2, 51);
        ST_U8(e + 0x3b3, 10);
        ST_U8(e + 0x3b6, 0);
        break;
    case 2:
        ST_U32(e + 0x39c, 6);
        ST_U32(e + 0x3a0, 1000);
        ST_U8(e + 0x3b0, 80);
        ST_U8(e + 0x3b1, 24);
        ST_U8(e + 0x3b2, 1);
        ST_U8(e + 0x3b3, 6);
        ST_U8(e + 0x3b4, 3);
        ST_U8(e + 0x3b5, 0);
        ST_U8(e + 0x3b6, 45);
        ST_U8(e + 0x3b7, 20);
        break;
    case 3:
    case 5:
        ST_U32(e + 0x39c, 6);
        ST_U32(e + 0x3a0, 1000);
        ST_U8(e + 0x3b0, 80);
        ST_U8(e + 0x3b1, 24);
        ST_U8(e + 0x3b2, 1);
        ST_U8(e + 0x3b3, 6);
        ST_U8(e + 0x3b4, 3);
        ST_U8(e + 0x3b5, 0);
        ST_U8(e + 0x3b6, 50);
        ST_U8(e + 0x3b7, 35);
        ST_U8(e + 0x3b8, 45);
        ST_U8(e + 0x3b9, 20);
        ST_U8(e + 0x3ba, 0);
        ST_U8(e + 0x3bb, 2);
        ST_U8(e + 0x3bc, 0);
        ST_U8(e + 0x3bd, 5);
        ST_U8(e + 0x3be, 0);
        ST_U8(e + 0x3bf, 20);
        break;
    case 4:
        ST_U32(e + 0x39c, 6);
        ST_U32(e + 0x3a0, 1500);
        ST_U32(e + 0x3a4, 1);
        ST_U32(e + 0x3a8, 1000);
        ST_U32(e + 0x3ac, 500);
        ST_U8(e + 0x3b0, 80);
        ST_U8(e + 0x3b1, 24);
        ST_U8(e + 0x3b2, 1);
        ST_U8(e + 0x3b3, 6);
        ST_U8(e + 0x3b4, 3);
        ST_U8(e + 0x3b5, 0);
        ST_U16(e + 0x3b6, 5);
        ST_U16(e + 0x3b8, 60);
        ST_U8(e + 0x3ba, 45);
        ST_U8(e + 0x3bb, 20);
        break;
    }
    ST_U8(e + 0x399, 1);
    ST_U8(e + 0x39a, 2);
    ST_U8(e + 0x40c, 0);
    ST_U8(e + 0x40d, 1);
    ST_U32(e + 0x410, 0x151800);
    ST_U32(e + 0x414, 2);
    ST_U8(e + 0x418, 2);
    ST_U32(e + 0x41c, 0x10e000);
    ST_U32(e + 0x420, 0x87000);
    ST_U32(e + 0x424, 0x87000);
}

/* ---------------------------------------------------------- picture end */

/* P class windows after an I / skipped picture: clear the class budgets and
 * sizes (st+0x118..0x190), positions at 1/5, 2/5, 3/5, 4/5 of the GOP
 * (inlined in the H.265 FRAME_END; update_pwin in the H.264 one) */
static void t65_update_pwin(E33 *X, uint32_t cfg, uint32_t st)
{
    int32_t n = LD_S32(cfg + 0xc), a = n / 5, b, c;
    uint32_t o;

    for (o = 0x118; o < 0x194; o += 4)
        ST_U32(st + o, 0);
    b = n - a;
    if (b < 0)
        b += 3;
    b = (b >> 2) + a;
    c = b + (n - b) / 3;
    ST_S32(st + 0x194, a);
    ST_S32(st + 0x198, b);
    ST_S32(st + 0x19c, c);
    ST_S32(st + 0x1a0, c + (n - c) / 2);
}

/* SMART: the QP of a picture coded with the motion ROI (inst+0x7f > 0, no
 * motion rule, cur+0x2ac == 0): kind 7 adds the mean ROI offset @roi, kind 6
 * on a CTU configuration (cfg+8 == 1) the part of cur+0x1d above st+0xac */
static uint32_t t65_roi_qp(E33 *X, uint32_t inst, uint32_t cfg, uint32_t st, uint32_t cur, int32_t roi)
{
    uint32_t qp = LD_U8(cur + 0x1c);

    if (LD_S8(inst + 0x7f) < 1 || LD_S8(inst + 0x7d) != 0 || LD_S8(cur + 0x2ac) != 0)
        return qp;
    if (LD_S8(cur) == 7)
        return qp + roi;
    if (LD_S8(cur) == 6 && LD_S8(cfg + 8) == 1 && LD_S8(st + 0xac) < (int32_t)LD_U8(cur + 0x1d))
        return qp + (LD_U8(cur + 0x1d) - LD_S8(st + 0xac));
    return qp;
}

/* P-share bookkeeping after an I/P picture: per-picture P size beyond the I */
static void t65_p_avg(E33 *X, uint32_t cfg, uint32_t st, uint32_t bits)
{
    int32_t g = LD_S32(cfg + 0xc), d;

    if (g == 1)
        return;
    d = (int32_t)(bits - LD_U32(st + 0x50));
    ST_S32(st + 0x108, d);
    ST_S32(st + 0x104, d / (g - 1));
}

/* shift a byte history of @n entries at @a up by one, newest @v at @a */
static void t65_hist8(E33 *X, uint32_t a, int n, uint8_t v)
{
    for (int i = n - 1; i > 0; i--)
        ST_U8(a + i, LD_U8(a + i - 1));
    ST_U8(a, v);
}

/* JZ_VPU_RC_FRAME_END_T33 (0x1b0a2c): account the coded picture.  The H.264
 * code (0x19c520) with the H.265 block layout (rate block at e+0x390, the
 * per-picture data at cur+0x1c..0x1f / +0x88 / +0x8c, P = slice type 1) */
static void e65_frame_end(E33 *X, uint32_t e)
{
    uint32_t st, inst, ps, cur, prev, cfg, pos, fps, bits, units;
    int32_t n, roi, mode;
    int8_t kind;
    int64_t qb;

    ST_U8(e + 0x50, LD_U8(e + 0x380));
    ST_U8(e + 0x53, LD_U8(e + 0x381));
    if (LD_S8(e + 0x30) == 0)
        e65_frame_feature(X, e);
    st = LD_U32(e + 0x3d8);
    inst = LD_U32(e + 0x3d4);
    ps = LD_U32(e + 0x3e0);
    cur = LD_U32(ps + 0x1c);
    cfg = LD_U32(e + 0x390);
    n = LD_S32(st + 0x14) != 0xfffffff ? LD_S32(st + 0x14) + 1 : 10;
    ST_S32(st + 0x14, n);
    ST_U32(st + 0x60, LD_U8(cur + 0x1c));
    /* ROI QP offset per coding unit (32x32 CTU * 4, else 16x16) */
    if (LD_S8(cfg + 8) == 1)
        units = ((LD_U16(cfg) + 0x1f) >> 5) * ((LD_U16(cfg + 2) + 0x1f) >> 5) * 4;
    else if (LD_S8(cfg + 8) == 0)
        units = ((LD_U16(cfg) + 0xf) >> 4) * ((LD_U16(cfg + 2) + 0xf) >> 4);
    else
        units = 1;
    if (units == 0)
        return;                 /* the vendor traps */
    roi = (int8_t)(LD_S32(cur + 0x8c) / (int32_t)units);
    ST_S8(st + 0xad, roi);
    ST_U32(st + 0x60, t65_roi_qp(X, inst, cfg, st, cur, roi));
    if (LD_S8(st + 0x7b) == 6) {
        ST_U8(st + 0x30, 6);
        kind = 6;
    } else {
        kind = LD_S8(cur + 0x1f);
        ST_S8(st + 0x30, kind);
    }
    mode = LD_S8(e + 0x394);
    if (mode == 0) {
        pos = LD_U32(st + 0x398) + 1;
        ST_U32(st + 0x398, pos == LD_U32(cfg + 0xc) ? 0 : pos);
        return;
    }
    fps = LD_U32(st + 0x358);
    if (fps == 0)
        return;                 /* the vendor traps */
    pos = LD_U32(st + 0x398) + 1;
    ST_U32(st + 0x398, pos);
    if (LD_S32(st + 0x28) == (6 / (int32_t)fps) * LD_S32(cfg + 0xc)) {
        ST_U32(st + 0x28, 0);
        ST_U64(st + 0x20, 0);
        ST_U8(st + 0x1c, 0);
    }
    prev = LD_U32(ps + 0x20);
    if (n != 1)
        ST_S32(st + 0x3a0, LD_S32(st + 0x3a0) + LD_S8(prev + 0x1c));
    if (mode == 3)
        ST_U64(st + 0x388, LD_U64(st + 0x388) + LD_U32(cur + 0x18));
    /* complexity per kbit/s: (cur+0x88 << 11) / (bit rate >> 10) */
    qb = t65_div64((int64_t)((uint64_t)LD_U32(cur + 0x88) << 11), LD_S32(inst + 8) >> 10);
    ST_S64(st + 0xa0, qb);
    ST_U8(st + 0x7a, LD_U8(st + 0xab));
    ST_U8(st + 0xab, 0);
    bits = LD_U32(cur + 0x18);

    if (kind == 2) {                    /* I picture */
        uint64_t r;

        ST_U32(st + 0x58, t65_roi_qp(X, inst, cfg, st, cur, roi));
        if (n == 1)
            ST_U64(st + 0x3b0, LD_U64(st + 0x3b0) + bits);
        else
            t65_p_avg(X, cfg, st, bits);
        ST_U64(st + 0x38, bits);
        ST_U64(st + 0x50, bits);
        ST_U64(st + 0x380, LD_U64(st + 0x380) + bits);
        ST_S64(st + 0x70, qb);
        if (n != 0) {
            ST_U8(st + 0x1c, LD_U8(prev + 0x1c));
            ST_U32(st + 0x28, 1);
            ST_S64(st + 0x20, LD_S8(prev + 0x1c));
        }
        ST_U8(st + 0x95, 1);
        r = LD_U64(st + 0x360);
        if (r == 0)
            return;             /* the vendor traps */
        /* I/P size ratio in percent of the mean P picture */
        ST_S32(st + 0x68, (int32_t)t65_div64((int64_t)((uint64_t)bits * 100), (int64_t)(int32_t)fps * (int64_t)r));
        ST_U32(st + 0x2c, 0);
        t65_update_pwin(X, cfg, st);
        for (uint32_t o = 0x218; o > 0x1d0; o -= 8)     /* I size history */
            ST_U64(st + o, LD_U64(st + o - 8));
        ST_U8(st + 0x78, LD_U8(st + 0xa8));
        ST_U32(st + 0x10c, 0);
        ST_U64(st + 0x1d0, bits);
        t65_hist8(X, st + 0x22a, 10, (uint8_t)LD_U32(st + 0x58));
    } else if (kind == 6) {             /* skipped picture */
        int64_t m = LD_S64(st + 0x38);
        int8_t q;

        t65_p_avg(X, cfg, st, bits);
        if (m < (int64_t)bits)
            m = bits;
        ST_U64(st + 0x380, LD_U64(st + 0x380) + (uint64_t)m);
        ST_U64(st + 0x50, bits);
        q = LD_S8(prev + 0x1c);
        if (LD_S8(st + 0x1c) < q)
            ST_S8(st + 0x1c, q);
        ST_S64(st + 0x20, LD_S64(st + 0x20) + q);
        ST_S32(st + 0x28, LD_S32(st + 0x28) + 1);
        ST_U32(st + 0x2c, 0);
        t65_update_pwin(X, cfg, st);
        ST_U8(st + 0x78, LD_U8(st + 0xa8));
    } else if (kind == 1) {             /* P picture */
        uint32_t qp = t65_roi_qp(X, inst, cfg, st, cur, roi);
        int32_t k;
        int8_t q;
        int b;

        ST_U32(st + 0x5c, qp);
        ST_U32(st + 0xe0, LD_U32(st + 0xe0) + qp);
        ST_U64(st + 0x48, bits);
        ST_U64(st + 0x380, LD_U64(st + 0x380) + bits);
        q = LD_S8(prev + 0x1c);
        if (LD_S8(st + 0x1c) < q)
            ST_S8(st + 0x1c, q);
        ST_S64(st + 0x20, LD_S64(st + 0x20) + q);
        /* P size by position window within the GOP (0x190..0x1a0) */
        k = LD_S32(st + 0x2c);
        for (b = 4; b >= 0 && k < LD_S32(st + 0x190 + b * 4); b--)
            ;
        if (b >= 0)
            ST_U64(st + 0x168 + b * 8, LD_U64(st + 0x168 + b * 8) + bits);
        ST_U8(st + 0x78, LD_U8(st + 0xa8));
        ST_S32(st + 0x28, LD_S32(st + 0x28) + 1);
        ST_S32(st + 0x2c, k + 1);
        if (pos == 1) {
            t65_p_avg(X, cfg, st, bits);
            ST_U64(st + 0x50, bits);
            ST_U32(st + 0x2c, 0);
            t65_update_pwin(X, cfg, st);
        } else {
            ST_S64(st + 0x3b0, LD_S64(st + 0x3b0) + bits + (int64_t)LD_S32(st + 0x104) - LD_U32(st + 0x100));
        }
        for (uint32_t o = 0x1cc; o > 0x1a8; o -= 4)     /* P size history */
            ST_U32(st + o, LD_U32(st + o - 4));
        ST_U32(st + 0x1a8, bits);
        t65_hist8(X, st + 0x220, 10, (uint8_t)qp);
    }
    t65_hist8(X, st + 0x234, 10, LD_U8(prev + 0x1c));
    t65_hist8(X, st + 0x23e, 10, LD_U8(prev + 0x1d));

    /* VBV fullness, clamped to [0, size] unless inst+0x38 */
    {
        int64_t v = LD_S64(st + 0x428) + bits - LD_S64(st + 0x438);

        ST_S64(st + 0x428, v);
        if (LD_S32(inst + 0x38) == 0) {
            if (v < 0)
                ST_U64(st + 0x428, 0);
            else if (v > LD_S64(st + 0x420))
                ST_U64(st + 0x428, LD_U64(st + 0x420));
        }
    }
    if (mode == 4)                      /* AVBR window budget */
        ST_S64(st + 0x288, LD_S64(st + 0x288) + bits -
               t65_div64(LD_S64(st + 0x270) * (int64_t)(int32_t)fps, (int64_t)LD_U32(cfg + 0xc)));
    if (pos == LD_U32(cfg + 0xc)) {     /* end of the GOP */
        int32_t f = LD_S32(st + 0x358);
        int64_t avg;

        ST_U32(st + 0x398, 0);
        ST_S32(st + 0x338, LD_S32(st + 0x338) + f);
        avg = t65_div64(LD_S64(st + 0x380), f);
        ST_S64(st + 0x368, avg);
        if (pos == 1) {
            ST_U8(st + 0xdc, 0);
            ST_U32(st + 0x100, 0);
        } else {
            uint32_t lo = LD_U32(st + 0x3b0), d = lo - LD_U32(st + 0x50), g1, q, t;

            ST_S8(st + 0xdc, (int8_t)((LD_U32(st + 0xe0) - 2 + pos) / (pos - 1)));
            g1 = LD_U32(cfg + 0xc) - 1;
            if (g1 == 0)
                return;         /* the vendor traps */
            q = d / g1;
            ST_U32(st + 0x100, q);
            t = LD_U32(st + 0x108) - g1 * LD_U32(st + 0x104);
            ST_S64(st + 0x3b0, LD_S64(st + 0x3b0) - (int64_t)(uint64_t)(d - g1 * q) + (int64_t)(int32_t)t);
        }
        ST_S64(st + 0xf8, avg);
        if (mode == 4)
            ST_S32(st + 0x2b8, LD_S32(st + 0x2b8) + f);
        {
            uint32_t g = LD_U32(cfg + 0xc);

            t65_hist8(X, st + 0x248, 10,
                      (uint8_t)t65_div64(LD_S64(st + 0x20) + (int64_t)g - 1, (int64_t)g));
        }
        e33_fill(X, st + 0x1a8, 0, 0x28);
        e33_fill(X, st + 0x220, 0, 10);
        pos = 0;
    }
    if (pos == 0) {                     /* bit-rate statistics window */
        int64_t a = LD_S64(st + 0x380), b = a;
        int32_t c;

        if (mode == 3)
            a = LD_S64(st + 0x388);
        ST_S64(st + 0x330, LD_S64(st + 0x330) + a);
        ST_S64(st + 0x318, LD_S64(st + 0x318) - b);
        ST_S64(st + 0x328, LD_S64(st + 0x328) - b);
        c = LD_S32(st + 0x338);
        if (c == LD_S32(st + 0x2d8)) {
            ST_U32(st + 0x338, 0);
            ST_S64(st + 0x2e8, t65_div64(LD_S64(st + 0x330), c));
            ST_U32(st + 0x344, LD_U32(inst + 0x40));
        } else {
            ST_S32(st + 0x344, LD_S32(st + 0x344) - 1);
            ST_S64(st + 0x320, t65_div64(LD_S64(st + 0x318), LD_S32(st + 0x2d8) - c));
        }
    }
    if (mode == 4 && LD_S32(st + 0x338) == 0) {
        int64_t s = LD_S64(st + 0x330);
        int32_t c = LD_S32(st + 0x2b8);

        ST_S64(st + 0x2a8, LD_S64(st + 0x2a8) - s);
        ST_S64(st + 0x2b0, LD_S64(st + 0x2b0) + s);
        if (c == LD_S32(st + 0x258)) {
            ST_U64(st + 0x288, 0);
            ST_U32(st + 0x2b8, 0);
            ST_S64(st + 0x268, t65_div64(LD_S64(st + 0x2b0), c));
        } else {
            ST_S64(st + 0x280, t65_div64(LD_S64(st + 0x2a8), LD_S32(st + 0x258) - c));
        }
    }
}
static void t65_clip_s8(E33 *X, uint32_t a, int8_t lo, int8_t hi)
{
    int8_t v = LD_S8(a);

    if (v < lo)
        ST_S8(a, lo);
    else if (hi < v)
        ST_S8(a, hi);
}

static void t65_clip_s32(E33 *X, uint32_t a, int32_t lo, int32_t hi)
{
    int32_t v = LD_S32(a);

    if (v < lo)
        ST_S32(a, lo);
    else if (hi < v)
        ST_S32(a, hi);
}

static void t65_clip_u32(E33 *X, uint32_t a, uint32_t lo, uint32_t hi)
{
    uint32_t v = LD_U32(a);

    if (v < lo)
        ST_U32(a, lo);
    else if (hi < v)
        ST_U32(a, hi);
}

/* statistics window in seconds: max(GOP duration, e+0x82c), 1..60 */
static void t65_cfg_window(E33 *X, uint32_t e, uint32_t cfg)
{
    uint32_t t = (uint32_t)(LD_S32(cfg + 0xc) * LD_S32(cfg + 0x10)) / LD_U32(cfg + 0x14);

    if (t < LD_U32(e + 0x39c))
        t = LD_U32(e + 0x39c);
    ST_U32(e + 0x39c, t == 0 ? 1 : t > 60 ? 60 : t);
    t65_clip_u32(X, e + 0x3a0, 2, 0x19000);       /* bit rate, kbit/s */
}

/* clamp the user parameters of the mode (0x1ea0b0 jump table) */
static int t65_cfg_clamp(E33 *X, uint32_t e, uint32_t mode)
{
    uint32_t cfg = LD_U32(e + 0x390);
    uint8_t lo, hi;

    if (mode == 0) {                    /* fixed QP: I/P/B QP */
        t65_clip_u8(X, e + 0x39c, 0, 51);
        t65_clip_u8(X, e + 0x39d, 0, 51);
        t65_clip_u8(X, e + 0x39e, 0, 51);
        return 0;
    }
    if (LD_U32(cfg + 0x14) == 0)
        return -1;                      /* the vendor traps */
    t65_cfg_window(X, e, cfg);
    lo = LD_U8(cfg + 0x19);
    hi = LD_U8(cfg + 0x18);
    if (mode == 1) {                    /* CBR */
        t65_clip_u8(X, e + 0x3a4, 0, 8);
        t65_clip_u8(X, e + 0x3b5, 0, 3);
        t65_clip_u8(X, e + 0x3b4, 0, 7);
        t65_clip_s8(X, e + 0x3b6, -10, 10);
        t65_clip_u8(X, e + 0x3b0, 1, 100);
        if (LD_U8(e + 0x3b0) < LD_U8(e + 0x3b1))
            ST_U8(e + 0x3b1, LD_U8(e + 0x3b0));
        t65_clip_u8(X, e + 0x3b3, lo, hi);
        t65_clip_u8(X, e + 0x3b2, lo, hi);
        return 0;
    }
    t65_clip_u8(X, e + 0x3b0, 50, 100);
    t65_clip_u8(X, e + 0x3b3, 0, 7);
    t65_clip_u8(X, e + 0x3b4, 0, 3);
    t65_clip_s8(X, e + 0x3b5, -10, 10);
    if (mode == 4) {                    /* AVBR */
        t65_clip_u32(X, e + 0x3a4, 1, 0x5a0);
        t65_clip_u32(X, e + 0x3a8, 2, 0x19000);
        t65_clip_u32(X, e + 0x3ac, 2, 0x19000);
        if (LD_U16(e + 0x3b6) > 1000)
            ST_U16(e + 0x3b6, 1000);
    } else if (mode != 2) {             /* SMART */
        t65_clip_u8(X, e + 0x3b6, 1, 100);
    }
    t65_clip_u8(X, e + 0x3b1, 1, 100);
    if (LD_U8(e + 0x3b1) < LD_U8(e + 0x3b2))
        ST_U8(e + 0x3b2, LD_U8(e + 0x3b1));
    if (mode == 2) {                    /* VBR */
        t65_clip_u8(X, e + 0x3b7, lo, hi);
        t65_clip_u8(X, e + 0x3b6, lo, hi);
    } else if (mode == 4) {
        if (LD_U16(e + 0x3b8) == 0)
            ST_U16(e + 0x3b8, 1);
        else if (LD_U16(e + 0x3b8) > 0x708)
            ST_U16(e + 0x3b8, 0x708);
        t65_clip_u8(X, e + 0x3bb, lo, hi);
        t65_clip_u8(X, e + 0x3ba, lo, hi);
    } else {
        t65_clip_u8(X, e + 0x3b7, lo, hi);
        t65_clip_u8(X, e + 0x3b9, lo, hi);
        t65_clip_u8(X, e + 0x3b8, lo, hi);
        t65_clip_u8(X, e + 0x3ba, 0, 2);
        t65_clip_u8(X, e + 0x3bb, 0, 3);
        t65_clip_u8(X, e + 0x3bc, 0, 1);
        t65_clip_u8(X, e + 0x3bd, 0, 10);
        t65_clip_u8(X, e + 0x3be, 0, 3);
        t65_clip_u8(X, e + 0x3bf, 1, 100);
    }
    return 0;
}

/* bit-rate class by picture size: low / high edge (0x1ea58c) */
static const int32_t t65_size_rate[3][2] = { { 400000, 1000000 }, { 800000, 4000000 }, { 2000000, 2000000 } };

/* JZ_VPU_RC_VIDEO_CFG_T33 (0x1ad514): state blocks, parameter clamps,
 * per-mode instance setup.  The H.264 code (0x1990f0) with the H.265 block
 * layout (e+0x820.. at e+0x390.., e+0x88c.. at e+0x400..), the
 * HW-revision dependent defaults of st+0x78 / +0xa8 and the final range
 * checks of the instance knobs */
static void e65_video_cfg(E33 *X, uint32_t e)
{
    uint32_t S = LD_U32(e), u, cfg, mode, fn, fd, gs, p34 = e + 0x34;
    int32_t px, n40 = 1;
    uint8_t ext, i_qp;

    u = (((LD_U16(e + 4) + 0x1f) >> 5) * ((LD_U16(e + 6) + 0x1f) >> 5) * 4 + 0xfu) & ~0xfu;
    if (S == 0) {
        S = e33_calloc(X, u * 2 + 0x7b0, 1);
        ST_U32(e, S);
        if (S == 0)
            return;
    }
    ST_U32(e + 0x3d4, S);
    ST_U32(e + 0x3d8, S + 0x90);
    ST_U32(e + 0x3dc, S + 0x500);
    ST_U32(e + 0x3e0, S + 0x540);
    ST_U32(e + 0x3e8, S + 0x570);
    ST_U32(e + 0x3ec, S + 0x640);
    ST_U32(S + 0x570, S + 0x660);
    ST_U32(S + 0x574, S + 0x680);
    ST_U32(S + 0x650, S + 0x6a0);
    ST_U32(S + 0x5ac, S + 0x7b0);
    ST_U32(S + 0x5b4, S + u + 0x7b0);
    ST_U32(e + 0x400, S + 0x6c0);
    ST_U32(e + 0x404, S + 0x7a0);
    ST_U32(e + 0x428, S + 0x770);
    ST_U32(e + 0x42c, S + 0x790);
    ST_U32(S + 0x560, S + 0x570);
    ST_U32(S + 0x564, S + 0x7a0);
    ST_U32(S + 0x55c, p34);
    ST_U32(S + 0x640, p34);
    ST_U32(S + 0x644, S + 0x500);
    ST_U32(S + 0x648, S + 0x7a0);
    ST_U32(S + 0x6c4, p34);
    ST_U32(S + 0x6c8, S + 0x500);
    ST_U32(S + 0x6cc, S + 0x570);
    ST_U32(S + 0x77c, p34);
    ST_U32(S + 0x780, S + 0x500);
    ST_U32(S + 0x784, S + 0x570);
    ST_U32(S + 0x788, S + 0x7a0);

    /* stream parameters */
    t65_clip_u32(X, e + 0x10, 1, 0xffff);
    t65_clip_u32(X, e + 0x18, 1, 0xf0);
    t65_clip_u32(X, e + 0x14, 1, LD_U32(e + 0x18));
    t65_clip_u8(X, e + 0x1c, 0, 51);
    if (LD_U8(e + 0x1c) < LD_U8(e + 0x1d))
        ST_U8(e + 0x1d, LD_U8(e + 0x1c));
    t65_clip_u8(X, e + 0xc, 0, 1);
    t65_clip_u8(X, e + 0x1e, 0, 1);
    t65_clip_u8(X, e + 0x396, 0, 6);
    t65_clip_u8(X, e + 0x394, 0, 5);
    t65_clip_u8(X, e + 0x397, 0, 1);
    t65_clip_u8(X, e + 0x398, 0, 1);
    t65_clip_s8(X, e + 0x395, -1, 51);
    if (LD_U32(e + 0x3cc) > 0xc350000)             /* fixed picture sizes: unsigned compares */
        ST_S32(e + 0x3cc, 0xc350000);
    if (LD_U32(e + 0x3d0) > 0xc350000)
        ST_S32(e + 0x3d0, 0xc350000);
    ext = LD_U8(e + 0x397);
    if (ext == 1) {
        if (LD_S32(e + 0x3c0) == 0)
            ST_S32(e + 0x3c0, 1);
        else if (LD_U32(e + 0x3c0) > 0x3c)         /* unsigned (0x1ac.., sltiu) */
            ST_S32(e + 0x3c0, 0x3c);
        t65_clip_s8(X, e + 0x3c4, 0, 30);
        t65_clip_s8(X, e + 0x3c5, 0, 30);
        t65_clip_s8(X, e + 0x3c6, 0, 30);
    } else {
        t65_clip_s8(X, e + 0x3c0, 0, 30);
        t65_clip_s8(X, e + 0x3c1, 0, 30);
    }
    mode = LD_U8(e + 0x394);
    if (t65_cfg_clamp(X, e, mode))
        return;
    t65_clip_u8(X, e + 0x399, 0, 3);
    {
        uint32_t g = LD_U32(e + 0x408);
        uint32_t m = (uint32_t)(((int32_t)(LD_U16(g) * LD_U16(g + 2) * 3) >> 1) << 3);

        if (m < LD_U32(e + 0x41c))
            ST_U32(e + 0x41c, m);
        if (m < LD_U32(e + 0x420))
            ST_U32(e + 0x420, m);
    }
    t65_clip_u8(X, e + 0x40d, 0, 2);
    if (LD_U32(e + 0x410) < 64000)
        ST_U32(e + 0x410, 64000);
    if (LD_U32(e + 0x414) > 0xffff)
        ST_U32(e + 0x414, 0xffff);
    if (ext == 1) {
        ST_U8(S + 0x558, LD_U8(e + 0x3c4));
        ST_U8(S + 0x55a, LD_U8(e + 0x3c5));
        ST_U8(S + 0x559, LD_U8(e + 0x3c6));
    } else {
        ST_U8(S + 0x558, LD_U8(e + 0x3c0));
        ST_U8(S + 0x559, LD_U8(e + 0x3c1));
    }

    /* mode bit rates (bit/s) and QP limits into the instance block */
    switch (mode) {                     /* 0x1ea0c8 */
    case 0:
        ST_U8(S + 0x541, LD_U8(e + 0x39c));
        ST_U8(S + 0x540, LD_U8(e + 0x39c));
        break;
    case 1:
        ST_U64(S + 0x548, (uint32_t)(LD_S32(e + 0x3a0) << 10));
        ST_U8(S + 0x541, LD_U8(e + 0x3b3));
        ST_U8(S + 0x540, LD_U8(e + 0x3b2));
        break;
    case 4:
        ST_U64(S + 0x550, (uint32_t)(LD_S32(e + 0x3a0) << 10));
        ST_U64(S + 0x548, (uint32_t)(((LD_S32(e + 0x3a8) + LD_S32(e + 0x3ac)) >> 1) << 10));
        ST_U8(S + 0x541, LD_U8(e + 0x3bb));
        ST_U8(S + 0x540, LD_U8(e + 0x3ba));
        break;
    default:                            /* VBR, SMART: max and mean */
        ST_U64(S + 0x550, (uint32_t)(LD_S32(e + 0x3a0) << 10));
        ST_S64(S + 0x548, (int64_t)(((uint64_t)LD_U32(e + 0x3a0) << 10) * LD_U8(e + 0x3b0)) / 100);
        ST_U8(S + 0x541, LD_U8(e + (mode == 2 ? 0x3b7 : 0x3b9)));
        ST_U8(S + 0x540, LD_U8(e + (mode == 2 ? 0x3b6 : 0x3b8)));
        break;
    }
    e33_fill(X, S + 0x90, 0, 0x468);
    if (LD_S8(LD_U32(S + 0x55c)) == 6) {      /* HW revision */
        ST_U8(S + 0x108, 4);
        ST_U8(S + 0x138, 4);
    } else if (LD_S8(LD_U32(S + 0x55c)) == 7) {
        ST_U8(S + 0x108, 6);
        ST_U8(S + 0x138, 6);
    }
    ST_U8(S + 0x7d, 0);
    ST_U8(S + 0x7f, 0);
    cfg = LD_U32(e + 0x390);
    px = LD_U16(cfg) * LD_U16(cfg + 2);
    ST_S32(S + 8, px);
    if (mode != 0) {
        int cls = px < 0xe1001 ? px >= 0x38400 : 2;
        int64_t br = LD_S64(S + 0x548);
        uint32_t units = 0;

        fn = LD_U32(cfg + 0x10);
        fd = LD_U32(cfg + 0x14);
        if (fn * 30 == 0)
            return;                     /* the vendor asserts */
        /* rate class: 2 above the high edge, 0 below the low edge */
        if (br > (int64_t)(int32_t)(fd * (uint32_t)t65_size_rate[cls][1] / (fn * 30)))
            ST_U32(S + 4, 2);
        else
            ST_U32(S + 4, br < (int64_t)(int32_t)((uint32_t)t65_size_rate[cls][0] * fd / (fn * 30)) ? 0 : 1);
        if (LD_S8(cfg + 8) == 1)
            units = (uint32_t)((((px + 0x400) >> 10) * 3) >> 2);
        else if (LD_S8(cfg + 8) == 0)
            units = (uint32_t)((((px + 0xff) >> 8) * 3) >> 2);
        ST_U64(S + 0x20, units);
        ST_U64(S + 0x98, LD_U32(e + 0x3cc));
        ST_U32(S + 0x28, 3);
        ST_U8(S + 0x80, 0);
        ST_U8(S + 0x54, 0);
        ST_U32(S + 0x40, 1);
        ST_U64(S + 0x440, 0);
        ST_U64(S + 0x190, 0);
        if (fd == 0)
            return;                     /* the vendor traps */
        gs = (fn * LD_U32(cfg + 0xc) + (fd - 1)) / fd;  /* GOP length in s */
        ST_U32(S + 0x3e8, gs);
        ST_S32(S + 0x368, LD_S32(e + 0x39c));
        ST_U8(S + 0x4c, 0);
        ST_U8(S + 0x4d, 2);
        ST_U32(S + 0x50, 2);
        ST_U8(S + 0x55, 1);
        ST_U32(S + 0x58, 0x1f);
        ST_U8(S + 0x7c, 0);
        ST_U32(S + 0x38, 0);
        switch (mode) {                 /* 0x1ea0e0 */
        case 1:
            if (gs == 0)
                return;                 /* the vendor traps */
            n40 = LD_S32(e + 0x39c) / (int32_t)gs;
            if (n40 == 0)
                n40 = 1;
            ST_S32(S + 0x40, n40);
            ST_U32(S + 0x38, 1);
            ST_U32(S + 0x10, LD_U8(e + 0x3b0));
            ST_U32(S + 0x14, LD_U8(e + 0x3b1));
            i_qp = LD_U8(e + 0x3b4);
            ST_S64(S + 0x90, LD_S8(e + 0x3b6));
            break;
        case 2:
            ST_U32(S + 0x44, 3);
            if (LD_S32(S + 0x48) < 1) {
                int64_t mx = LD_S64(S + 0x550);

                ST_U32(S + 0x48, mx > 2000000 ? 20 : mx > 1000000 ? 15 : mx > 500000 ? 10 : 5);
            } else if (LD_S32(S + 0x48) > 100) {
                ST_U32(S + 0x48, 100);
            }
            ST_U32(S + 0x10, LD_U8(e + 0x3b1));
            ST_U32(S + 0x14, LD_U8(e + 0x3b2));
            i_qp = LD_U8(e + 0x3b3);
            ST_S64(S + 0x90, LD_S8(e + 0x3b5));
            break;
        case 4: {
            uint32_t f = LD_U32(e + 0x3a4), w = LD_U16(e + 0x3b8), lo_br = LD_U32(e + 0x3a8) * 0x400;
            uint32_t v = LD_U16(e + 0x3b6) * w * f;

            ST_U32(S + 0x2e8, w * f);
            ST_U64(S + 0x300, lo_br);
            ST_U64(S + 0x308, (uint32_t)(LD_S32(e + 0x3ac) << 10));
            ST_S64(S + 0x60, (int64_t)(int32_t)v * (int64_t)lo_br / 1000);
            ST_U32(S + 0x10, LD_U8(e + 0x3b1));
            ST_U32(S + 0x14, LD_U8(e + 0x3b2));
            ST_U32(S + 0x50, 1);
            i_qp = LD_U8(e + 0x3b3);
            ST_S64(S + 0x90, LD_S8(e + 0x3b5));
            break;
        }
        default: {                      /* SMART (3, 5) */
            int64_t mean = LD_S64(S + 0x550) * LD_U8(e + 0x3b0) / 100;

            ST_S64(S + 0x68, mean * LD_U8(e + 0x3b6) / 100);
            ST_U32(S + 0x78, LD_S8(S + 0x540) <= LD_U8(e + 0x3b7) ? LD_S8(S + 0x540) : LD_U8(e + 0x3b7));
            ST_U32(S + 0x10, LD_U8(e + 0x3b1));
            ST_U32(S + 0x14, LD_U8(e + 0x3b2));
            i_qp = LD_U8(e + 0x3b3);
            ST_U8(S + 0x7d, LD_U8(e + 0x3ba));
            ST_U8(S + 0x7e, LD_U8(e + 0x3bb));
            if (mode == 3) {
                uint32_t c = LD_U32(e + 0x390);

                if (LD_U32(c + 0x10) == 0)
                    return;             /* the vendor traps */
                ST_U32(S + 0x3c, (uint32_t)(LD_S32(e + 0x3c0) * LD_S32(c + 0x14)) / LD_U32(c + 0x10));
                ST_U8(S + 0x7c, LD_U8(e + 0x3bc));
                if (LD_S8(e + 0x3bc) != 0) {
                    ST_S64(S + 0x70, mean * LD_U8(e + 0x3bf) / 100);
                    ST_U8(S + 0x80, LD_U8(e + 0x3be));
                    ST_U8(S + 0x81, LD_U8(e + 0x3bd));
                }
            }
            ST_S64(S + 0x90, LD_S8(e + 0x3b5));
            break;
        }
        }
        ST_U8(S + 0x18, i_qp);
        ST_U8(S + 0x19, i_qp);
        ST_S32(S + 0x3d4, n40);
        e33_fill(X, S + 0x238, 0, 0x8c);
        e33_fill(X, S + 0x434, 0, 8);
        e33_fill(X, S + 0x3d8, 0, 8);
        e33_fill(X, S + 0x360, 0, 8);
        e33_fill(X, S + 0x2c4, 0xff, 0x1e);
        if (n40 < 0)
            ST_U32(S + 0x40, 0);
        else if (n40 > 60)
            ST_U32(S + 0x40, 60);
        /* range checks of the instance knobs (no-ops for the values above) */
        t65_clip_s32(X, S + 0x38, 0, 1);
        t65_clip_s8(X, S + 0x4c, 0, 1);
        t65_clip_s8(X, S + 0x4d, 0, 2);
        t65_clip_s32(X, S + 0x50, 1, 3);
        t65_clip_s8(X, S + 0x55, 0, 1);
        t65_clip_s32(X, S + 0x58, 5, 35);
        t65_clip_s8(X, S + 0x7c, 0, 1);
    }

    /* picture defaults, slice block, mode word of the HW block */
    {
        uint32_t p = LD_U32(e + 0x3e8), q = LD_U32(e + 0x400), r = LD_U32(e + 0x428);

        ST_U32(LD_U32(e + 0x3ec) + 0xc, LD_U32(e + 0x2ec));
        ST_U8(p + 9, 7);
        ST_U8(p + 8, 7);
        for (int i = 0; i < 0x20; i++)
            ST_U8(LD_U32(p + 4) + i, 7);
        ST_U8(q, 1);
        ST_U8(q + 1, 0);
        ST_U8(q + 2, 1);
        ST_U8(q + 3, 1);
        ST_U8(q + 0xa4, 1);
        ST_U32(q + 0x10, LD_U32(e + 0x2ec));
        if (mode == 1)
            ST_U32(r + 8, LD_U8(e + 0x3b5));
        else if (mode != 0)
            ST_U32(r + 8, LD_U8(e + 0x3b4));
    }
}



/* ---------------------------------------------------------- slice block */

/* basic-unit CTU rows by slice-level class (0x1ea208) */
static const uint8_t t65_bu_rows[9] = { 1, 2, 3, 4, 5, 6, 10, 20, 32 };

/* per-slice bit limits by level, three columns (0x1ea214: rev 6, x/1600 of
 * the slice target; 0x1ea2a4: rev 7, x/3200); both rows of each table are
 * the same (the H.264 tables) */
static const uint16_t t65_bu_lim6[12][3] = {
    { 50, 50, 50 }, { 60, 70, 80 }, { 70, 90, 120 }, { 80, 110, 150 },
    { 100, 160, 200 }, { 130, 200, 250 }, { 180, 250, 300 }, { 250, 300, 350 },
    { 300, 350, 400 }, { 350, 400, 450 }, { 400, 450, 500 }, { 450, 500, 550 },
};
static const uint16_t t65_bu_lim7[12][3] = {
    { 24, 24, 24 }, { 18, 18, 18 }, { 10, 10, 10 }, { 0, 0, 0 },
    { 20, 20, 20 }, { 40, 40, 40 }, { 70, 70, 70 }, { 110, 110, 110 },
    { 160, 160, 160 }, { 220, 220, 220 }, { 290, 290, 290 }, { 370, 370, 370 },
};

static inline uint32_t t65_udiv(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;       /* the vendor traps on 0 */
}

/* the vendor's float -> unsigned conversion (trunc around 2^31) */
static inline uint32_t t65_f2u(float f)
{
    if (f >= 2147483648.0f)
        return (uint32_t)(int32_t)(f - 2147483648.0f) | 0x80000000u;
    return (uint32_t)(int32_t)f;
}

/* bits of the CTUs [lo, hi) of the complexity map @m (u32 per CTU) in the
 * share of @bits: sum * bits / tot (64 bit, @bits sign-extended) */
static uint32_t t65_bu_share(E33 *X, uint32_t m, int32_t lo, int32_t hi, uint32_t bits, uint32_t tot)
{
    uint64_t sum = 0;

    if (lo >= hi)
        return 0;
    for (int32_t i = lo; i < hi; i++)
        sum += LD_U32(m + 4 * i);
    return (uint32_t)((sum * (uint64_t)(int64_t)(int32_t)bits) / (uint64_t)tot);
}

/*
 * Per-slice target bits (get_bu_target_bits, 0x19e254): the H.264 code
 * (0x19c62c) on 64x64 CTUs.  Basic-unit size s+0x90c (CTUs) and count
 * s+0x910, the targets s+0x948.. (one per unit; cumulative when @cum), the
 * per-unit limits of the revision (s+0x916.. rev 6, s+0x92e.. rev 7).
 * @vb: +4 stream parameters (byte 0 = HW revision, +0x88 the last picture
 * bits, +0x98.. / +0x198 the slice sizes of the last IDR and their sum),
 * +8 picture parameters, +0xc the per-CTU complexity map (+0x4c).
 */
static void e65_bu_target_bits(E33 *X, uint32_t vb, uint32_t s, int32_t cum, int32_t lvl, int8_t col,
                               uint32_t bits)
{
    uint32_t cur = LD_U32(vb + 4), t = s + 0x948, per, tot = LD_U32(cur + 0x88), mbw, mbs, bu, n, rl;
    int8_t rev = LD_S8(cur);
    int32_t rows, i;

    rl = (uint32_t)lvl < 9 ? t65_bu_rows[lvl] : 0;
    if (rev == 6) {
        uint32_t r;

        mbw = LD_U16(s + 6) >> 6;
        r = rl * mbw;
        mbs = (uint32_t)((LD_U16(s + 8) + 0x3f) >> 6) * mbw;
        if (r < mbw)
            bu = mbw;
        else
            bu = (uint32_t)((int32_t)mbs >> 1) < r ? (uint32_t)((int32_t)mbs >> 1) : r;
        n = t65_udiv(mbs - 1 + bu, bu);
        if (n > 0x80) {
            bu = mbs >> 7;
            n = 0x80;
        }
        ST_U32(s + 0x90c, bu);
        ST_U32(s + 0x910, n);
    } else {
        mbw = (LD_U16(s + 6) + 0x3f) >> 6;
        if (rev == 7) {
            uint32_t r = rl * mbw;
            int32_t m = ((LD_U16(s + 8) + 0x3f) >> 6) * (int32_t)mbw;

            ST_U8(s + 0xb5c, 1);
            if (r < mbw)
                bu = mbw;
            else
                bu = (uint32_t)(m >> 1) < r ? (uint32_t)(m >> 1) : r;
            n = t65_udiv((uint32_t)m + bu - 1, bu);
            ST_U32(s + 0x90c, bu);
            ST_U32(s + 0x910, n);
            if (n > 0x3f) {
                bu = (uint32_t)(m >> 6) + 1;
                n = t65_udiv((uint32_t)m + bu - 1, bu);
                ST_U32(s + 0x90c, bu);
                ST_U32(s + 0x910, n);
            }
            ST_S16(s + 0xb5e, (int16_t)bu);
        } else {
            n = LD_U32(s + 0x910);
        }
        if (n < 2)
            return;                     /* the vendor asserts */
        bu = LD_U32(s + 0x90c);
    }
    rows = (int8_t)t65_udiv(bu, mbw);
    per = t65_udiv(bits, n);
    if (n == 0)
        return;                         /* the vendor traps */

    if (rev == 7 && LD_S32(LD_U32(vb + 8)) >= 1 && tot != 0) {
        /* from the per-CTU complexity of the last picture */
        uint32_t m = LD_U32(vb + 0xc) + 0x4c;

        for (i = 0; i < (int32_t)n; i++)
            ST_U32(t + 4 * i, cum ? t65_bu_share(X, m, 0, rows * (i + 1), bits, tot)
                                  : t65_bu_share(X, m, rows * i, rows * (i + 1), bits, tot));
    } else if (rev == 6 && LD_S8(s + 2) == 2 && LD_S8(LD_U32(vb + 8) + 0x2f) != 0 && LD_U32(cur + 0x198) != 0) {
        /* IDR: from the slice sizes of the last IDR (n - 1 of them) */
        float den = (float)(double)LD_U32(cur + 0x198), fb = (float)(int32_t)bits;
        int32_t acc = 0;

        for (i = 0; i < (int32_t)n - 1; i++) {
            if (!cum) {
                ST_U32(t + 4 * i, t65_f2u((float)(double)LD_U32(cur + 0x98 + 4 * i) / den * fb));
            } else {
                acc += LD_S32(cur + 0x98 + 4 * i);
                ST_U32(t + 4 * i, t65_f2u((float)acc / den * fb));
            }
        }
    } else {
        /* first third 1.5x, second third 1x, last third 0.5x */
        uint32_t third = n / 3, two = n * 2 / 3;

        if (!cum) {
            for (i = 0; i < (int32_t)third; i++)
                ST_U32(t + 4 * i, per * 3 >> 1);
            for (i = third; i < (int32_t)two; i++)
                ST_U32(t + 4 * i, per);
            for (i = two; i < (int32_t)n; i++)
                ST_U32(t + 4 * i, per >> 1);
        } else {
            uint32_t b1 = third * per * 3 >> 1, b2 = (two - third) * per + b1;

            for (i = 0; i < (int32_t)third; i++)
                ST_U32(t + 4 * i, per * 3 * (uint32_t)(i + 1) >> 1);
            for (i = third; i < (int32_t)two; i++)
                ST_U32(t + 4 * i, (uint32_t)(i - third + 1) * per + b1);
            for (i = two; i < (int32_t)n; i++)
                ST_U32(t + 4 * i, ((uint32_t)(i - two + 1) * per >> 1) + b2);
        }
    }
    for (i = 0; i < (int32_t)n; i++)
        if (LD_U32(t + 4 * i) > 0xfffffff)
            ST_U32(t + 4 * i, 0xfffffff);

    if (rev == 6) {
        int32_t p100 = (int32_t)bits / 100;

        for (int k = 0; k < 12; k++) {
            int32_t lo = (p100 * (2 + 3 * k)) >> 4;
            int32_t v = (int32_t)(((uint32_t)t65_bu_lim6[k][col] * per / 100 >> 4) & 0xffff);
            uint32_t m = (uint32_t)(lo <= v ? v : lo);

            ST_U16(s + 0x916 + 2 * k, (m & 0xffff) < 4 ? 4 : m);
        }
    } else if (rev == 7) {
        for (int k = 0; k < 12; k++) {
            int16_t v = (int16_t)((uint32_t)t65_bu_lim7[k][col] * per / 0xc80);

            if (v < 4)
                v = 4;
            ST_S16(s + 0x92e + 2 * k, k < 3 ? -v : v);
        }
    }
}
