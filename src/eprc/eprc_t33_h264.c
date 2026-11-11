/*
 * Ingenic "eprc" picture rate control, T33 revision, H.264 (#295, planned):
 * reimplemented from the vendor T32 libimp 2.2.0 (eprc_default_set_H264_T33,
 * JZ_VPU_RC_*_H264_T33 and their helpers at 0x18b9f0..0x19d550), which the
 * vendor uses on the T32 (HERA).  The state keeps the vendor layout in the
 * arena of eprc_t33_mem.h; offsets are named where their meaning is known,
 * comments give vendor code addresses.  Work in progress: the helpers below
 * are checked call by call against the vendor controller
 * (tools/t32/eprc33/diff.py).
 *
 * Blocks (arena addresses passed around as uint32_t):
 *   cfg  = controller block + 4 (picture size, frame rate, QP limits)
 *   st   = the per-picture state of a mode (state block + 0x570 ...)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("wrapv")
#endif
#include "eprc/eprc_t33_mem.h"

#include <math.h>

/* ------------------------------------------------------------ constants */

/* I/P ratio model: slope / offset per 20 %-step of the I share (0x1e9c55) */
static const int8_t t33_iratio_slope[5] = { 40, 25, 20, 15, 10 };
static const int8_t t33_iratio_ofs[5] = { 0, 3, 5, 8, 12 };

/* bits-per-pixel classes of the reference QP model (0x1e9d78 ..) */
static const float t33_bpp_class[7] = { 0.32f, 0.122f, 0.0469f, 0.0213f, 0.0102f, 0.0049f, 0.0024f };
/* QP = slope * bpp + ofs per class (0x1e9c28) */
static const int16_t t33_bpp_slope[7] = { -10, -25, -66, -195, -450, -943, -1599 };
static const int16_t t33_bpp_ofs[7] = { 25, 30, 35, 41, 46, 51, 54 };

/* fractional steps of a size ratio in 1 dQP units (update_qp, 0x1e9d7c ..) */
static const float t33_frac_step[5] = { 0.122f, 0.26f, 0.414f, 0.587f, 0.782f };

/* P-picture dQP thresholds, three per row (adjust_P_frame_dqp, 0x1e9c60) */
static const float t33_pdqp_row[5][3] = {
    { 0.2f, 0.4f, 0.65f }, { 0.3f, 0.5f, 0.75f }, { 0.3f, 0.5f, 0.75f },
    { 0.3f, 0.5f, 0.75f }, { 0.3f, 0.6f, 0.8f },
};

static inline uint32_t clip_u(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int32_t clip_s(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* --------------------------------------------------------------- helpers */

/* QP difference of the I picture from the I share of a GOP for two
 * shares @a and @b (percent; get_iratio_idqp, 0x18e93c), corrected by the
 * bit rate (@bits: int64 bit/s at that address). */
static float t33_iratio_qp(int32_t share)
{
    float f;
    int k;

    if (share < 100) {
        k = share < 20 ? 0 : share < 40 ? 1 : share < 60 ? 2 : share < 80 ? 3 : 4;
        f = (float)((double)(t33_iratio_slope[k] * share) / 100.0 + (double)t33_iratio_ofs[k]);
    } else {
        f = 22.0f;
    }
    if (share > 100)
        f = share < 170 ? (float)((double)((share - 100) / 10) + 22.0) : 29.0f;
    return f;
}

static int32_t e33_iratio_idqp(E33 *X, uint32_t bits, int32_t a, int32_t b)
{
    float fa = t33_iratio_qp(a), fb = t33_iratio_qp(b), diff, mb;
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

/* clamp the picture QP (st+0x90) to the window of its kind and to the QP
 * limits of @cfg (limit_frm_qp, 0x18ec34).  @win: s8 window table (I max /
 * I min at 0/1, P half-widths at 0x18..0x1a), @frames: int (pictures). */
static void e33_limit_qp(E33 *X, uint32_t cfg, uint32_t frames, uint32_t st, uint32_t win)
{
    int8_t kind = LD_S8(st + 0x7b);
    int32_t qp = LD_S32(st + 0x90);

    if (kind == 2 || kind == 6) {
        qp = clip_s(qp, LD_S8(win + 1), LD_S8(win));
    } else if (kind == 0) {
        int32_t n = LD_S32(frames);
        int64_t spent = LD_S64(st + 0x48);
        int apply = 1;

        if (n != 0 && spent > 0 && (spent >> 32) == 0 && (uint32_t)(n * 3) / 5u < (uint32_t)spent)
            apply = 0;
        if (n != 0 && (spent >> 32) > 0)
            apply = 0;
        if (apply) {
            int8_t src = LD_S8(st + 0x30), ref = LD_S8(st + 0x60);
            int8_t d = LD_S8(win + (src == 2 ? 0x18 : src == 6 ? 0x1a : 0x19));

            qp = clip_s(qp, (int8_t)(ref - d), (int8_t)(ref + d));
        }
    }
    ST_S32(st + 0x90, clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18)));
}

/* reference QP of an I picture from its bits per pixel (get_refer_qp_mode0,
 * 0x18f590): @cfg picture size, @pixels int, @bits int64 */
static int32_t e33_refer_qp_bpp(E33 *X, uint32_t cfg, uint32_t pixels, uint32_t bits)
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
    f = (float)LD_S32(pixels) * f;
    if (f == 0.0f)
        return 0;
    bpp = (float)LD_S64(bits) / f;
    for (k = 0; k < 7 && bpp <= t33_bpp_class[k]; k++)
        ;
    if (k == 7)
        qp = 51;
    else
        qp = (int32_t)((float)t33_bpp_slope[k] * bpp + (float)t33_bpp_ofs[k]);
    /* signed: a large bpp gives a negative QP, which ends at the minimum (slt, 0x18f6c4) */
    return (int8_t)clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18));
}

/* dQP for a size ratio @b / @a in steps of 6 per doubling (update_qp,
 * 0x1960cc): negative when @b is larger */
static int32_t e33_ratio_dqp(E33 *X, int32_t a, int32_t b)
{
    float q, fr;
    int32_t n, k;

    (void)X;
    if (a < 1)
        a = 1;
    if (a == 1) {
        a = b;
        if (b < 1)
            return 0;
    }
    if (b == 0 || a == 0)
        return 0;               /* the vendor asserts */
    if (a < 1)
        a = 1;
    if (b < 1)
        b = 1;
    if (a < b) {
        q = (float)b / (float)a;
        n = (int32_t)q;
        if ((n - 1) * 6 > 51)
            return 51;
        fr = q - (float)n;
        for (k = 0; k < 5 && t33_frac_step[k] <= fr; k++)
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
        for (k = 0; k < 5 && t33_frac_step[k] <= fr; k++)
            ;
        if (k == 5 && 1.0f <= fr)
            return 0;
        return (int8_t)((int8_t)((n - 1) * 6) + k);
    }
    return 0;
}

/* reference QP from the picture size and frame-rate ratio
 * (get_refer_qp_mode1, 0x196398) */
static int32_t e33_refer_qp_size(E33 *X, uint32_t cfg, int32_t pixels, int32_t gop_pos, uint32_t unused,
                          int32_t bits)
{
    int32_t a = e33_ratio_dqp(X, 1024000, bits), b = e33_ratio_dqp(X, 921600, pixels);
    int32_t num = LD_S32(cfg + 0x10), k;
    uint32_t den = LD_U32(cfg + 0x14);
    int32_t qp;

    (void)unused;
    k = den > (uint32_t)(num * 30) ? -2 : den > (uint32_t)(num * 25) ? -1 : den > (uint32_t)(num * 20) ? 0
      : den > (uint32_t)(num * 15) ? 1 : den > (uint32_t)(num * 10) ? 2 : 3;
    qp = (int8_t)a + (int8_t)b + k + (int8_t)(gop_pos < 3 ? gop_pos : 2) + 37;
    return (int8_t)clip_s(qp, LD_U8(cfg + 0x19), LD_U8(cfg + 0x18));
}

/* dQP of a P picture from its predicted size against the target
 * (adjust_P_frame_dqp, 0x18c328): @cfg mode at +0x4d, @st direction at
 * +0x94 and target bits (int64) at +0x48, @bits predicted bits, @row the
 * threshold row */
static int32_t e33_adjust_p_dqp(E33 *X, uint32_t cfg, uint32_t st, uint32_t u3, uint32_t u4, uint32_t bits_lo,
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
        const float *th = t33_pdqp_row[row];

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

/* window borders of the P-picture QP model: fifths of @n (update_pwin,
 * 0x191844), the window sums cleared */
static void e33_update_pwin(E33 *X, uint32_t src, uint32_t st)
{
    int32_t n = LD_S32(src + 0xc), a = n / 5, b, c;
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

/* picture-level outputs of the controller (RC_output_param, 0x18ed8c) */
static void e33_output_param(E33 *X, uint32_t e, int32_t mb_qp)
{
    uint32_t pic = LD_U32(e + 0x86c);
    uint32_t qp = LD_U32(pic + 8);
    int8_t kind = LD_S8(pic + 0x20);

    ST_U8(e + 0x810, qp);
    ST_U8(e + 0x81c, LD_U8(LD_U32(e + 0x878) + 9));
    ST_U32(e + 0x814, LD_U32(pic + 0x10));
    ST_U32(e + 0x818, LD_U32(pic + 0x18));
    if (kind == 6)
        kind = 0;
    ST_S8(e + 0x811, kind);
    if (LD_U32(pic + 8) != (qp & 0xff) && LD_S8(e + 0x52) != kind)
        return;                 /* the vendor prints and exits */
    if (mb_qp == 0) {
        ST_U8(e + 0x81d, 0);
        ST_U8(e + 0x81e, 0);
    } else {
        ST_U8(e + 0x81d, LD_U8(LD_U32(e + 0x8b8) + 2));
        ST_U8(e + 0x81e, LD_U8(LD_U32(e + 0x8b8) + 3));
    }
}

/* VBV sizes (vbv_init, 0x18ee4c): @cfg frame rate, @mode byte, @st */
static void e33_vbv_init(E33 *X, uint32_t cfg, uint32_t mode, uint32_t st)
{
    uint64_t rate;

    if (LD_S32(st + 0x14) == 0) {
        ST_U32(st + 0x428, 0);
        ST_U32(st + 0x42c, 0);
        ST_U32(st + 0x430, 0);
        /* buffer = 2 * rate * seconds (64-bit wrap as the vendor) */
        ST_U64(st + 0x420, (LD_U64(st + 0x2f0) * 2u) * (uint64_t)(int64_t)LD_S32(st + 0x2d8));
    }
    if (LD_S32(cfg + 0x14) == 0)
        return;                 /* the vendor asserts */
    if (LD_U8(mode) == 1)
        rate = LD_U64(st + 0x2e0);
    else if ((uint8_t)(LD_U8(mode) - 2) <= 3)
        rate = LD_U64(st + 0x2f0);
    else
        return;
    ST_S64(st + 0x438, (int64_t)(rate * LD_U32(cfg + 0x10)) / (int64_t)LD_U32(cfg + 0x14));
}

/* CBR target scale (percent of the bit rate) by rate-quality level, and the
 * matching lower bound (0x1e9ce8 / 0x1e9cf1) */
static const uint8_t t33_cbr_max_pct[9] = { 100, 110, 120, 130, 140, 150, 160, 170, 180 };
static const uint8_t t33_cbr_min_pct[9] = { 100, 90, 80, 80, 80, 80, 80, 80, 80 };

enum { T33_FIXQP, T33_CBR, T33_VBR, T33_SMART, T33_CVBR, T33_AVBR };

/* (re)derive the rate budgets of the mode (init_stat_param, 0x18ef74):
 * @cfg controller parameters (+4 mode, +0x10 kbit/s), @inst instance
 * block, @st statistics block, @r rate block (+8 target, +0x10 maximum) */
static void e33_init_stat(E33 *X, uint32_t cfg, uint32_t inst, uint32_t st, uint32_t r)
{
    uint8_t mode = LD_U8(cfg + 4);
    uint32_t kbps;
    int64_t d;

    if (mode >= T33_CBR && mode <= T33_AVBR) {
        kbps = LD_U32(cfg + 0x10);
        ST_U32(cfg + 0x10, kbps < 2 ? 2 : kbps > 0x19000 ? 0x19000 : kbps);
    }
    kbps = LD_U32(cfg + 0x10);
    switch (mode) {
    case T33_CBR:
        d = LD_S64(r + 8);
        ST_U64(r + 8, (uint32_t)(kbps << 10));
        if (d != (int64_t)(uint32_t)(kbps << 10))
            ST_U64(st + 0x428, 0);
        break;
    case T33_VBR: {
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
    case T33_SMART:
    case T33_AVBR: {
        uint8_t pct = LD_U8(cfg + 0x20);
        int64_t t;

        ST_U64(r + 0x10, (uint32_t)(kbps << 10));
        ST_S64(r + 8, (int64_t)((uint64_t)kbps * 1024u * pct) / 100);
        t = (int64_t)((uint64_t)pct * (uint32_t)(kbps << 10)) / 100;
        ST_S64(inst + 0x68, (int64_t)((uint64_t)t * LD_U8(cfg + 0x26)) / 100);
        ST_U8(inst + 0x7d, LD_U8(cfg + 0x2a));
        ST_U8(inst + 0x7e, LD_U8(cfg + 0x2b));
        if (mode == T33_SMART) {
            t = (int64_t)(LD_U64(r + 0x10) * pct) / 100;
            ST_S64(inst + 0x70, (int64_t)((uint64_t)t * LD_U8(cfg + 0x2f)) / 100);
        }
        break;
    }
    case T33_CVBR:
        ST_U64(r + 0x10, (uint32_t)(kbps << 10));
        break;
    default:
        break;
    }
    switch (mode) {
    case T33_CBR: {
        uint64_t t = LD_U64(r + 8);
        uint8_t lvl = LD_U8(cfg + 0x14);

        ST_U64(st + 0x2e0, t);
        ST_S64(st + 0x2f0, (int64_t)(t * t33_cbr_max_pct[lvl]) / 100);
        ST_S64(st + 0x2f8, (int64_t)(t * t33_cbr_min_pct[lvl]) / 100);
        break;
    }
    case T33_VBR:
        ST_U64(st + 0x2e0, LD_U64(r + 8));
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, 0);
        break;
    case T33_SMART:
    case T33_AVBR:
        ST_U64(st + 0x2e0, LD_U64(r + 8));
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, LD_U64(inst + 0x68));
        break;
    case T33_CVBR:
        ST_U64(st + 0x2f0, LD_U64(r + 0x10));
        ST_U64(st + 0x2f8, LD_U64(st + 0x278));
        ST_U64(st + 0x2e0, LD_U64(st + 0x270));
        break;
    }
    if (mode >= T33_CBR && mode <= T33_AVBR)
        e33_vbv_init(X, LD_U32(cfg), cfg + 4, st);
    {
        int64_t sec = LD_S32(st + 0x2d8);

        ST_U64(st + 0x320, LD_U64(st + 0x2f0));
        ST_U64(st + 0x330, 0);
        ST_U64(st + 0x328, LD_U64(st + 0x2e0) * (uint64_t)sec);
        ST_U64(st + 0x318, LD_U64(st + 0x2f0) * (uint64_t)sec);
    }
}

/* the bit budget of the picture (FRC_get_bitrate, 0x197898): restart the
 * statistics after a change, refresh them, and give the instance (+0x30)
 * the larger of the maximum rate and the lower limit at @inst+0x1c -> +2000 */
static void e33_frc_bitrate(E33 *X, uint32_t cfg, uint32_t out, uint32_t st, uint32_t inst)
{
    int32_t n = LD_S32(st + 0x398), lim;
    uint8_t mode = LD_U8(cfg + 4);
    int64_t rate;

    if (n != 0) {
        if (n >= 2) {
            if (LD_S32(st + 0x358) == 0)
                return;         /* the vendor asserts */
            ST_S64(st + 0x368, LD_S64(st + 0x380) / (int64_t)LD_S32(st + 0x358));
            ST_U64(st + 0x428, 0);
            ST_S8(st + 0xdc, (int32_t)(((double)LD_S32(st + 0xe0) + 0.5) / (double)(n - 1)));
        }
        ST_U32(st + 0x398, 0);
        ST_U32(st + 0x338, 0);
        ST_U32(st + 0x2b8, 0);
    }
    if (mode >= T33_CBR && mode <= T33_AVBR) {
        if (mode == T33_CVBR && LD_S32(st + 0x2b8) == 0) {
            ST_U64(st + 0x2b0, 0);
            ST_U64(st + 0x288, 0);
            ST_U64(st + 0x2a8, LD_U64(st + 0x270) * (uint64_t)(int64_t)LD_S32(st + 0x258));
        }
        if (LD_S32(st + 0x338) == 0)
            e33_init_stat(X, cfg, out, st, inst);
        if (LD_S32(st + 0x398) == 0)
            e33_init_inst(X, cfg, out, st, inst);
    }
    lim = LD_S32(LD_U32(inst + 0x1c) + 2000);
    rate = LD_S64(st + 0x2f0);
    if (lim == 0) {
        if ((int8_t)LD_S32(cfg + 4) == T33_CBR)
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

/* ----------------------------------------------------- scene classes */

/* scene block (controller block + 0x878 ->): +0 -> class output byte,
 * +4 -> class history (32 bytes, newest first), +8 class in force,
 * +9 class of this picture, +0x1c activity, +0x40 motion level */
#define SC_OUT   0x0
#define SC_HIST  0x4
#define SC_CUR   0x8
#define SC_NEW   0x9

static void t33_hist_push(E33 *X, uint32_t sc, uint8_t v)
{
    uint32_t h = LD_U32(sc + SC_HIST);

    for (int i = 31; i > 0; i--)
        ST_U8(h + i, LD_U8(h + i - 1));
    ST_U8(h, v);
}

/* class of the picture from the engine's block-type statistics
 * (get_scene_frame_method1, 0x18e0f8): @cfgp -> picture parameters,
 * @stat statistics block, @pic -> picture info */
static void e33_scene_class(E33 *X, uint32_t cfgp, uint32_t stat, uint32_t pic, uint32_t sc)
{
    uint32_t cfg = LD_U32(cfgp), mbs, a, b, s, t, h0;
    uint32_t c8 = LD_U16(stat + 0xf8), c9 = (LD_U16(stat + 0xfa) + LD_U16(stat + 0xfc)) & 0xffff;
    uint32_t still = LD_U16(stat + 0xfe);
    int k, m = -1;
    uint8_t lvl;

    mbs = (LD_U16(cfg + 4) * LD_U16(cfg + 6)) & 0xffff;
    if (mbs == 0)
        mbs = 1;
    a = (still * 100 / mbs) & 0xffff;
    ST_U8(sc + 0x1c, (((still + (c9 * 7 + c8 * 3) / 10) & 0xffff) * 100) / mbs);
    b = ((((uint32_t)LD_U16(stat + 0xf0) + LD_U16(stat + 0xf2) + LD_U16(stat + 0xf4) + LD_U16(stat + 0xf6)) & 0xffff)
         * 100 / mbs) & 0xffff;
    h0 = LD_U8(LD_U32(sc + SC_HIST));
    k = 4;
    if (a > 80) {
        k = 6;
    } else if (a + (s = (c9 * 100 / mbs) & 0xffff) >= 81) {
        k = a < 61 ? 4 : 5;
    } else {
        t = (c8 * 100 / mbs) & 0xffff;
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
    t33_hist_push(X, sc, (uint8_t)m);
    ST_U8(sc + SC_NEW, m);
    lvl = 3;
    if (LD_S32(LD_U32(pic)) != 0)
        lvl = b >= 95 ? 0 : b >= 75 ? 1 : 2;
    ST_U8(sc + 0x40, lvl);
}

/* class map when the last ten pictures agree (0x1e9670) */
static const uint8_t t33_class_same[8] = { 0, 1, 2, 4, 3, 5, 6, 7 };

static void t33_class_set(E33 *X, uint32_t sc, uint8_t v)
{
    ST_U8(sc + SC_CUR, v);
    ST_U8(LD_U32(sc + SC_OUT), v);
}

/* scene class in force from the class history (IFA_get_frm_feature,
 * 0x18e3b0) */
static void e33_frame_feature(E33 *X, uint32_t e)
{
    uint32_t fi = LD_U32(e + 0x87c), sc = LD_U32(e + 0x878), h;
    uint8_t c, v[10];
    int rev = LD_S8(e + 0x34), i;

    if (LD_S8(LD_U32(fi + 8)) == 1)
        return;
    if (LD_S8(LD_U32(fi + 4) + 0x20) == 2) {
        t33_hist_push(X, sc, LD_U8(sc + SC_NEW));
        goto tail;
    }
    if (rev == 6 || rev == 7)
        e33_scene_class(X, e + 0x874, LD_U32(fi), fi + 4, sc);
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
            t33_class_set(X, sc, t33_class_same[c]);
        else {
            ST_U8(sc + SC_CUR, t33_class_same[c]);
            ST_U8(LD_U32(sc + SC_OUT), c);
        }
        goto tail;
    }
#define ANY(x) (v[1] == (x) || v[2] == (x) || v[3] == (x) || v[4] == (x) || v[5] == (x) || v[6] == (x) \
                || v[7] == (x) || v[8] == (x) || v[9] == (x))
    if (ANY(6)) {
        t33_class_set(X, sc, 6);
    } else if (ANY(5)) {
        t33_class_set(X, sc, 5);
    } else if (ANY(1)) {
        if (c != 0)
            ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
        else
            t33_class_set(X, sc, 1);
    } else if (ANY(3) || ANY(4)) {
        if (c == 0)
            t33_class_set(X, sc, 4);
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
            t33_class_set(X, sc, c != 0 ? 1 : 4);
        } else if (mixed && !zero_late) {
            if (c != 0)
                ST_U8(LD_U32(sc + SC_OUT), LD_U8(sc + SC_CUR));
            else
                t33_class_set(X, sc, two ? 4 : 0);
        } else if (c != 0) {
            t33_class_set(X, sc, 1);
        } else {
            t33_class_set(X, sc, two ? 4 : 0);
        }
    }
#undef ANY
tail:
    if (rev == 7) {
        uint32_t src = LD_U32(fi) + 0x228;

        for (i = 0; i < 32; i++)
            ST_U32(sc + 0x44 + 4 * i, LD_U32(src + 4 * i));
    }
}

/* ------------------------------------------------------ I-picture share */

/* share model of the I picture (percent of the GOP budget) over the mean
 * I/P complexity v: per bit-rate class a piece-wise linear curve with the
 * knots t33_ishare_knot (0x1e9ce0), value base - slope * (v - lo) / (hi - lo)
 * (0x1e9cac, rows of base[5] + slope[5]) */
static const int8_t t33_ishare_base[4][5] = {
    { 20, 50, 60, 80, 90 }, { 20, 50, 50, 65, 85 }, { 15, 50, 50, 55, 80 }, { 15, 40, 40, 50, 70 },
};
static const int8_t t33_ishare_slope[4][5] = {
    { 10, 30, 10, 20, 10 }, { 10, 30, 0, 15, 20 }, { 7, 35, 0, 5, 25 }, { 7, 25, 0, 10, 20 },
};
static const int8_t t33_ishare_knot[5] = { 100, 50, 20, 10, 5 };
/* bit-rate classes (bit/s / 10000, 0x1e9c9c) */
static const int32_t t33_ishare_rate[4] = { 8000, 6000, 4000, 2000 };
/* SMART/AVBR still-scene cut in percent by motion class and level (0x1e9bd8) */
static const int8_t t33_ishare_still[5][8] = {
    { -30, -30, -30, -30, 20, 20, 20, 20 }, { -20, -20, -20, -20, 20, 20, 20, 20 },
    { -13, -13, -13, -13, 20, 20, 20, 20 }, { 0, 0, 0, 0, 10, 10, 10, 10 },
    { 10, 10, 10, 10, 30, 40, 50, 60 },
};

static int32_t t33_ishare_curve(int k, int seg, int32_t v)
{
    int32_t lo = seg < 4 ? t33_ishare_knot[seg + 1] : 0;

    return t33_ishare_base[k][seg] - t33_ishare_slope[k][seg] * (v - lo) / (t33_ishare_knot[seg] - lo);
}

/* share of the I picture in percent (decide_Iqp_proportion, 0x18f7f8):
 * @rc rate block (+0 -> cfg (+0xc GOP length), +4 mode, +0x3c bit budget
 * cap), @inst
 * instance block (+0x7d still mode, +0x7e level), @st statistics (+0x234
 * ten s8 I/P complexity history, +0xa0 bit/s, +0xdc last I QP, +0x2e0
 * target, +0x358 seconds), @pic picture state (+0x1c complexity, +0x1d
 * still strength) */
static int32_t e33_i_share(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t pic)
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
        a = t33_ishare_curve(hi, seg, v);
        b = t33_ishare_curve(lo, seg, v);
    } else {
        a = t33_ishare_base[hi][4];
        b = t33_ishare_base[lo][4];
    }
    share = (uint32_t)a;
    if (interp)
        share += (int32_t)((uint32_t)(b - a) * (uint32_t)(rate - t33_ishare_rate[hi]))
               / (t33_ishare_rate[lo] - t33_ishare_rate[hi]);

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

        if (mode != T33_SMART && mode != T33_AVBR)
            goto cap;
        m = (qi + LD_S32(st + 0x58)) >> 1;
        cls = m < 11 ? 0 : m < 21 ? 1 : m < 33 ? 2 : m < 46 ? 3 : m < 52 ? 4 : 3;
        if (LD_S8(inst + 0x7d) == 1)
            share = (int32_t)((100 - LD_S8(pic + 0x1d) * t33_ishare_still[cls][LD_S8(inst + 0x7e)] / 100)
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

/* I QP from the history of I pictures (get_history_iqp, 0x18b9f0): @st
 * keeps the sizes of the last ten I pictures (int64 at +0x1d0) and their QPs
 * (s8 at +0x22a).  The QP of the picture closest above @lo_pct percent of
 * the budget (@bits * seconds at st+0x358) is a lower bound, the QP of the
 * one closest below @hi_pct an upper bound: q_lo + 1 <= qp < q_hi (the
 * caller passes the larger share as @lo_pct). */
static int32_t e33_history_iqp(E33 *X, uint32_t st, int32_t lo_pct, int32_t hi_pct, int32_t qp, int32_t bits)
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

/* ---------------------------------------------------------- I picture QP */

/* forget the I-picture history (sizes, QPs, QP window) after a change */
static void t33_iqp_reset(E33 *X, uint32_t st)
{
    ST_S8(st + 0x111, 52);
    ST_S8(st + 0x110, 0);
    for (int i = 0; i < 0x50; i++)
        ST_U8(st + 0x1d0 + i, 0);
    for (int i = 0; i < 10; i++)
        ST_S8(st + 0x22a + i, 0);
}

/* the vendor skips QP 39 in two places: one step in the direction of dQP */
static int32_t t33_skip39(int32_t q, int32_t dqp)
{
    if ((int8_t)q != 39)
        return (int8_t)q;
    return dqp < 1 ? (int8_t)(q - (dqp != 0)) : (int8_t)(q + 1);
}

static uint32_t t33_gop_share(uint32_t gop, uint32_t share)
{
    return gop < 30 ? share : share * gop / (((gop - 30) >> 1) + 30);
}

/* budgets of the I picture and of a P picture for the GOP (+0x80, +0x88) */
static void t33_iqp_budgets(E33 *X, uint32_t inst, uint32_t st, int64_t share)
{
    int32_t sec = LD_S32(st + 0x358);

    ST_S64(st + 0x80, (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)share * (uint64_t)(int64_t)sec) / 100);
    ST_S64(st + 0x88, (int64_t)((uint64_t)LD_S64(st + 0x2e0) * (uint64_t)(int64_t)LD_S32(inst + 0x10)
                                * (uint64_t)(int64_t)sec) / 100);
}

/* QP of an I picture (get_I_frame_qp, 0x187b78).  @rc rate block (+0 ->
 * cfg, +4 mode, +5 fixed I QP or -1), @inst instance parameters (+0x10 /
 * +0x14 I share limits in percent, +0x28 maximum QP step, +0x54 reference
 * model, +0x7c history use, +0x78 SMART/AVBR I QP cap), @st statistics,
 * @out picture block (+8 int64 bit rate, +0x20 -> picture state) */
static int32_t e33_iqp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
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
        t33_iqp_budgets(X, inst, st, 60);
        if (LD_S8(rc + 5) != -1)
            return LD_S8(rc + 5);
        if (LD_S8(inst + 0x54) == 0)
            return e33_refer_qp_bpp(X, cfg, inst + 8, out + 8);
        return e33_refer_qp_size(X, cfg, LD_S32(inst + 8), sec, 0, LD_S32(out + 8));
    }
    gop = LD_S32(cfg + 0xc);
    if (gop == 1) {
        int64_t v = (int64_t)((uint64_t)LD_S64(st + 0x360) * (uint64_t)(int64_t)sec);

        ST_S64(st + 0x80, v);
        ST_S64(st + 0x88, v);
        return (int8_t)((int8_t)LD_U32(st + 0x58) + (int8_t)e33_iratio_idqp(X, bits_a, LD_S32(st + 0x68), 100));
    }
    pic = LD_U32(out + 0x20);
    share = e33_i_share(X, rc, inst, st, pic);

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
    refqp = e33_refer_qp_bpp(X, cfg, inst + 8, out + 8);

    stable_scene = LD_S8(inst + 0x7c) != 0 && LD_S8(pic + 8) == 0 && LD_S8(st + 0x98) != 0;
    ST_S8(st + 0x99, stable_scene);
    if (!stable_scene) {
        if (!stable) {
            uint32_t s;

            t33_iqp_reset(X, st);
            s = t33_gop_share((uint32_t)gop, (uint32_t)share);
            qp = qcls;
            if ((int32_t)s > 19) {
                int32_t d = e33_iratio_idqp(X, bits_a, (int32_t)(est100 / per10), share), m;

                qp = t33_skip39((int8_t)((int8_t)LD_U32(st + 0x58) + 2 + (int8_t)d), d);
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
            t33_iqp_reset(X, st);
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
    d_hi = e33_iratio_idqp(X, bits_a, (int32_t)r, LD_S32(inst + 0x14));
    q_lo = (int8_t)e33_iratio_idqp(X, bits_a, (int32_t)r, LD_S32(inst + 0x10));
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
        qb = (int8_t)(base + (int8_t)e33_iratio_idqp(X, bits_a, (int32_t)r, 0));
        hi_pct = share + 10;
    } else {
        qb = (int8_t)(base + (int8_t)e33_iratio_idqp(X, bits_a, (int32_t)r, share - 10));
        hi_pct = share < 90 ? share + 10 : 100;
    }
    qa = (int8_t)(base + (int8_t)e33_iratio_idqp(X, bits_a, (int32_t)r, hi_pct));
    q_mid = e33_iratio_idqp(X, bits_a, (int32_t)r, share);
    {
        int32_t sum = q_mid + last, cand = qcls;
        int use_mid = 0;  /* candidate: the QP of the I share (else the P class) */

        if (keep) {
            qa = qcls;
        } else {
            if (refqp < sum) {
                int32_t q2 = e33_iratio_idqp(X, bits_a, (int32_t)(est100 / per_out), share);

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
                cand = t33_skip39((int8_t)(base + (int8_t)q_mid), q_mid);
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
    h1 = e33_history_iqp(X, st, lo_pct, hi_pct, qa, (int32_t)LD_U32(st + 0x360));
    if (refqp < h1 && !keep) {
        int32_t h2 = e33_history_iqp(X, st, lo_pct, hi_pct, qa, LD_S32(out + 8));

        h1 = refqp < h2 ? (int8_t)(h2 < h1 ? h2 : h1) : refqp;
    }
    {
        uint32_t s = t33_gop_share(LD_U32(cfg + 0xc), (uint32_t)share);

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
    t33_iqp_budgets(X, inst, st, (int64_t)share);
    qp = (int8_t)(qp + LD_S8(st));
    if (LD_S8(st + 0x248) > 4)
        return qp;
    {
        int8_t mode = (int8_t)LD_S32(rc + 4);

        if (mode != T33_SMART && mode != T33_AVBR)
            return qp;
    }
    return LD_S32(inst + 0x78) < qp ? (int8_t)LD_S32(inst + 0x78) : qp;
}

/* ---------------------------------------------------------- P picture QP */

/* I-share curve over the complexity when no I picture follows (0x1e9cd4) */
static const int8_t t33_pshare_base[5] = { 10, 40, 60, 70, 80 };
static const int8_t t33_pshare_slope[5] = { 5, 30, 20, 10, 10 };

static int64_t t33_max64(int64_t a, int64_t b)
{
    return a < b ? b : a;
}

static int64_t t33_min64(int64_t a, int64_t b)
{
    return b < a ? b : a;
}

static int64_t t33_div64(int64_t a, int64_t b)
{
    return b ? a / b : 0;
}

static int32_t t33_pdqp(E33 *X, uint32_t inst, uint32_t st, uint32_t out, int64_t bits, int32_t row)
{
    return e33_adjust_p_dqp(X, inst, st, out, 0, (uint32_t)bits, (int32_t)(bits >> 32), (int8_t)row);
}

/* QP of a P picture from the remaining GOP budget (estimate_qp, 0x18ff78):
 * @rc rate block (+0 -> cfg, +4 mode), @inst instance parameters (+0x18
 * budget rule 0..7, +0x20 int64 minimum bits, +0x55/+0x58 QP floor rule,
 * +0x60 CVBR long-term bits), @st statistics (+0x2c position in the GOP,
 * +0x190.. five position thresholds with their budgets (+0x140) and
 * estimates (+0x118), +0x5c last P QP, +0x97 hold counter), @out picture
 * block (+0x20 -> picture state, +9 scene cut).  Returns the QP; the
 * budgets go to st+0x80.., the direction to st+0x94. */
static int32_t e33_estimate_qp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
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
    ST_U8(st + 0x96, 0);    /* the vendor clears it in a delay slot (0x190038) */
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
    if (mode == T33_CVBR && LD_S64(inst + 0x60) <= LD_S64(st + 0x288))
        v1 = LD_S64(st + 0x270) * sec - a;
    s = LD_S64(st + 0x168) + LD_S64(st + 0x170) + LD_S64(st + 0x178) + LD_S64(st + 0x180) + LD_S64(st + 0x188);
    v1 -= s;
    v2 -= s;
    d = (int32_t)(gop - 1) - pos;
    if (d < 1)
        d = 1;
    imin = LD_S64(inst + 0x20);
    l3 = t33_max64(imin, (v1 < 1 ? 1 : v1) / d);
    if (gop == 0 || (float)(uint32_t)(gop * 100 - 100) == 0.0f)
        return 0;           /* the vendor asserts */
    f57 = (float)(uint32_t)(gop * 100 - 100);

    /* budget and size estimate of the position class */
    pic = LD_U32(out + 0x20);
    if (cls == 4) {
        int32_t m;

        share = e33_i_share(X, rc, inst, st, pic) & 0xff;
        if (LD_S8(st + 0x99) != 1 && gop > 29)
            share = ((((gop - 30) >> 1) + 30) * (uint32_t)share / gop) & 0xff;
        m = (int32_t)gop - LD_S32(st + 0x1a0);
        f57 = (float)(int32_t)(100 - share) / f57;
        if (m == 1)
            return 0;       /* the vendor asserts */
        c59 = t33_div64(LD_S64(st + 0x160), (int64_t)(uint32_t)(m - 1));
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
            share = t33_pshare_base[seg] - t33_pshare_slope[seg] * (v - lo) / (t33_ishare_knot[seg] - lo);
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
        c59 = t33_div64(LD_S64(st + cls * 8 + 0x140), t_hi - t_lo < 1 ? 1 : t_hi - t_lo);
        c60 = (int64_t)((float)LD_S64(st + cls * 8 + 0x118) * f57);
        a4 = cls;
    }
    l59 = c59;
    l60 = c60;
    b = LD_U8(inst + 0x18);

    if (mode == T33_CBR && b >= 8) {
        l59 = imin < 0 ? 0 : imin;
        dqp = t33_pdqp(X, inst, st, out, l59, a4);
        if (dqp >= 0)
            goto p0904;
        goto p0f60;
    }
    /* the budget rule of inst+0x18 (jump tables 0x1e96d8 / 0x1e96f8) */
    l6 = mode == T33_CBR ? t33_min64(l59, l3) : t33_min64(t33_max64(l60, l59), l3);
    if (b >= 8) {
        l6 = 0;
    } else {
        l59 = l6;
        switch (b) {
        case 0: {
            int64_t x = LD_S64(st + 0x360) * sec - LD_S64(st + 0x380);

            l59 = t33_div64(x < 1 ? 1 : x, (int64_t)(uint32_t)(gop - LD_S32(st + 0x398)));
            l60 = t33_max64(l59, t33_min64(l60, l6) / 2);
            l6 = t33_min64(l60, (l6 + l3) / 2);
            break;
        }
        case 1:
            l6 = t33_min64(l6, (l6 + l3) / 2);
            break;
        case 2: {
            int64_t r;

            l5 = t33_max64(l6, l60 < 1 ? 1 : l60);
            l59 = l5;
            if (cls == 4) {
                r = t33_div64((l6 < l60 ? l60 - l6 : l6 - l60) * 100, l5);
                l59 = r < 50 ? l60 : l5;
            }
            l6 = t33_min64(l59, (l3 + l6) / 2);
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
                r = t33_div64((l5 < l60 ? l60 - l5 : l5 - l60) * 100, l5);
                l59 = r < 50 ? (l60 + l5) / 2 : t33_div64(l60 + l5 * 4, 5);
            }
            if (cls == 4) {
                if (l60 < l5)
                    l59 = (l5 + l60) / 2;
                else
                    l59 = c == 0 ? l5 : l60;
            }
tail34:
            l6 = t33_min64(l59, (l3 + l5) / 2);
            break;
        }
        case 5:
            b = 5;
            l6 = t33_min64(t33_max64(l60, l6), (l3 + l6) / 2);
            break;
        case 6:
            l6 = t33_min64(t33_max64(l60, l6), (l6 + l3) / 2);
            break;
        case 7:
            l6 = t33_max64(l6, l60);
            break;
        }
    }
    l59 = t33_max64(l6, imin);
    dqp = t33_pdqp(X, inst, st, out, l59, a4);
    if (mode == T33_CVBR) {
        int32_t lim = LD_S32(st + 0x290);

        q_p = LD_S32(st + 0x5c);
        if (lim < dqp + q_p) {
            int64_t l62;

            if (LD_S64(inst + 0x60) <= LD_S64(st + 0x288)) {
                int64_t x = LD_S64(st + 0x270) * sec - a - s;

                l3 = t33_max64((x < 1 ? 1 : x) / d, imin);
            }
            {
                double d59 = (double)l59, d3 = (double)l3;

                l62 = d59 < d3 * 0.8 ? (int64_t)(d3 * 0.8) : (int64_t)d59;
            }
            dqp = t33_pdqp(X, inst, st, out, l62, a4);
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
        if (mode == T33_CBR)
            goto p0f60;
        goto p0f6c;
    }
p0904:
    q_p = LD_S32(st + 0x5c);
    if (dqp < 1)
        goto p1114;
    if (mode != T33_CBR)
        goto p0920;
    if ((int8_t)b >= 5 && LD_S8(pic + 9) != 0)
        goto p0920;
    c43 = LD_S8(st + 0x97);
    goto p0954;
p0920:
    q_p = LD_S32(st + 0x5c);
    if (t33_pdqp(X, inst, st, out, l3, a4) > 0) {
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
        x = (int32_t)t33_div64(u61 * 100, per);
        q = e33_iratio_idqp(X, st + 0xa0, x, 50);
        if (q_p <= q + LD_S32(st + 0x58)) {
            if (t33_pdqp(X, inst, st, out, (v2 < 1 ? 1 : v2) / d, a4) >= 0)
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
static const uint8_t t33_pclass_share[2][5] = { { 80, 95, 100, 95, 100 }, { 100, 100, 100, 100, 100 } };
/* GOP-2 (st+0x30 == 2) dQP steps over the motion level (0x1e9718) */
static const uint8_t t33_g2_level[11] = { 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

static int64_t t33_pos1(int64_t v)
{
    return v < 1 ? 1 : v;
}

/* QP of a P picture (get_P_frame_qp, 0x191960): refresh the class budgets
 * (st+0x140..) and size estimates (st+0x118..) at a class boundary, then
 * the fixed QP before the first I picture (st+0x398 == 0), the GOP-2 level
 * step (st+0x30 == 2) or estimate_qp. */
static int32_t e33_p_qp(E33 *X, uint32_t rc, uint32_t inst, uint32_t st, uint32_t out)
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
            b = t33_pos1(t1 - a) * (int64_t)LD_S32(st + 0x194) / (int64_t)(gop - 1);
            ST_S64(st + 0x140, t33_pos1((int64_t)((uint64_t)b * t33_pclass_share[row][0]) / 100));
            ST_S64(st + 0x118, t1);
            continue;
        }
        for (j = 0; j < k; j++)
            s += LD_S64(st + 0x168 + j * 8);
        b = t33_pos1(t1 - a - s);
        if (k < 4)
            b = b * (int64_t)(LD_S32(st + 0x194 + k * 4) - pos) / (int64_t)d;
        ST_S64(st + 0x140 + k * 8, t33_pos1((int64_t)((uint64_t)b * t33_pclass_share[row][k]) / 100));
        e = (int64_t)((uint64_t)LD_S64(st + 0x160 + k * 8) * 100) / t33_pclass_share[row][k - 1];
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
        ST_S64(st + 0x88, t33_max64(t33_pos1(LD_S64(st + 0x370) * sec) / (int64_t)gop, imin));
        ST_S64(st + 0x80, t33_max64(t33_pos1(t1) / (int64_t)gop, imin));
        c = LD_S8(st + 0xdc);
        if (c > 0)
            return c;
        q = LD_S32(st + 0x5c);
        return q < 1 ? LD_S8(st + 0x60) : (int8_t)q;
    }
    if (LD_S8(st + 0x30) != 2)
        return e33_estimate_qp(X, rc, inst, st, out);
    lv = gop < 30 ? LD_U32(st + 0x68) : gop * LD_U32(st + 0x68) / (((gop - 30) >> 1) + 30);
    if (LD_S32(st + 0x14) != 1 && (LD_S8(inst + 0x4c) != 0 || LD_S8(pic + 0x1c) > 9))
        return e33_estimate_qp(X, rc, inst, st, out);
    for (idx = 0; idx < 11 && (int32_t)lv >= t33_g2_level[idx]; idx++)
        ;
    idx = idx < 11 ? idx - 3 : 8;
    if (gop == 1)
        return 0;           /* the vendor asserts */
    {
        int64_t i38 = LD_S64(st + 0x38);

        ST_S64(st + 0x88, t33_max64(t33_pos1(LD_S64(st + 0x370) * sec - i38) / (int64_t)(gop - 1), imin));
        ST_S64(st + 0x80, t33_max64(t33_pos1(t1 - i38) / (int64_t)(gop - 1), imin));
    }
    return (int8_t)(LD_S32(st + 0x58) + idx);
}

/* default controller settings (eprc_default_set_H264_T33, 0x198e40): @e is
 * the controller block, its +4 the configuration (1280x720, 25 fps, QP
 * 10..51); the per-mode block at +0x82c is filled for the mode at +0x824 */
static void e33_default_set(E33 *X, uint32_t e)
{
    uint32_t cfg = e + 4;

    ST_U32(e + 0x874, cfg);
    ST_U32(e + 0x820, cfg);
    ST_U32(e + 0x880, cfg);
    ST_U32(e + 0x894, cfg);
    ST_U8(e + 0xc, 0);
    ST_U16(e + 4, 1280);
    ST_U16(e + 6, 720);
    ST_U16(e + 8, 120);
    ST_U16(e + 10, 80);
    ST_U32(e + 0x10, 25);
    ST_U32(e + 0x18, 25);
    ST_U32(e + 0x14, 1);
    ST_U8(e + 0x1c, 51);
    ST_U8(e + 0x1d, 10);
    ST_U8(e + 0x1e, 0);
    ST_U8(e + 0x30, 0);
    ST_U8(e + 0x31, 2);
    ST_U8(e + 0x825, 0x30);
    ST_U8(e + 0x826, 0);
    ST_U8(e + 0x827, 0);
    ST_U32(e + 0x85c, 0);
    ST_U32(e + 0x860, 0);
    switch (LD_U8(e + 0x824)) {
    case 0:                 /* jump table 0x1e97ac */
        ST_U8(e + 0x82c, 30);
        ST_U8(e + 0x82d, 33);
        ST_U8(e + 0x82e, 33);
        break;
    case 1:
        ST_U32(e + 0x82c, 6);
        ST_U32(e + 0x830, 1000);
        ST_U8(e + 0x834, 2);
        ST_U8(e + 0x840, 24);
        ST_U8(e + 0x841, 1);
        ST_U8(e + 0x844, 6);
        ST_U8(e + 0x845, 3);
        ST_U8(e + 0x842, 51);
        ST_U8(e + 0x843, 10);
        ST_U8(e + 0x846, 0);
        break;
    case 2:
        ST_U32(e + 0x82c, 6);
        ST_U32(e + 0x830, 1000);
        ST_U8(e + 0x840, 80);
        ST_U8(e + 0x841, 24);
        ST_U8(e + 0x842, 1);
        ST_U8(e + 0x843, 6);
        ST_U8(e + 0x844, 3);
        ST_U8(e + 0x845, 0);
        ST_U8(e + 0x846, 45);
        ST_U8(e + 0x847, 20);
        break;
    case 3:
    case 5:
        ST_U32(e + 0x82c, 6);
        ST_U32(e + 0x830, 1000);
        ST_U8(e + 0x840, 80);
        ST_U8(e + 0x841, 24);
        ST_U8(e + 0x842, 1);
        ST_U8(e + 0x843, 6);
        ST_U8(e + 0x844, 3);
        ST_U8(e + 0x845, 0);
        ST_U8(e + 0x846, 50);
        ST_U8(e + 0x847, 35);
        ST_U8(e + 0x848, 45);
        ST_U8(e + 0x849, 20);
        ST_U8(e + 0x84a, 0);
        ST_U8(e + 0x84b, 2);
        ST_U8(e + 0x84c, 0);
        ST_U8(e + 0x84d, 5);
        ST_U8(e + 0x84e, 0);
        ST_U8(e + 0x84f, 20);
        break;
    case 4:
        ST_U32(e + 0x82c, 6);
        ST_U32(e + 0x830, 1500);
        ST_U32(e + 0x834, 1);
        ST_U32(e + 0x838, 1000);
        ST_U32(e + 0x83c, 500);
        ST_U8(e + 0x840, 80);
        ST_U8(e + 0x841, 24);
        ST_U8(e + 0x842, 1);
        ST_U8(e + 0x843, 6);
        ST_U8(e + 0x844, 3);
        ST_U8(e + 0x845, 0);
        ST_U16(e + 0x846, 5);
        ST_U16(e + 0x848, 60);
        ST_U8(e + 0x84a, 45);
        ST_U8(e + 0x84b, 20);
        break;
    }
    ST_U8(e + 0x850, 6);
    ST_U8(e + 0x851, 6);
    ST_U8(e + 0x829, 0);
    ST_U8(e + 0x82a, 2);
    ST_U8(e + 0x898, 0);
    ST_U8(e + 0x899, 1);
    ST_U32(e + 0x89c, 0x151800);
    ST_U32(e + 0x8a0, 2);
    ST_U8(e + 0x8a4, 2);
    ST_U32(e + 0x8a8, 0x10e000);
    ST_U32(e + 0x8ac, 0x87000);
    ST_U32(e + 0x8b0, 0x87000);
}
