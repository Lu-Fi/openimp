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

void t32_rc_free(T32Rc *rc)
{
    if (rc->on)
        EPRC_Free(&rc->eprc);
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
    memset(regs, 0, sizeof(regs));
    if (stats)
        memcpy(regs, stats, (nstats < EPRC_STAT_REGS ? nstats : EPRC_STAT_REGS) * sizeof(regs[0]));
    (void)EPRC_FrameEndEx(&rc->eprc, bytes, regs, &pic, 0);
    rc->pictures++;
    rc->bits += (uint64_t)bytes * 8u;
}
