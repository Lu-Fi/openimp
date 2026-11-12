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

/* IMP attributes -> i264e parameter block.  The placement follows the
 * i264e defaults and the controller clamps they end in (i264e_ratecontrol_init
 * copies P+0x130.. to the controller block); the vendor's IMP -> i264e layer
 * (IMP_Encoder_SetChnAttrRcMode, i264e_reconfig_rc_set, i264e_idr_reconfig)
 * is not checked against it yet.  A 0 in a field whose valid range starts
 * above 0 (I share, change position, statistics times, picture sizes, still
 * rate / QP) or that would freeze the QP (the I/P and P/P QP steps) keeps the
 * i264e default, so channels set up with only the
 * basic fields (IMP_Encoder_SetDefaultParam) run on the vendor defaults. */
static void t33_set_nz(T32Rc33 *c, uint32_t off, uint32_t v)
{
    if (v)
        t32_rc33_set(c, off, v);
}

static void t33_head(T32Rc33 *c, uint8_t max_qp, uint8_t min_qp, uint8_t init_qp, uint8_t max_iqp, uint8_t min_iqp)
{
    if (max_qp > 51u)
        max_qp = 51u;
    if (min_qp > max_qp)
        min_qp = max_qp;
    t32_rc33_set(c, T33P_MAX_QP, max_qp);
    t32_rc33_set(c, T33P_MIN_QP, min_qp);
    t32_rc33_set(c, T33P_INIT_QP, init_qp > 0u && init_qp <= 51u ? init_qp : 0xffffffffu);
    if (max_iqp > 0u && max_iqp <= 51u && min_iqp <= max_iqp) {
        t32_rc33_set(c, T33P_MAX_IQP, max_iqp);
        t32_rc33_set(c, T33P_MIN_IQP, min_iqp);
    } else {
        t32_rc33_set(c, T33P_MAX_IQP, max_qp);
        t32_rc33_set(c, T33P_MIN_IQP, min_qp);
    }
}

static void t33_vbr(T32Rc33 *c, uint32_t kbps, int8_t ibias, uint32_t change_pos, uint32_t static_time,
                    uint32_t quality, uint32_t max_iprop, uint32_t min_iprop, uint32_t max_ipic, uint32_t max_ppic)
{
    t32_rc33_set(c, T33P_KBPS, kbps);
    t32_rc33_set(c, 0x11c, (uint32_t)(int32_t)ibias);
    t33_set_nz(c, 0x134, change_pos);
    t33_set_nz(c, 0x130, static_time);
    t32_rc33_set(c, 0x138, quality);
    t33_set_nz(c, 0x150, max_iprop);
    t33_set_nz(c, 0x154, min_iprop);
    t33_set_nz(c, 0x158, max_ipic);
    t33_set_nz(c, 0x15c, max_ppic);
}

int t32_rc33_params(T32Rc33 *c, const IMPEncoderAttrRcMode *m, uint32_t gop, uint32_t fps_num, uint32_t fps_den)
{
    if (!gop || !fps_num || !fps_den)
        return -1;
    t32_rc33_set(c, T33P_GOP, gop);
    t32_rc33_set(c, T33P_FPS_NUM, fps_num);
    t32_rc33_set(c, T33P_FPS_DEN, fps_den);
    switch (m->rcMode) {
    case IMP_ENC_RC_MODE_CBR: {
        const IMPEncoderAttrH264CBR *a = &m->attrH264Cbr;

        t32_rc33_set(c, T33P_MODE, 1);
        t33_head(c, a->maxQp, a->minQp, a->initialQp, a->maxIQp, a->minIQp);
        t33_vbr(c, a->outBitRate, a->iBiasLvl, 80, a->staticTime, a->qualityLvl, a->maxIprop, a->minIprop,
                a->maxIPictureSize, a->maxPPictureSize);
        t32_rc33_set(c, 0x13c, a->flucLvl);
        t33_set_nz(c, 0x120, a->IPfrmQPDelta);
        t33_set_nz(c, 0x124, a->PPfrmQPDelta);
        break;
    }
    case IMP_ENC_RC_MODE_VBR: {
        const IMPEncoderAttrH264VBR *a = &m->attrH264Vbr;

        t32_rc33_set(c, T33P_MODE, 2);
        t33_head(c, a->maxQp, a->minQp, a->initialQp, a->maxIQp, a->minIQp);
        t33_vbr(c, a->maxBitRate, a->iBiasLvl, a->changePos, a->staticTime, a->qualityLvl, a->maxIprop,
                a->minIprop, a->maxIPictureSize, a->maxPPictureSize);
        break;
    }
    case IMP_ENC_RC_MODE_SMART: {
        const IMPEncoderAttrH264Smart *a = &m->attrH264Smart;

        t32_rc33_set(c, T33P_MODE, 3);
        t33_head(c, a->maxQp, a->minQp, a->initialQp, a->maxIQp, a->minIQp);
        t33_vbr(c, a->maxBitRate, a->iBiasLvl, a->changePos, a->staticTime, a->qualityLvl, a->maxIprop,
                a->minIprop, a->maxIPictureSize, a->maxPPictureSize);
        t33_set_nz(c, 0x160, a->minStillRate);
        t33_set_nz(c, 0x164, a->maxStillQp);
        t32_rc33_set8(c, 0x168, a->superSmartEn);
        t32_rc33_set(c, 0x16c, a->supSmtStillLvl);
        t32_rc33_set(c, 0x170, a->supSmtStillRateLvl);
        t32_rc33_set(c, 0x174, a->maxSupSmtStillRate);
        break;
    }
    case IMP_ENC_RC_MODE_CVBR: {
        const IMPEncoderAttrH264CVBR *a = &m->attrH264CVbr;

        t32_rc33_set(c, T33P_MODE, 4);
        t33_head(c, a->maxQp, a->minQp, a->initialQp, a->maxIQp, a->minIQp);
        t33_vbr(c, a->maxBitRate, a->iBiasLvl, a->changePos, 2, a->qualityLvl, a->maxIprop, a->minIprop,
                a->maxIPictureSize, a->maxPPictureSize);
        t32_rc33_set(c, T33P_LONG_MAX_KBPS, a->longMaxBitRate);
        t32_rc33_set(c, T33P_LONG_MIN_KBPS, a->longMinBitRate);
        t33_set_nz(c, 0x180, a->shortStatTime);
        t33_set_nz(c, 0x184, a->longStatTime);
        t32_rc33_set(c, 0x188, a->extraBitRate);
        break;
    }
    case IMP_ENC_RC_MODE_AVBR: {
        const IMPEncoderAttrH264AVBR *a = &m->attrH264AVbr;

        t32_rc33_set(c, T33P_MODE, 5);
        t33_head(c, a->maxQp, a->minQp, a->initialQp, a->maxIQp, a->minIQp);
        t33_vbr(c, a->maxBitRate, a->iBiasLvl, a->changePos, a->staticTime, a->qualityLvl, a->maxIprop,
                a->minIprop, a->maxIPictureSize, a->maxPPictureSize);
        t33_set_nz(c, 0x160, a->minStillRate);
        t33_set_nz(c, 0x164, a->maxStillQp);
        break;
    }
    default:
        return -1;              /* FIXQP: no controller; QVBR: T23 revision (PRJ009/PRJ010 only) */
    }
    return 0;
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

int t32_rc_init(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height,
                uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc)
{
    EprcParams p;

    t32_rc_free(rc);
    memset(rc, 0, sizeof(*rc));
    rc->hevc = hevc;
    rc->mode = (int)m->rcMode;
    rc->i_min = rc->i_max = -1;
    if (t32_rc_params(m, width, height, gop, fps_num, fps_den, &p) != 0)
        return 0;
    if (!hevc && m->rcMode != IMP_ENC_RC_MODE_QVBR) {
        T32Rc33 *c = calloc(1, sizeof(*c));

        if (!c || t32_rc33_open(c, width, height) != 0) {
            free(c);
            return -1;
        }
        if (t32_rc33_params(c, m, gop, fps_num, fps_den) != 0) {
            t32_rc33_close(c);
            free(c);
            return 0;
        }
        t32_rc33_init(c);
        rc->t33 = c;
        rc->params = p;
        rc->min_qp = (int)t32_rc33_get(c, T33P_MIN_QP);
        rc->max_qp = (int)t32_rc33_get(c, T33P_MAX_QP);
        rc->on = 1;
        return 0;
    }
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

void t32_rc_end(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats)
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
        t32_rc33_end(rc->t33, bytes, st);
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
