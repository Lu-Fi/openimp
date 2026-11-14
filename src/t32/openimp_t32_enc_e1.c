/*
 * OpenIMP T32/T33 encoder parameter calls of batches E1a / E1b / E2 (#480, planned).
 *
 * Return codes, argument checks and read-backs are those of vendor libimp 2.0.2.1 on the T33 (QEMU
 * -M ingenic-t33, apitest case t33e1, audit issue480/runs); the T32 2.2.0 entry points are the same.
 * What reaches the stream:
 *   - SetH264Vui / SetH265Vui: video signal type, aspect ratio and bitstream restriction in the next
 *     IDR's SPS, byte for byte as the vendor writes them; the getters show the new values from that IDR on
 *   - InsertUserData: SEI user_data_unregistered NAL with the vendor's UUID before the slice of the next
 *     pictures, one per picture, at most maxUserDataCnt queued of at most maxUserDataSize bytes
 * Stored and read back like the vendor (deferred to the next IDR where the vendor defers), NOT applied to
 * the HERA engine (no command-list oracle for them yet): TransCfg (chroma QP offsets), SuperFrameCfg, Crop,
 * Denoise, HSkip, ROI / MapRoi, MbRC, QpgMode, Srd, ChangeRef, Color2Grey, GDR, Pskip, RcTrigLevel,
 * MotionQuality, QpgAI, pad frames.
 * Built with the vendor 2.x headers (their types); the channel table is openimp_t32_encoder.c's
 * (OpenIMP_T32_EncChnQuery; the encoder calls OpenIMP_T32_E1Reset / _E1AtIdr / _E1Sei).
 * Lock order: this unit's lock, then the encoder's.
 */
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

#include "t32/openimp_t32_enc_e1.h"

#define T32_ENC_ERR_CHN     ((int)0x80040001)
#define T32_ENC_ERR_PARAM   ((int)0x80040002)
#define T32_ENC_ERR_NOCHN   ((int)0x80040008)
#define T32_ENC_ERR_MODE    ((int)0x80040080)
#define E1_ERR_NULL         ((int)0x80040010)
#define E1_ERR_FULL         ((int)0x80040800)
#define E1_ERR_OFF          ((int)0x80048000)
#define ALIGN16(x) (((x) + 15u) & ~15u)
#define T32_E1_UD_MAX   2                /* user-data queue entries (maxUserDataCnt: vendor range 0..2) */
#define T32_E1_UD_BYTES 1024             /* bytes per entry (maxUserDataSize: vendor 16..1024) */
#define T32_E1_CHNS     16

int OpenIMP_T32_EncMotionQualityOn(int encChn);  /* openimp_t32_fs_enc_ext.c: SetMotionQualityEnable */
int OpenIMP_T32_EncQpgAIOn(int encChn);          /* SetChnQpgAIEnable */

struct t32_e1 {
    T32Vui vui;                          /* the next IDR's VUI */
    int vui_pend;
    IMPEncoderH264Vui v4, v4_pend;       /* getter images: in effect / from the next IDR */
    IMPEncoderH265Vui v5, v5_pend;
    int trans264, trans264_pend;
    IMPEncoderH265TransCfg trans265, trans265_pend;
    IMPEncoderSuperFrmCfg sf, sf_pend;
    uint32_t sf_max;
    IMPEncoderCropCfg crop, crop_pend;
    int qpg, qpg_pend;
    IMPEncoderSrdCfg srd, srd_pend;
    int changeref, grey, mbrc, pskip_req, gdr_req, pad, trig_mode;
    uint32_t trig_level;
    uint8_t pad_attr[32];
    IMPEncoderGDRCfg gdr;
    IMPEncoderPskipCfg pskip;
    int dn_type, dn_iqp, dn_pqp;
    IMPEncoderAttrHSkip hskip;
    IMPEncoderROICfg roi[IMP_ENC_ROI_WIN_COUNT], roi_pend[IMP_ENC_ROI_WIN_COUNT];
    IMPEncoderMotionQuality mq;
    uint32_t ud_cnt, ud_size, ud_head, ud_n, ud_len[T32_E1_UD_MAX];
    uint8_t ud[T32_E1_UD_MAX][T32_E1_UD_BYTES];
};

static pthread_mutex_t e1_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t32_e1 e1_tab[T32_E1_CHNS];
#define E1 e1_tab[encChn]

static const uint8_t e1_uuid264[16] = { 0xd7, 0x3e, 0xba, 0x3d, 0xe6, 0xa6, 0x4c, 0x80,
                                        0x93, 0x79, 0x64, 0x0b, 0x42, 0xf2, 0xb8, 0x66 };
static const uint8_t e1_uuid265[16] = { 0x2c, 0xa2, 0xde, 0x09, 0xb5, 0x17, 0x47, 0xdb,
                                        0xbb, 0x55, 0xa4, 0xfe, 0x7f, 0xc2, 0xfc, 0x4e };

/* 0, 0x80040001 for a channel of 16 and more, 0x80040008 when not created */
static int e1_get(int encChn, T32EncChnInfo *ci)
{
    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    memset(ci, 0, sizeof(*ci));
    return OpenIMP_T32_EncChnQuery(encChn, ci) == 0 ? 0 : T32_ENC_ERR_NOCHN;
}

/* CreateChn: the vendor i264e / i265e defaults the getters read back */
void OpenIMP_T32_E1Reset(int encChn, const T32EncChnInfo *ci)
{
    struct t32_e1 *x;
    uint32_t h16, w;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return;
    h16 = ALIGN16(ci->height);
    w = ci->width;
    pthread_mutex_lock(&e1_lock);
    x = &E1;
    memset(x, 0, sizeof(*x));
    x->hskip.n = 1;
    if (ci->codec == 2)
        x->hskip.maxSameSceneCnt = 1;
    x->mbrc = x->changeref = ci->codec == 1;
    /* super frames: I threshold = (bufSize - 8320) * 8, P = I / 1.4, B = P / 1.3; mode none, RDO, 1 re-encode */
    x->sf.superIFrmBitsThr = (ci->codec == 2 ? 2u : 4u) * w * h16;
    x->sf.superPFrmBitsThr = (uint32_t)((uint64_t)x->sf.superIFrmBitsThr * 10u / 14u);
    x->sf.superBFrmBitsThr = (uint32_t)((uint64_t)x->sf.superPFrmBitsThr * 10u / 13u);
    x->sf.maxReEncodeTimes = 1;
    x->sf_max = x->sf.superIFrmBitsThr;
    x->sf_pend = x->sf;
    /* VUI getter images: timing from the channel's frame rate, video format 5, colour 2 / 2 / 2 */
    x->v4.vuiTimeInfo.timing_info_present_flag = 1;
    x->v4.vuiTimeInfo.fixed_frame_rate_flag = 1;
    x->v4.vuiTimeInfo.num_units_in_tick = ci->fps_den ? ci->fps_den : 1u;
    x->v4.vuiTimeInfo.time_scale = 2u * ci->fps_num;
    x->v4.vuiVideoSignal.video_format = 5;
    x->v4.vuiVideoSignal.colour_primaries = x->v4.vuiVideoSignal.transfer_characteristics =
        x->v4.vuiVideoSignal.matrix_coefficients = 2;
    x->v5.vuiTimeInfo.timing_info_present_flag = 1;
    x->v5.vuiTimeInfo.num_units_in_tick = x->v4.vuiTimeInfo.num_units_in_tick;
    x->v5.vuiTimeInfo.time_scale = ci->fps_num;
    x->v5.vuiVideoSignal = x->v4.vuiVideoSignal;
    x->vui.format = 5;
    x->ud_cnt = ci->ud_cnt > T32_E1_UD_MAX ? T32_E1_UD_MAX : ci->ud_cnt;
    x->ud_size = ci->ud_size > T32_E1_UD_BYTES ? T32_E1_UD_BYTES : ci->ud_size;
    pthread_mutex_unlock(&e1_lock);
}

/* an IDR (encoder thread): the deferred settings take effect; 1 = new VUI in *vui */
int OpenIMP_T32_E1AtIdr(int encChn, T32Vui *vui)
{
    struct t32_e1 *x;
    int v;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    x = &E1;
    v = x->vui_pend;
    x->vui_pend = 0;
    if (v) {
        *vui = x->vui;
        x->v4 = x->v4_pend;
        x->v5 = x->v5_pend;
    }
    x->trans264 = x->trans264_pend;
    x->trans265 = x->trans265_pend;
    x->sf = x->sf_pend;
    x->crop = x->crop_pend;
    x->qpg = x->qpg_pend;
    x->srd = x->srd_pend;
    memcpy(x->roi, x->roi_pend, sizeof(x->roi));
    pthread_mutex_unlock(&e1_lock);
    return v;
}

/* the next queued user-data SEI NAL (start code included, escaped) at @out: bytes, 0 = none */
size_t OpenIMP_T32_E1Sei(int encChn, int hevc, uint8_t *out, size_t cap)
{
    struct t32_e1 *x;
    uint8_t raw[T32_E1_UD_BYTES + 32], *p = raw;
    uint32_t len, size;
    size_t n;
    int esc = 0;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    x = &E1;
    if (!x->ud_n || cap < 8u + (16u + T32_E1_UD_BYTES + 8u) * 3u / 2u) {
        pthread_mutex_unlock(&e1_lock);
        return 0;
    }
    len = x->ud_len[x->ud_head];
    *p++ = 5;                                       /* user_data_unregistered */
    for (size = 16u + len; size >= 255u; size -= 255u)
        *p++ = 0xff;
    *p++ = (uint8_t)size;
    memcpy(p, hevc ? e1_uuid265 : e1_uuid264, 16);
    p += 16;
    memcpy(p, x->ud[x->ud_head], len);
    p += len;
    *p++ = 0x80;                                    /* rbsp trailing bits */
    x->ud_head = (x->ud_head + 1u) % T32_E1_UD_MAX;
    x->ud_n--;
    pthread_mutex_unlock(&e1_lock);
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    n = 4;
    if (hevc) {
        out[n++] = 39u << 1;                        /* PREFIX_SEI, layer 0, temporal id 1 */
        out[n++] = 1;
    } else {
        out[n++] = 6;
    }
    return n + t32_h264_escape(out + n, raw, (size_t)(p - raw), &esc);
}

/* ---- VUI (GOP: openimp_t32_encoder.c) ---------------------------------------------------------- */

int IMP_Encoder_SetH264Vui(int encChn, const IMPEncoderH264Vui *v)
{
    T32EncChnInfo ci;
    struct t32_e1 *x;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!v)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1) {
        x = &E1;
        if (!x->vui_pend) {
            x->v4_pend = x->v4;
            x->v5_pend = x->v5;
        }
        /* vendor: the SAR (no idc), the video signal fields only while its flag is set, the restriction */
        x->vui.sar_w = v->vuiAspectRatio.sar_width;
        x->vui.sar_h = v->vuiAspectRatio.sar_height;
        if (v->vuiVideoSignal.video_signal_type_present_flag) {
            x->vui.format = v->vuiVideoSignal.video_format & 7u;
            x->vui.full = v->vuiVideoSignal.video_full_range_flag ? 1 : 0;
        }
        x->vui.signal = x->vui.format != 5 || x->vui.full;
        x->vui.restriction = v->vuiBitstreamRestric.bitstream_restriction_flag ? 1 : 0;
        x->v4_pend.vuiVideoSignal.video_signal_type_present_flag = x->vui.signal;
        x->v4_pend.vuiVideoSignal.video_format = x->vui.format ? x->vui.format : 5;
        x->v4_pend.vuiVideoSignal.video_full_range_flag = x->vui.full;
        x->v4_pend.vuiAspectRatio.sar_width = x->vui.sar_w;
        x->v4_pend.vuiAspectRatio.sar_height = x->vui.sar_h;
        x->vui_pend = 1;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetH264Vui(int encChn, IMPEncoderH264Vui *v)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!v)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1)
        *v = E1.v4;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetH265Vui(int encChn, const IMPEncoderH265Vui *v)
{
    T32EncChnInfo ci;
    struct t32_e1 *x;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!v)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 2) {
        x = &E1;
        if (!x->vui_pend) {
            x->v4_pend = x->v4;
            x->v5_pend = x->v5;
        }
        x->vui.aspect = v->vuiAspectRatio.aspect_ratio_info_present_flag ? 1 : 0;
        x->vui.aspect_idc = v->vuiAspectRatio.aspect_ratio_idc;
        x->vui.sar_w = v->vuiAspectRatio.sar_width;
        x->vui.sar_h = v->vuiAspectRatio.sar_height;
        x->vui.signal = v->vuiVideoSignal.video_signal_type_present_flag ? 1 : 0;
        x->vui.format = v->vuiVideoSignal.video_format & 7u;
        x->vui.full = v->vuiVideoSignal.video_full_range_flag ? 1 : 0;
        x->vui.restriction = v->vuiBitstreamRestric.bitstream_restriction_flag ? 1 : 0;
        x->v5_pend.vuiAspectRatio = v->vuiAspectRatio;
        x->v5_pend.vuiVideoSignal.video_signal_type_present_flag = x->vui.signal;
        x->v5_pend.vuiVideoSignal.video_format = x->vui.format;
        x->v5_pend.vuiVideoSignal.video_full_range_flag = x->vui.full;
        x->vui_pend = 1;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetH265Vui(int encChn, IMPEncoderH265Vui *v)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!v)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 2)
        *v = E1.v5;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

/* ---- chroma QP offsets: stored, taken at the next IDR (not in the PPS / HERA yet) ------------- */

int IMP_Encoder_SetH264TransCfg(int encChn, const IMPEncoderH264TransCfg *t)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!t)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 1)               /* vendor: no channel or another codec answers 0 */
        E1.trans264_pend = t->chroma_qp_index_offset < -12 ? -12 : t->chroma_qp_index_offset > 12 ? 12 :
                              t->chroma_qp_index_offset;
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_GetH264TransCfg(int encChn, IMPEncoderH264TransCfg *t)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!t)
        return E1_ERR_NULL;
    t->chroma_qp_index_offset = 0;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 1)
        t->chroma_qp_index_offset = E1.trans264;
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_SetH265TransCfg(int encChn, const IMPEncoderH265TransCfg *t)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!t)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 2) {
        E1.trans265_pend.chroma_cr_qp_offset = t->chroma_cr_qp_offset < -12 ? -12 : t->chroma_cr_qp_offset > 12 ? 12 :
                                                  t->chroma_cr_qp_offset;
        E1.trans265_pend.chroma_cb_qp_offset = t->chroma_cb_qp_offset < -12 ? -12 : t->chroma_cb_qp_offset > 12 ? 12 :
                                                  t->chroma_cb_qp_offset;
    }
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_GetH265TransCfg(int encChn, IMPEncoderH265TransCfg *t)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!t)
        return E1_ERR_NULL;
    t->chroma_cr_qp_offset = t->chroma_cb_qp_offset = 0;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 2)
        *t = E1.trans265;
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

/* ---- super frames: stored, taken at the next IDR ------------------------------------------------ */

int IMP_Encoder_SetSuperFrameCfg(int encChn, const IMPEncoderSuperFrmCfg *c)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!c)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec != 0) {
        E1.sf_pend = *c;
        if (c->superIFrmBitsThr > E1.sf_max)
            E1.sf_pend.superIFrmBitsThr = E1.sf_max;
    }
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_GetSuperFrameCfg(int encChn, IMPEncoderSuperFrmCfg *c)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!c)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec != 0)
        *c = E1.sf;
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

/* ---- user data ------------------------------------------------------------------------------- */

int IMP_Encoder_InsertUserData(int encChn, void *data, uint32_t len)
{
    T32EncChnInfo ci;
    struct t32_e1 *x;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    else if (!rc && (!data || !len))
        rc = E1_ERR_NULL;
    if (!rc) {
        x = &E1;
        if (!x->ud_cnt || !x->ud_size)
            rc = T32_ENC_ERR_MODE;      /* no user-data buffers in the channel attribute */
        else if (x->ud_n >= x->ud_cnt)
            rc = E1_ERR_FULL;
        else if (len > x->ud_size)
            rc = T32_ENC_ERR_MODE;
        else {
            uint32_t at = (x->ud_head + x->ud_n) % T32_E1_UD_MAX;

            memcpy(x->ud[at], data, len);
            x->ud_len[at] = len;
            x->ud_n++;
        }
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

/* ---- frame-type / reference control (E1b): stored --------------------------------------------- */

int IMP_Encoder_SetChangeRef(int encChn, int enable)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    else if (!rc && ci.codec == 1)
        E1.changeref = enable != 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChangeRef(int encChn, int *enable)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    else if (!rc && enable)
        *enable = ci.codec == 1 ? E1.changeref : 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnColor2Grey(int encChn, const IMPEncoderColor2GreyCfg *c)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1 && c)
        E1.grey = c->enable ? 1 : 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChnColor2Grey(int encChn, IMPEncoderColor2GreyCfg *c)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!c)
        return E1_ERR_NULL;
    c->enable = 0;                      /* vendor: also on a channel that does not exist */
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 1)
        c->enable = E1.grey != 0;
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_SetGDRCfg(int encChn, const IMPEncoderGDRCfg *g)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!g)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc) {
        E1.gdr.enable = g->enable ? 1 : 0;
        if (g->enable) {                /* out of range: cycle 120, frames 6 */
            E1.gdr.gdrCycle = g->gdrCycle >= 3 && g->gdrCycle <= 65535 ? g->gdrCycle : 120;
            E1.gdr.gdrFrames = g->gdrFrames >= 2 && g->gdrFrames <= 10 ? g->gdrFrames : 6;
        }
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetGDRCfg(int encChn, IMPEncoderGDRCfg *g)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!g)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc)
        *g = E1.gdr;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_RequestGDR(int encChn, int gdrFrames)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc)
        E1.gdr_req = gdrFrames >= 2 && gdrFrames <= 10 ? gdrFrames : E1.gdr.gdrFrames;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetPskipCfg(int encChn, const IMPEncoderPskipCfg *p)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!p)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc) {
        if (!p->enable)
            E1.pskip.enable = 0;
        else if (p->pskipMaxFrames < 1 || p->pskipThr < 0)
            rc = T32_ENC_ERR_PARAM;
        else
            E1.pskip = *p;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetPskipCfg(int encChn, IMPEncoderPskipCfg *p)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!p)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc)
        *p = E1.pskip;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_RequestPskip(int encChn)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc)
        E1.pskip_req = 1;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnRcTrigLevel(int encChn, uint32_t level, int mode)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1) {
        E1.trig_level = level;
        E1.trig_mode = mode;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetPadFrameAttr(int encChn, int enable, const void *attr)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1) {
        E1.pad = enable != 0;
        if (attr)
            memcpy(E1.pad_attr, attr, sizeof(E1.pad_attr));
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

/* vendor order: channel, frame, created and receiving, H.264 only (H.265: 0, frame kept), pad frames on.
 * Pad-frame encoding itself is not implemented: with pad frames on the frame is handed back (0x80040080). */
int IMP_Encoder_PadFrame(int encChn, void *frame, void *priv, void (*release)(void *frame, void *priv))
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        rc = T32_ENC_ERR_CHN;
    else if (!frame)
        rc = E1_ERR_NULL;
    else {
        pthread_mutex_lock(&e1_lock);
        int ok = e1_get(encChn, &ci) == 0;
        rc = !ok || !ci.recv ? T32_ENC_ERR_MODE : ci.codec != 1 ? 1 : T32_ENC_ERR_MODE;
        pthread_mutex_unlock(&e1_lock);
        if (rc == 1)
            return 0;
    }
    if (release)
        release(frame, priv);
    return rc;
}

/* ---- E2: stored ------------------------------------------------------------------------------- */

int IMP_Encoder_SetChnCrop(int encChn, const IMPEncoderCropCfg *c)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!c)
        return E1_ERR_NULL;
    if ((c->x | c->y | c->w | c->h) & 1u)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && (c->x + c->w > ci.width || c->y + c->h > ci.height))
        rc = T32_ENC_ERR_PARAM;
    if (!rc && ci.codec != 0) {
        E1.crop_pend = *c;
        E1.crop_pend.enable = c->enable ? 1 : 0;
        if (ci.codec != 2)                   /* H.264 reads back at once, H.265 from the next IDR */
            E1.crop = E1.crop_pend;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChnCrop(int encChn, IMPEncoderCropCfg *c)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && c) {
        memset(c, 0, sizeof(*c));
        if (ci.codec != 0)
            *c = E1.crop;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnDenoise(int encChn, const IMPEncoderAttrDenoise *d)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1 && d) {
        E1.dn_type = 0;              /* denoise was not enabled at CreateChn: the type stays 0 */
        E1.dn_iqp = d->dnIQp;
        E1.dn_pqp = d->dnPQp;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChnDenoise(int encChn, IMPEncoderAttrDenoise *d)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 1 && d) {      /* vendor: the enable byte is left alone */
        d->dnType = E1.dn_type;
        d->dnIQp = E1.dn_iqp;
        d->dnPQp = E1.dn_pqp;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnHSkip(int encChn, const IMPEncoderAttrHSkip *h)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && h && ci.codec != 0) {
        /* vendor 2.0.2.1 in QEMU: H.264 takes every type up to H1M_TRUE, H.265 only N1X */
        if ((uint32_t)h->skipType > (ci.codec == 2 ? 0u : (uint32_t)IMP_Encoder_STYPE_H1M_TRUE))
            rc = T32_ENC_ERR_PARAM;
        else {
            E1.hskip.skipType = h->skipType;
            E1.hskip.m = h->m;
            E1.hskip.n = h->n;
        }
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChnHSkip(int encChn, IMPEncoderAttrHSkip *h)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && h)
        *h = E1.hskip;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnHSkipBlackEnhance(int encChn, const int bBlackEnhance)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 0)
        E1.hskip.bBlackEnhance = bBlackEnhance != 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnROI(int encChn, const IMPEncoderROIAttr *r)
{
    T32EncChnInfo ci;
    int rc, i;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!r)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 0)
        for (i = 0; i < IMP_ENC_ROI_WIN_COUNT; i++) {
            const IMPEncoderROICfg *c = &r->roi[i];
            IMPEncoderROICfg *s = &E1.roi_pend[i];

            memset(s, 0, sizeof(*s));
            s->u32Index = c->u32Index & 0xffu;
            s->bEnable = c->bEnable ? 1 : 0;
            if (s->bEnable) {           /* macroblock units, as the vendor keeps them */
                s->bRelatedQp = c->bRelatedQp ? 1 : 0;
                s->s32Qp = c->s32Qp;
                s->rect.p0.x = (c->rect.p0.x / 16) * 16;
                s->rect.p0.y = (c->rect.p0.y / 16) * 16;
                s->rect.p1.x = (c->rect.p1.x / 16) * 16;
                s->rect.p1.y = (c->rect.p1.y / 16) * 16;
            }
            if (ci.codec == 1)          /* H.264 reads back at once, H.265 from the next IDR */
                E1.roi[i] = *s;
        }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetChnROI(int encChn, IMPEncoderROIAttr *r)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!r)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc) {
        memset(r, 0, sizeof(*r));
        if (ci.codec != 0)
            memcpy(r->roi, E1.roi, sizeof(r->roi));
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnMapRoi(int encChn, IMPEncoderMapRoiCfg *m, IMPEncoderMappingList *list)
{
    T32EncChnInfo ci;
    int rc;

    (void)list;
    if (!m || !m->map)
        return E1_ERR_NULL;
    if ((uint32_t)m->type >= (uint32_t)IMP_MAPPINGTYPE_BUTT)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetMbRC(int encChn, int enable)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    else if (!rc && ci.codec == 1)
        E1.mbrc = enable != 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetMbRC(int encChn, int *enable)
{
    T32EncChnInfo ci;
    int rc;

    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    else if (!rc && enable)
        *enable = ci.codec == 1 ? E1.mbrc : 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetQpgMode(int encChn, const IMPEncoderQpgMode *q)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!q)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 0) {
        E1.qpg_pend = (int)*q;
        if (ci.codec != 2)                   /* H.264 reads back at once, H.265 from the next IDR */
            E1.qpg = E1.qpg_pend;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetQpgMode(int encChn, IMPEncoderQpgMode *q)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!q)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 0)
        *q = (IMPEncoderQpgMode)E1.qpg;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetSrdCfg(int encChn, const IMPEncoderSrdCfg *s)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!s)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 2)
        rc = T32_ENC_ERR_MODE;          /* H.265 only */
    else if (!rc)
        E1.srd_pend = *s;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_GetSrdCfg(int encChn, IMPEncoderSrdCfg *s)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!s)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && ci.codec != 2)
        rc = T32_ENC_ERR_MODE;
    else if (!rc)
        *s = E1.srd;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetMotionQuality(int encChn, const IMPEncoderMotionQuality *m)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!m)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    pthread_mutex_unlock(&e1_lock);
    if (!rc && !OpenIMP_T32_EncMotionQualityOn(encChn))
        rc = E1_ERR_OFF;
    if (rc)
        return rc;
    if (m->enable && (uint32_t)m->level > 3u)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok) {
        if (m->enable)
            E1.mq.level = m->level;
        E1.mq.enable = m->enable ? 1 : 0;
    }
    pthread_mutex_unlock(&e1_lock);
    return 0;
}

int IMP_Encoder_GetMotionQuality(int encChn, IMPEncoderMotionQuality *m)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!m)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc && !OpenIMP_T32_EncMotionQualityOn(encChn))
        rc = E1_ERR_OFF;                /* vendor: the struct is left alone */
    if (!rc)
        *m = E1.mq;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

int IMP_Encoder_SetChnQpgAI(int encChn, IMPEncoderQpgAICfg *q)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!q)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    pthread_mutex_unlock(&e1_lock);
    if (!rc && !OpenIMP_T32_EncQpgAIOn(encChn))
        rc = E1_ERR_OFF;
    else if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_MODE;
    return rc;
}
