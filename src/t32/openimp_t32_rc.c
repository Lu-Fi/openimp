/*
 * OpenIMP T32 rate control on HERA (#295, planned): the T32 rate-control
 * attributes on src/eprc (see openimp_t32_rc.h, docs/T32_RC.md).
 */
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"

/* i264e_param_default values the T23 front end uses for the fields a mode
 * has none of (docs/T23_EPRC.md) */
#define T32_RC_FRM_STEP     3u
#define T32_RC_GOP_STEP     15u
#define T32_RC_STATIC_TIME  2u
#define T32_RC_CHANGE_POS   80u
#define T32_RC_QUALITY      4u
#define T32_RC_MIN_KBPS     128u        /* T23 front end: VBR maxBitRate floor */

static int32_t clip(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* the common head of every T32 rate-controlled attribute: maxQp, minQp,
 * (blkQpEn,) initialQp, maxIQp, minIQp */
static void qp_range(uint32_t max_qp, uint32_t min_qp, EprcParams *p)
{
    if (max_qp > 51u)
        max_qp = 51u;
    if (min_qp > max_qp)
        min_qp = max_qp;
    p->min_qp = min_qp;
    p->max_qp = max_qp;
}

static void vbr_extras(uint32_t static_time, uint32_t change_pos, uint32_t quality, EprcParams *p)
{
    p->static_time = (int32_t)static_time <= 0 ? 1u : static_time;
    p->change_pos = (uint32_t)clip((int32_t)change_pos, 50, 100);
    p->quality = (uint32_t)clip((int32_t)quality, 0, 6);
}

int t32_rc_params(const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t gop,
                  uint32_t fps_num, uint32_t fps_den, EprcParams *p)
{
    uint32_t kbps;

    memset(p, 0, sizeof(*p));
    p->width = width;
    p->height = height;
    p->gop = gop;
    p->fps_num = fps_num;
    p->fps_den = fps_den;
    p->frm_qp_step = T32_RC_FRM_STEP;
    p->gop_qp_step = T32_RC_GOP_STEP;
    p->static_time = T32_RC_STATIC_TIME;
    p->change_pos = T32_RC_CHANGE_POS;
    p->quality = T32_RC_QUALITY;
    p->init_qp = -1;
    if (!width || !height || !gop || !fps_num || !fps_den)
        return -1;
    switch (m->rcMode) {
    case IMP_ENC_RC_MODE_CBR: {
        const IMPEncoderAttrH264CBR *a = &m->attrH264Cbr;

        p->rc_mode = EPRC_MODE_CBR;
        qp_range(a->maxQp, a->minQp, p);
        p->bitrate = p->max_bitrate = a->outBitRate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        if ((int32_t)a->staticTime > 0)
            p->static_time = a->staticTime;
        p->quality = (uint32_t)clip((int32_t)a->qualityLvl, 0, 6);
        break;
    }
    case IMP_ENC_RC_MODE_VBR: {
        const IMPEncoderAttrH264VBR *a = &m->attrH264Vbr;

        p->rc_mode = EPRC_MODE_VBR;
        qp_range(a->maxQp, a->minQp, p);
        p->max_bitrate = a->maxBitRate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        vbr_extras(a->staticTime, a->changePos, a->qualityLvl, p);
        break;
    }
    case IMP_ENC_RC_MODE_SMART: {
        const IMPEncoderAttrH264Smart *a = &m->attrH264Smart;

        p->rc_mode = EPRC_MODE_SMART;
        qp_range(a->maxQp, a->minQp, p);
        p->max_bitrate = a->maxBitRate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        vbr_extras(a->staticTime, a->changePos, a->qualityLvl, p);
        break;
    }
    case IMP_ENC_RC_MODE_CVBR: {
        /* constrained VBR: the short-term maximum is the VBR ceiling; the
         * long-term bounds (longMax/MinBitRate, longStatTime) are not
         * modelled by the T23 revision */
        const IMPEncoderAttrH264CVBR *a = &m->attrH264CVbr;

        p->rc_mode = EPRC_MODE_VBR;
        qp_range(a->maxQp, a->minQp, p);
        p->max_bitrate = a->maxBitRate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        vbr_extras(a->shortStatTime, a->changePos, a->qualityLvl, p);
        break;
    }
    case IMP_ENC_RC_MODE_AVBR: {
        /* adaptive VBR (lower rate in still scenes): the T23 SMART mode,
         * which lowers the budget of static GOPs */
        const IMPEncoderAttrH264AVBR *a = &m->attrH264AVbr;

        p->rc_mode = EPRC_MODE_SMART;
        qp_range(a->maxQp, a->minQp, p);
        p->max_bitrate = a->maxBitRate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        vbr_extras(a->staticTime, a->changePos, a->qualityLvl, p);
        break;
    }
    case IMP_ENC_RC_MODE_QVBR: {
        /* PRJ009/PRJ010 only in the vendor header; on PRJ007 OpenIMP runs
         * it as VBR at the target rate */
        const IMPEncoderAttrH264QVBR *a = &m->attrH264QVbr;

        p->rc_mode = EPRC_MODE_VBR;
        qp_range(a->maxQp, a->minQp, p);
        p->max_bitrate = a->targetBitrate;
        p->i_bias = clip(a->iBiasLvl, -10, 10);
        vbr_extras(a->staticTime, T32_RC_CHANGE_POS, T32_RC_QUALITY, p);
        break;
    }
    default:
        return -1;
    }
    kbps = p->rc_mode == EPRC_MODE_CBR ? p->bitrate : p->max_bitrate;
    if (!kbps)
        return -1;
    if (p->rc_mode != EPRC_MODE_CBR && p->max_bitrate < T32_RC_MIN_KBPS)
        p->max_bitrate = T32_RC_MIN_KBPS;
    return 0;
}

/* IMP attributes -> i264e parameter block, as vendor 2.2.0 makes it:
 * IMP_Encoder_CreateChn sizes the stream buffer (encAttr.bufSize, 0xdc574),
 * channel_i264e_encoder_init (0xd527c) copies the attribute fields into
 * i264e_param_default's block and i264e_validate_parameters (0x6c138,
 * called by i264e_init) clamps them.  The vendor keeps no "0 = default"
 * rule: a 0 goes in as 0 and only the clamps below lift it (static time,
 * statistics times 1, change position 50, still rate 1, ...); the one
 * exception is maxIPictureSize 0 = 1.5 x the rate.  The QP steps of a CBR
 * channel (IPfrmQPDelta / PPfrmQPDelta) 0 stay 0.  Checked field by field
 * against the vendor library (tools/eprc_t33_imp_oracle.py,
 * tests/t32/rc_imp_test.c). */
static int32_t clip3(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* c_reduce_fraction */
static void reduce_fraction(uint32_t *n, uint32_t *d)
{
    uint32_t a = *n, b = *d, r;

    if (!a || !b)
        return;
    r = a % b;
    while (r) {
        a = b;
        b = r;
        r = a % b;
    }
    *n /= b;
    *d /= b;
}

void t32_rc_vendor_rate(uint32_t *fps_num, uint32_t *fps_den, uint32_t *gop)
{
    reduce_fraction(fps_num, fps_den);
    if (!*fps_num || !*fps_den) {
        *fps_num = 25;
        *fps_den = 1;
    }
    /* the GOP becomes a multiple of the frame rate numerator */
    if ((*gop * *fps_den) % *fps_num)
        *gop = (*gop / *fps_num + 1u) * *fps_num;
    if ((int32_t)*gop < 1)
        *gop = (uint32_t)clip3((int32_t)*fps_num, 1, 1500);   /* i264e min / max keyint 1 / 1500 */
}

uint32_t t32_rc_vendor_bufsize(uint32_t width, uint32_t height, uint32_t bufsize)
{
    double px = (double)width * (double)height;

    if (!bufsize) {
        double v = px * 3.0 * 0.5 + 8192.0 + 128.0;

        bufsize = v < 2147483648.0 ? (uint32_t)(int32_t)v : (uint32_t)(int32_t)(v - 2147483648.0) | 0x80000000u;
    } else {
        /* at least what a picture needs (H.264, no IVDC) */
        float f = width < 801u && height < 601u ? 2.0f : 3.0f;
        int32_t min = (int32_t)(px * 3.0 * 0.5 / (double)f * 0.5 + 8192.0 + 128.0);

        if (min < 0x4100)
            min = 0x4100;
        if (bufsize < (uint32_t)min)
            bufsize = (uint32_t)min;
    }
    return (bufsize + 3u) & ~3u;
}

static void t33_head(T32Rc33 *c, const IMPEncoderAttrH264CBR *a)
{
    /* the six QP bytes sit at the same place in every rate-controlled mode */
    t32_rc33_set(c, T33P_MIN_QP, a->minQp);
    t32_rc33_set(c, T33P_MAX_QP, a->maxQp);
    t32_rc33_set8(c, 0xf4, a->blkQpEn);
    t32_rc33_set(c, T33P_INIT_QP, a->initialQp);
    t32_rc33_set(c, T33P_MAX_IQP, a->maxIQp);
    t32_rc33_set(c, T33P_MIN_IQP, a->minIQp);
}

static void t33_sizes(T32Rc33 *c, uint32_t kbps, uint32_t max_i, uint32_t max_p)
{
    t32_rc33_set(c, 0x158, max_i ? max_i : (kbps * 3u) >> 1);
    t32_rc33_set(c, 0x15c, max_p);
}

/* i264e_validate_parameters, the rate-control part */
static void t33_validate(T32Rc33 *c)
{
#define G(o) ((int32_t)t32_rc33_get(c, (o)))
#define S(o, v) t32_rc33_set(c, (o), (uint32_t)(v))
#define CL(o, lo, hi) S((o), clip3(G(o), (lo), (hi)))
    int32_t w = G(T33P_WIDTH), h = G(T33P_HEIGHT), area, lo, k = w < 0x321 ? 3 : 6;
    union { uint32_t u; float f; } r1, r2, qc;

    CL(0x118, 1, 0x40000000);
    CL(T33P_FIX_IQP, 0, 51);
    CL(T33P_FIX_PQP, 0, 51);
    CL(0xe8, 0, 51);
    CL(T33P_MAX_QP, 0, 51);
    CL(T33P_MIN_QP, 0, G(T33P_MAX_QP));
    CL(T33P_INIT_QP, 0, 51);
    CL(T33P_MAX_IQP, 0, 51);
    CL(T33P_MIN_IQP, 0, 51);
    CL(0x11c, -10, 10);
    CL(0x120, 0, 15);
    CL(0x124, 0, 15);
    CL(0x128, 2, 51);
    if (G(T33P_KBPS) < 0x80)
        S(T33P_KBPS, 0x80);
    if (G(0x130) < 1)
        S(0x130, 1);
    CL(0x134, 50, 100);
    CL(0x13c, 0, 8);
    CL(0x138, 0, 7);
    CL(0x150, 0, 100);
    CL(0x154, 0, G(0x150));
    CL(0x160, 1, 100);
    CL(0x164, 0, 51);
    CL(0x16c, 0, 10);
    CL(0x170, 0, 3);
    CL(0x174, 1, 100);
    if (G(0x180) < 1)
        S(0x180, 1);
    if (G(0x184) < 1)
        S(0x184, 1);
    CL(0x188, 0, 1000);
    qc.u = t32_rc33_get(c, 0x140);
    qc.f = (float)clip3((int32_t)qc.f, 2, 10);
    S(0x140, qc.u);
    CL(0x144, G(T33P_MAX_QP), 51);
    /* VBV: between 3/50 and 12/3 (12/6 above 800 pixels wide) bits per pixel */
    area = (int32_t)(((uint32_t)w + 15u) & ~15u) * (int32_t)(((uint32_t)h + 15u) & ~15u);
    lo = area * 3 / 50;
    CL(0x19c, 0, 3);
    CL(0x1a0, lo, area * 12 / k);
    r1.u = t32_rc33_get(c, 0x108);
    r2.u = t32_rc33_get(c, 0x10c);
    CL(0x1a4, lo, (int32_t)((float)G(0x1a0) / r1.f));
    CL(0x1a8, lo, (int32_t)((float)G(0x1a4) / r2.f));
    CL(0x114, 1, G(0x118));
#undef CL
#undef S
#undef G
}

int t32_rc33_params(T32Rc33 *c, const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t bufsize,
                    uint32_t gop, uint32_t fps_num, uint32_t fps_den)
{
    switch (m->rcMode) {
    case IMP_ENC_RC_MODE_FIXQP: {
        const IMPEncoderAttrH264FixQP *a = &m->attrH264FixQp;

        t32_rc33_set(c, T33P_MODE, 0);
        t32_rc33_set(c, T33P_FIX_IQP, a->IQp);
        t32_rc33_set(c, T33P_FIX_PQP, a->PQp);
        t32_rc33_set(c, 0xe8, a->BQp);
        t32_rc33_set8(c, 0xf4, a->blkQpEn);
        break;
    }
    case IMP_ENC_RC_MODE_CBR: {
        const IMPEncoderAttrH264CBR *a = &m->attrH264Cbr;

        t32_rc33_set(c, T33P_MODE, 1);
        t33_head(c, a);
        t32_rc33_set(c, T33P_KBPS, a->outBitRate);
        t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)a->iBiasLvl);
        t32_rc33_set(c, 0x120, a->IPfrmQPDelta);
        t32_rc33_set(c, 0x124, a->PPfrmQPDelta);
        t32_rc33_set(c, 0x130, a->staticTime);
        t32_rc33_set(c, 0x13c, a->flucLvl);
        t32_rc33_set(c, 0x150, a->maxIprop);
        t32_rc33_set(c, 0x154, a->minIprop);
        t33_sizes(c, a->outBitRate, a->maxIPictureSize, a->maxPPictureSize);
        /* qualityLvl is not handed on (the i264e default stays) */
        break;
    }
    case IMP_ENC_RC_MODE_VBR: {
        const IMPEncoderAttrH264VBR *a = &m->attrH264Vbr;

        t32_rc33_set(c, T33P_MODE, 2);
        t33_head(c, &m->attrH264Cbr);
        t32_rc33_set(c, T33P_KBPS, a->maxBitRate);
        t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)a->iBiasLvl);
        t32_rc33_set(c, 0x134, a->changePos);
        t32_rc33_set(c, 0x130, a->staticTime);
        t32_rc33_set(c, 0x138, a->qualityLvl);
        t32_rc33_set(c, 0x150, a->maxIprop);
        t32_rc33_set(c, 0x154, a->minIprop);
        t33_sizes(c, a->maxBitRate, a->maxIPictureSize, a->maxPPictureSize);
        break;
    }
    case IMP_ENC_RC_MODE_SMART: {
        const IMPEncoderAttrH264Smart *a = &m->attrH264Smart;

        t32_rc33_set(c, T33P_MODE, 3);
        t33_head(c, &m->attrH264Cbr);
        t32_rc33_set(c, T33P_KBPS, a->maxBitRate);
        t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)a->iBiasLvl);
        t32_rc33_set(c, 0x134, a->changePos);
        t32_rc33_set(c, 0x130, a->staticTime);
        t32_rc33_set(c, 0x138, a->qualityLvl);
        t32_rc33_set(c, 0x150, a->maxIprop);
        t32_rc33_set(c, 0x154, a->minIprop);
        t32_rc33_set(c, 0x160, a->minStillRate);
        t32_rc33_set(c, 0x164, a->maxStillQp);
        t32_rc33_set8(c, 0x168, a->superSmartEn);
        t32_rc33_set(c, 0x16c, a->supSmtStillLvl);
        t32_rc33_set(c, 0x170, a->supSmtStillRateLvl);
        t32_rc33_set(c, 0x174, a->maxSupSmtStillRate);
        t33_sizes(c, a->maxBitRate, a->maxIPictureSize, a->maxPPictureSize);
        break;
    }
    case IMP_ENC_RC_MODE_CVBR: {
        const IMPEncoderAttrH264CVBR *a = &m->attrH264CVbr;

        t32_rc33_set(c, T33P_MODE, 4);
        t33_head(c, &m->attrH264Cbr);
        t32_rc33_set(c, T33P_KBPS, a->maxBitRate);
        t32_rc33_set(c, T33P_LONG_MAX_KBPS, a->longMaxBitRate);
        t32_rc33_set(c, T33P_LONG_MIN_KBPS, a->longMinBitRate);
        t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)a->iBiasLvl);
        t32_rc33_set(c, 0x134, a->changePos);
        t32_rc33_set(c, 0x180, a->shortStatTime);
        t32_rc33_set(c, 0x184, a->longStatTime);
        t32_rc33_set(c, 0x138, a->qualityLvl);
        t32_rc33_set(c, 0x150, a->maxIprop);
        t32_rc33_set(c, 0x154, a->minIprop);
        t32_rc33_set(c, 0x188, a->extraBitRate);
        t33_sizes(c, a->maxBitRate, a->maxIPictureSize, a->maxPPictureSize);
        break;
    }
    case IMP_ENC_RC_MODE_AVBR: {
        const IMPEncoderAttrH264AVBR *a = &m->attrH264AVbr;

        t32_rc33_set(c, T33P_MODE, 5);
        t33_head(c, &m->attrH264Cbr);
        t32_rc33_set(c, T33P_KBPS, a->maxBitRate);
        t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)a->iBiasLvl);
        t32_rc33_set(c, 0x134, a->changePos);
        t32_rc33_set(c, 0x130, a->staticTime);
        t32_rc33_set(c, 0x138, a->qualityLvl);
        t32_rc33_set(c, 0x150, a->maxIprop);
        t32_rc33_set(c, 0x154, a->minIprop);
        t32_rc33_set(c, 0x160, a->minStillRate);
        t32_rc33_set(c, 0x164, a->maxStillQp);
        t33_sizes(c, a->maxBitRate, a->maxIPictureSize, a->maxPPictureSize);
        break;
    }
    default:
        return -1;              /* QVBR: the vendor refuses it on PRJ007 */
    }
    t32_rc_vendor_rate(&fps_num, &fps_den, &gop);
    t32_rc33_set(c, T33P_GOP, gop);
    t32_rc33_set(c, T33P_FPS_NUM, fps_num);
    t32_rc33_set(c, T33P_FPS_DEN, fps_den);
    /* the VBV size in bits: the stream buffer less 8320 bytes */
    t32_rc33_set(c, 0x1a0, (t32_rc_vendor_bufsize(width, height, bufsize) - 8320u) * 8u);
    /* the bitstream-full handling (g_encoder_bsfull_*: mode 1, count 2, on, 250) */
    t32_rc33_set(c, 0x2e4, 1);
    t32_rc33_set(c, 0x2e8, 2);
    t32_rc33_set8(c, 0x344, 1);
    t32_rc33_set(c, 0x348, 250);
    t33_validate(c);
    return 0;
}

/* IMP_Encoder_SetChnAttrRcMode on an H.264 channel (vendor 2.2.0): the
 * attribute goes through i264e_reconfig_rc_set (0x707f0) into the encoder's
 * reconfiguration copy of the parameters (seeded by i264e_reconfig_init
 * from the validated block), which i264e_idr_reconfig copies back at the
 * next IDR before i264e_validate_parameters and i264e_ratecontrol_init.
 * Fields the new mode does not carry keep their last value; the clamps are
 * i264e_reconfig_rc_set's.  Unlike the channel set-up a 0 maxIPictureSize
 * stays 0, the CBR qualityLvl is taken, blkQpEn becomes 0/1, and (vendor
 * code) the SMART supSmtStillRateLvl is taken from maxIprop clamped to 0..3. */
static void t33_runtime(uint32_t *s, const IMPEncoderAttrRcMode *m)
{
#define W(o, v) (s[(o) / 4] = (uint32_t)(v))
#define AT_LEAST1(v) ((int32_t)(v) > 0 ? (int32_t)(v) : 1)
    const IMPEncoderAttrH264CBR *h = &m->attrH264Cbr;  /* the common head */
    int32_t maxprop = 0;

    W(T33P_MODE, m->rcMode);
    if (m->rcMode == IMP_ENC_RC_MODE_FIXQP) {
        W(T33P_FIX_IQP, clip3((int32_t)m->attrH264FixQp.IQp, 0, 51));
        W(T33P_FIX_PQP, clip3((int32_t)m->attrH264FixQp.PQp, 0, 51));
        W(0xe8, clip3((int32_t)m->attrH264FixQp.BQp, 0, 51));
        W(0xf4, m->attrH264FixQp.blkQpEn);
        return;
    }
    W(T33P_MAX_QP, clip3(h->maxQp, 0, 51));
    W(T33P_MIN_QP, clip3(h->minQp, 0, 51));
    W(0xf4, h->blkQpEn);
    W(T33P_INIT_QP, clip3(h->initialQp, 0, 51));
    W(T33P_MAX_IQP, clip3(h->maxIQp, 0, 51));
    W(T33P_MIN_IQP, clip3(h->minIQp, 0, 51));
#define IPROP(a) do { maxprop = clip3((int32_t)(a)->maxIprop, 0, 100); W(0x150, maxprop); \
        W(0x154, clip3((int32_t)(a)->minIprop, 0, maxprop)); W(0x158, (a)->maxIPictureSize); \
        W(0x15c, (a)->maxPPictureSize); } while (0)
#define VBRS(a) do { W(T33P_KBPS, (a)->maxBitRate); W(0x11c, clip3((a)->iBiasLvl, -10, 10)); \
        W(0x130, AT_LEAST1((a)->staticTime)); W(0x134, clip3((int32_t)(a)->changePos, 50, 100)); \
        W(0x138, clip3((int32_t)(a)->qualityLvl, 0, 7)); IPROP(a); } while (0)
    switch (m->rcMode) {
    case IMP_ENC_RC_MODE_CBR:
        W(T33P_KBPS, h->outBitRate);
        W(0x11c, clip3(h->iBiasLvl, -10, 10));
        W(0x120, clip3(h->IPfrmQPDelta, 0, 15));
        W(0x124, clip3(h->PPfrmQPDelta, 0, 15));
        W(0x130, AT_LEAST1(h->staticTime));
        W(0x138, clip3((int32_t)h->qualityLvl, 0, 7));
        W(0x13c, clip3(h->flucLvl, 0, 8));
        IPROP(h);
        break;
    case IMP_ENC_RC_MODE_VBR:
        VBRS(&m->attrH264Vbr);
        break;
    case IMP_ENC_RC_MODE_SMART: {
        const IMPEncoderAttrH264Smart *a = &m->attrH264Smart;

        VBRS(a);
        W(0x160, clip3(a->minStillRate, 1, 100));
        W(0x164, clip3(a->maxStillQp, 0, 51));
        s[0x168 / 4] = (s[0x168 / 4] & ~0xffu) | a->superSmartEn;
        W(0x16c, clip3(a->supSmtStillLvl, 0, 10));
        W(0x170, clip3((int32_t)a->maxIprop, 0, 3));
        W(0x174, clip3(a->maxSupSmtStillRate, 1, 100));
        break;
    }
    case IMP_ENC_RC_MODE_CVBR: {
        const IMPEncoderAttrH264CVBR *a = &m->attrH264CVbr;

        W(T33P_KBPS, a->maxBitRate);
        W(T33P_LONG_MAX_KBPS, a->longMaxBitRate);
        W(T33P_LONG_MIN_KBPS, a->longMinBitRate);
        W(0x11c, clip3(a->iBiasLvl, -10, 10));
        W(0x134, clip3((int32_t)a->changePos, 50, 100));
        W(0x180, AT_LEAST1(a->shortStatTime));
        W(0x184, AT_LEAST1(a->longStatTime));
        W(0x138, clip3((int32_t)a->qualityLvl, 0, 7));
        W(0x188, clip3(a->extraBitRate, 0, 1000));
        IPROP(a);
        break;
    }
    case IMP_ENC_RC_MODE_AVBR: {
        const IMPEncoderAttrH264AVBR *a = &m->attrH264AVbr;

        VBRS(a);
        W(0x160, clip3(a->minStillRate, 1, 100));
        W(0x164, clip3(a->maxStillQp, 0, 51));
        break;
    }
    default:
        break;
    }
#undef VBRS
#undef IPROP
#undef AT_LEAST1
#undef W
}

/* IMP_Encoder_SetDefaultParam, rate-control part (vendor 2.2.0, the same
 * for H.264 and H.265): the full set of mode defaults; QPs are bytes (the
 * initial QP as given, -1 = 0xff), picture size caps 6/5 and 18/25 of the
 * rate in the vendor's 32-bit arithmetic. */
void t32_rc_default_attr(IMPEncoderAttrRcMode *rc, IMPPayloadType type, int initqp, uint32_t kbps)
{
    IMPEncoderRcMode mode = rc->rcMode;
    uint32_t ipic = kbps * 6u / 5u, ppic = ipic * 3u / 5u;
    uint8_t iqp = (uint8_t)initqp;

    memset(rc, 0, sizeof(*rc));
    rc->rcMode = mode;
    if (type == PT_JPEG) {
        rc->attrJPEGFixQp.qp = (uint32_t)initqp;
        return;
    }
    switch (mode) {
    case IMP_ENC_RC_MODE_FIXQP:
        rc->attrH264FixQp.IQp = rc->attrH264FixQp.PQp = (uint32_t)initqp;
        return;
    case IMP_ENC_RC_MODE_CBR: {
        IMPEncoderAttrH264CBR *a = &rc->attrH264Cbr;

        a->maxQp = a->maxIQp = 45;
        a->minQp = a->minIQp = 15;
        a->initialQp = iqp;
        a->outBitRate = kbps;
        a->IPfrmQPDelta = a->PPfrmQPDelta = 8;
        a->staticTime = 2;
        a->flucLvl = 2;
        a->qualityLvl = 3;
        a->maxIprop = 100;
        a->minIprop = 1;
        a->maxIPictureSize = ipic;
        a->maxPPictureSize = ppic;
        return;
    }
    case IMP_ENC_RC_MODE_CVBR: {
        IMPEncoderAttrH264CVBR *a = &rc->attrH264CVbr;

        a->maxQp = a->maxIQp = 45;
        a->minQp = a->minIQp = 15;
        a->initialQp = iqp;
        a->maxBitRate = kbps;
        a->longMaxBitRate = kbps * 4u / 5u;
        a->longMinBitRate = kbps * 3u / 5u;
        a->changePos = 80;
        a->shortStatTime = 2;
        a->longStatTime = 60;
        a->qualityLvl = 6;
        a->maxIprop = 100;
        a->minIprop = 1;
        a->extraBitRate = 5;
        a->maxIPictureSize = ipic;
        a->maxPPictureSize = ppic;
        return;
    }
    case IMP_ENC_RC_MODE_VBR:
    case IMP_ENC_RC_MODE_SMART:
    case IMP_ENC_RC_MODE_AVBR: {
        /* VBR, SMART and AVBR share the head up to minIprop */
        IMPEncoderAttrH264VBR *a = &rc->attrH264Vbr;

        a->maxQp = a->maxIQp = 45;
        a->minQp = a->minIQp = 15;
        a->initialQp = iqp;
        a->maxBitRate = kbps;
        a->changePos = 80;
        a->staticTime = 2;
        a->qualityLvl = 6;
        a->maxIprop = 100;
        a->minIprop = 1;
        if (mode == IMP_ENC_RC_MODE_VBR) {
            a->maxIPictureSize = ipic;
            a->maxPPictureSize = ppic;
        } else if (mode == IMP_ENC_RC_MODE_SMART) {
            IMPEncoderAttrH264Smart *s = &rc->attrH264Smart;

            s->minStillRate = 25;
            s->maxStillQp = 35;
            s->supSmtStillLvl = 5;
            s->supSmtStillRateLvl = 2;
            s->maxSupSmtStillRate = 20;
            s->maxIPictureSize = ipic;
            s->maxPPictureSize = ppic;
        } else {
            IMPEncoderAttrH264AVBR *v = &rc->attrH264AVbr;

            v->minStillRate = 25;
            v->maxStillQp = 35;
            v->maxIPictureSize = ipic;
            v->maxPPictureSize = ppic;
        }
        return;
    }
    default:
        return;
    }
}

void t32_rc_free(T32Rc *rc)
{
    if (rc->t33) {
        t32_rc33_close(rc->t33);
        free(rc->t33);
        rc->t33 = NULL;
    } else if (rc->on) {
        EPRC_Free(&rc->eprc);
    }
    free(rc->slice);
    rc->slice = NULL;
    rc->on = 0;
}

int t32_rc_init(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t pic_w, uint32_t pic_h, uint32_t bufsize,
                uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc)
{
    uint32_t width = (pic_w + 15u) & ~15u, height = (pic_h + 15u) & ~15u;
    EprcParams p;

    t32_rc_free(rc);
    memset(rc, 0, sizeof(*rc));
    rc->hevc = hevc;
    rc->mode = (int)m->rcMode;
    rc->i_min = rc->i_max = -1;
    if (!hevc && m->rcMode != IMP_ENC_RC_MODE_QVBR) {
        /* H.264: the vendor's i264e set-up, whatever the attribute holds */
        T32Rc33 *c = calloc(1, sizeof(*c));

        if (!c || t32_rc33_open(c, width, height) != 0) {
            free(c);
            return -1;
        }
        if (t32_rc33_params(c, m, pic_w, pic_h, bufsize, gop, fps_num, fps_den) != 0) {
            t32_rc33_close(c);
            free(c);
            return 0;
        }
        /* i264e_reconfig_init: the copy SetChnAttrRcMode works on */
        for (unsigned o = 0; o < T32_RC_PWORDS; o++)
            rc->stash[o] = t32_rc33_get(c, 4u * o);
        rc->stash[0xf4 / 4] &= 0xffu;
        rc->have_stash = 1;
        rc->width = width;
        rc->height = height;
        if (m->rcMode == IMP_ENC_RC_MODE_FIXQP) {
            t32_rc33_close(c);
            free(c);
            return 0;
        }
        t32_rc33_init(c);
        if (t32_rc_params(m, width, height, gop, fps_num, fps_den, &p) == 0)
            rc->params = p;     /* for the log line only */
        rc->t33 = c;
        rc->min_qp = (int)t32_rc33_get(c, T33P_MIN_QP);
        rc->max_qp = (int)t32_rc33_get(c, T33P_MAX_QP);
        rc->on = 1;
        return 0;
    }
    if (t32_rc_params(m, width, height, gop, fps_num, fps_den, &p) != 0)
        return 0;
    /* maxIQp / minIQp sit at the same place in every rate-controlled
     * attribute but QVBR (which has them one byte earlier) */
    {
        uint32_t imax = m->rcMode == IMP_ENC_RC_MODE_QVBR ? m->attrH264QVbr.maxIQp : m->attrH264Cbr.maxIQp;
        uint32_t imin = m->rcMode == IMP_ENC_RC_MODE_QVBR ? m->attrH264QVbr.minIQp : m->attrH264Cbr.minIQp;

        if (imax > 0u && imax <= 51u && imin <= imax) {
            rc->i_min = (int)imin;
            rc->i_max = (int)imax;
        }
    }
    rc->slice = calloc(1, EPRC_SLICE_SIZE);
    if (!rc->slice)
        return -1;
    if (EPRC_Init(&rc->eprc, &p, rc->slice) != 0) {
        free(rc->slice);
        rc->slice = NULL;
        return -1;
    }
    rc->params = p;
    rc->min_qp = (int)p.min_qp;
    rc->max_qp = (int)p.max_qp;
    rc->on = 1;
    return 0;
}

int t32_rc_reconfig(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t pic_w, uint32_t pic_h, uint32_t bufsize,
                    uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc)
{
    uint32_t stash[T32_RC_PWORDS], width = rc->width, height = rc->height;
    T32Rc33 *c;

    if (hevc || !rc->have_stash || rc->hevc || m->rcMode > IMP_ENC_RC_MODE_AVBR)
        return t32_rc_init(rc, m, pic_w, pic_h, bufsize, gop, fps_num, fps_den, hevc);
    memcpy(stash, rc->stash, sizeof(stash));
    t33_runtime(stash, m);
    t32_rc_free(rc);
    memset(rc, 0, sizeof(*rc));
    memcpy(rc->stash, stash, sizeof(stash));
    rc->have_stash = 1;
    rc->width = width;
    rc->height = height;
    rc->mode = (int)m->rcMode;
    rc->i_min = rc->i_max = -1;
    if (m->rcMode == IMP_ENC_RC_MODE_FIXQP)
        return 0;
    c = calloc(1, sizeof(*c));
    if (!c || t32_rc33_open(c, width, height) != 0) {
        free(c);
        return -1;
    }
    /* i264e_idr_reconfig: the copy back (blkQpEn as 0/1), then the clamps */
    for (unsigned o = 0; o < T32_RC_PWORDS; o++)
        t32_rc33_set(c, 4u * o, stash[o]);
    t32_rc33_set(c, 0xf4, stash[0xf4 / 4] != 0);
    t33_validate(c);
    t32_rc33_init(c);
    rc->t33 = c;
    rc->min_qp = (int)t32_rc33_get(c, T33P_MIN_QP);
    rc->max_qp = (int)t32_rc33_get(c, T33P_MAX_QP);
    rc->on = 1;
    return 0;
}

int t32_rc_start(T32Rc *rc, int idr, uint32_t gop_pos)
{
    EprcFrameIn in;
    EprcPicture pic;
    int qp;

    if (!rc->on)
        return -1;
    if (rc->t33) {
        /* the controller clamps the IDR QP to maxIQp / minIQp itself */
        qp = t32_rc33_start(rc->t33, idr ? 0u : gop_pos);
        rc->qp = clip(qp < 0 ? rc->t33->qp : qp, 0, 51);
        rc->idr = idr;
        return rc->qp;
    }
    memset(&in, 0, sizeof(in));
    in.frames_since_idr = idr ? 0u : gop_pos;
    if (EPRC_FrameStart(&rc->eprc, &in, &pic) != 0)
        return -1;
    qp = pic.qp;
    if (idr && rc->i_max >= 0)
        qp = clip(qp, rc->i_min, rc->i_max);
    rc->qp = clip(qp, 0, 51);
    rc->idr = idr;
    return rc->qp;
}

unsigned t32_rc_idr_rows(T32Rc *rc)
{
    return rc->on && rc->t33 ? t32_rc33_idr_rows(rc->t33) : 0u;
}

void t32_rc_end(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats)
{
    t32_rc_end_rows(rc, bytes, stats, nstats, NULL, 0);
}

void t32_rc_end_rows(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats, const uint32_t *rows,
                     unsigned nrows)
{
    uint32_t regs[EPRC_STAT_REGS];
    EprcPicture pic;

    if (!rc->on)
        return;
    if (rc->t33) {
        uint32_t st[T33_RC_STATS];

        memset(st, 0, sizeof(st));
        if (stats)
            memcpy(st, stats, (nstats < T33_RC_STATS ? nstats : T33_RC_STATS) * sizeof(st[0]));
        t32_rc33_end_rows(rc->t33, bytes, st, rows, nrows);
        rc->pictures++;
        rc->bits += (uint64_t)bytes * 8u;
        return;
    }
    memset(regs, 0, sizeof(regs));
    if (stats)
        memcpy(regs, stats, (nstats < EPRC_STAT_REGS ? nstats : EPRC_STAT_REGS) * sizeof(regs[0]));
    (void)EPRC_FrameEndEx(&rc->eprc, bytes, regs, &pic, 0);
    rc->pictures++;
    rc->bits += (uint64_t)bytes * 8u;
}

const uint8_t *t32_rc_slice(T32Rc *rc)
{
    return rc->on && rc->t33 ? t32_rc33_slice(rc->t33) : NULL;
}

const uint8_t *t32_rc_map(T32Rc *rc, int which, uint32_t *len)
{
    *len = 0;
    return rc->on && rc->t33 ? t32_rc33_map(rc->t33, which, len) : NULL;
}
