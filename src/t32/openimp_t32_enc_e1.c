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
 *   - T33 (#784): Crop as SPS frame cropping / H.265 conformance window and H264TransCfg as PPS + HERA chroma
 *     QP offset from the next IDR, as vendor 2.0.2.1 (OpenIMP_T32_E1Hdr; the T32 build keeps them stored)
 * Stored and read back like the vendor (deferred to the next IDR where the vendor defers), NOT applied to
 * the HERA engine (the vendor applies them, #784 lists where): H265TransCfg, SuperFrameCfg, Crop on T32,
 * Denoise, HSkip, ROI / MapRoi, MbRC, QpgMode, Srd, ChangeRef, Color2Grey, GDR, Pskip, RcTrigLevel,
 * MotionQuality, QpgAI, pad frames.
 * Built with the vendor 2.x headers (their types); the channel table is openimp_t32_encoder.c's
 * (OpenIMP_T32_EncChnQuery; the encoder calls OpenIMP_T32_E1Reset / _E1AtIdr / _E1Sei).
 * Lock order: this unit's lock, then the encoder's.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

#include "t32/openimp_t32_enc_e1.h"
#include "t32/openimp_t32_pskip.h"

#define T32_ENC_ERR_CHN     ((int)0x80040001)
#define T32_ENC_ERR_PARAM   ((int)0x80040002)
#define T32_ENC_ERR_NOCHN   ((int)0x80040008)
#define T32_ENC_ERR_MODE    ((int)0x80040080)
#define E1_ERR_PARAM        ((int)0x80040002)
#define E1_ERR_NULL         ((int)0x80040010)
#define E1_ERR_FULL         ((int)0x80040800)
#define E1_ERR_OFF          ((int)0x80048000)
#define ALIGN16(x) (((x) + 15u) & ~15u)
#define T32_E1_UD_MAX   2                /* user-data queue entries (maxUserDataCnt: vendor range 0..2) */
#define T32_E1_UD_BYTES 1024             /* bytes per entry (maxUserDataSize: vendor 16..1024) */
#define T32_E1_CHNS     16

int OpenIMP_T32_CscGamut(void);                   /* openimp_t32_tuning.c: ISP frame CSC gamut, -1 unknown */
int OpenIMP_T32_EncMotionQualityOn(int encChn);  /* openimp_t32_fs_enc_ext.c: SetMotionQualityEnable */
int OpenIMP_T32_EncQpgAIOn(int encChn);          /* SetChnQpgAIEnable */

struct t32_e1 {
    T32Vui vui;                          /* the next IDR's VUI */
    int vui_pend;
    int auto_vui;                        /* IMP_Encoder_SetAutoVui (#930) */
    uint32_t auto_vui_last;              /* the frame gamut the VUI was last set for (-1: none yet) */
    IMPEncoderH264Vui v4, v4_pend;       /* getter images: in effect / from the next IDR */
    IMPEncoderH265Vui v5, v5_pend;
    int trans264, trans264_pend;
    IMPEncoderH265TransCfg trans265, trans265_pend;
    IMPEncoderSuperFrmCfg sf, sf_pend;
    uint32_t sf_max;
    IMPEncoderCropCfg crop, crop_pend;
    int qpg, qpg_pend;
    IMPEncoderSrdCfg srd, srd_pend;
    int changeref, grey, grey_hw, mbrc, pskip_req, gdr_req, gdr_req_frames, pad, trig_mode;
    int rc_new;                 /* T32_E1RC_* changes for the next IDR (#784) */
    uint32_t trig_level;
    uint8_t pad_attr[32];
    IMPEncoderGDRCfg gdr;
    IMPEncoderPskipCfg pskip;
    int dn_type, dn_iqp, dn_pqp;
    IMPEncoderAttrHSkip hskip;
    IMPEncoderROICfg roi[IMP_ENC_ROI_WIN_COUNT], roi_pend[IMP_ENC_ROI_WIN_COUNT];
    uint8_t roi_rec[IMP_ENC_ROI_WIN_COUNT][9];  /* the vendor's set_param(2) records (#726) */
    int roi_new;
    IMPEncoderMotionQuality mq;
    uint8_t mq_on;                      /* #784: SetMotionQuality enable latched at the IDR */
    uint32_t ud_cnt, ud_size, ud_head, ud_n, ud_len[T32_E1_UD_MAX];
    uint8_t ud[T32_E1_UD_MAX][T32_E1_UD_BYTES];
};

static pthread_mutex_t e1_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t32_e1 e1_tab[T32_E1_CHNS];
/* #977: the map SetChnMapRoi copied (vendor 2.2.0 i264e_reconfig_roi_set 0x7fae0, kind 1:
 * h+0x2af4 = 1, a malloc'ed copy at h+0x2af8 of the h+0x2afc bytes, flag 4); outside the
 * E1 state: the buffer is kept, CreateChn (a new encoder) only forgets the map.  T33 (#99):
 * vendor 2.0.2.1 i264e_reconfig_roi_set 0x82660 / I265eReconfig::setRoi 0x98ef8, the same
 * offsets */
static struct { uint8_t *map; uint32_t size, cap; int fresh; } e1_map[T32_E1_CHNS];
#ifndef PLATFORM_T33
/* #977: the SetChnQpgAI map (vendor 2.2.0 i264e_reconfig_qpgai_set 0x81874: a malloc'ed copy of
 * width x height bytes at h+0x2c4c, +0x2c50 width, +0x2c54 height, +0x2c58 / +0x2c59 the modes
 * clipped to 0..2 / 0..3, flag 0x200000) */
static struct { uint8_t *map; uint32_t size, cap, w, h; uint8_t fore, mark; int fresh; } e1_qai[T32_E1_CHNS];
#endif
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
    e1_map[encChn].size = 0;
    e1_map[encChn].fresh = 0;
#ifndef PLATFORM_T33
    e1_qai[encChn].size = 0;
    e1_qai[encChn].fresh = 0;
#endif
    x->auto_vui_last = 0xffffffffu;
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
    x->mq_on = x->mq.enable ? 1 : 0;   /* #784: vendor takes MotionQuality at the next IDR */
    memcpy(x->roi, x->roi_pend, sizeof(x->roi));
    pthread_mutex_unlock(&e1_lock);
    return v;
}

void OpenIMP_T32_E1Hdr(int encChn, T32E1Hdr *h)
{
    memset(h, 0, sizeof(*h));
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return;
    pthread_mutex_lock(&e1_lock);
    h->chroma264 = E1.trans264;
    h->chroma265 = E1.trans265.chroma_cr_qp_offset;
    h->crop = E1.crop.enable ? 1 : 0;
    h->motion_quality = E1.mq_on;
    if (h->crop) {
        h->crop_x = E1.crop.x;
        h->crop_y = E1.crop.y;
        h->crop_w = E1.crop.w;
        h->crop_h = E1.crop.h;
    }
    pthread_mutex_unlock(&e1_lock);
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
#if !defined(PLATFORM_T33)
            x->vui.desc = v->vuiVideoSignal.colour_description_present_flag ? 1 : 0;
            x->vui.prim = v->vuiVideoSignal.colour_primaries;
            x->vui.trans = v->vuiVideoSignal.transfer_characteristics;
            x->vui.matrix = v->vuiVideoSignal.matrix_coefficients;
#endif
        }
#if !defined(PLATFORM_T33)
        else {
            /* vendor 2.2.0 (QEMU A/B apitest t33e1, #99): a signal flag of 0 returns the SPS to the default (no video signal
             * type, no colour description) instead of keeping the fields an earlier call set */
            x->vui.format = 5;
            x->vui.full = 0;
            x->vui.desc = x->vui.prim = x->vui.trans = x->vui.matrix = 0;
        }
#endif
        x->vui.signal = x->vui.format != 5 || x->vui.full;
        x->vui.restriction = v->vuiBitstreamRestric.bitstream_restriction_flag ? 1 : 0;
        x->v4_pend.vuiVideoSignal.video_signal_type_present_flag = x->vui.signal;
        x->v4_pend.vuiVideoSignal.video_format = x->vui.format ? x->vui.format : 5;
        x->v4_pend.vuiVideoSignal.video_full_range_flag = x->vui.full;
#if !defined(PLATFORM_T33)
        if (x->vui.desc) {
            x->v4_pend.vuiVideoSignal.colour_description_present_flag = 1;
            x->v4_pend.vuiVideoSignal.colour_primaries = x->vui.prim;
            x->v4_pend.vuiVideoSignal.transfer_characteristics = x->vui.trans;
            x->v4_pend.vuiVideoSignal.matrix_coefficients = x->vui.matrix;
        } else {
            x->v4_pend.vuiVideoSignal.colour_description_present_flag = 0;
        }
#endif
        x->v4_pend.vuiAspectRatio.sar_width = x->vui.sar_w;
        x->v4_pend.vuiAspectRatio.sar_height = x->vui.sar_h;
        x->vui_pend = 1;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

/* IMP_Encoder_SetAutoVui (T32 2.2.0 0xf4118): chn >= 16 0x80040001, not created 0x80040008, else the
 * flag is kept (any non-zero = on). */
#if !defined(PLATFORM_T33)           /* T33 2.0.2.1 has no SetAutoVui (2.1.0 does) */
int IMP_Encoder_SetAutoVui(int encChn, int bEnable)
{
    T32EncChnInfo ci;
    int rc;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    if (!rc)
        E1.auto_vui = bEnable != 0;
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

#endif

/* The encode loop (vendor on_encoder_group_data_update 0xf1730 / 0xf2bd8), once per frame: with AutoVui on and
 * a frame gamut that differs from the last one, read the VUI, set the video signal and write it back (it takes
 * effect at the next IDR like any SetH264Vui): video_signal_type_present 1, full range = the gamut is not one
 * of the limited ones (1, 3, 5), colour_description_present 1, matrix 5 for BT601 (0, 1), 9 for BT2020 (4, 5),
 * else 1; colour primaries and transfer 1 on the first update.  A failed Get / Set is retried on the next frame. */
void OpenIMP_T32_E1AutoVui(int encChn, int hevc)
{
    int on, g;
    uint32_t last;
    uint8_t full, mc;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return;
    pthread_mutex_lock(&e1_lock);
    on = E1.auto_vui;
    last = E1.auto_vui_last;
    pthread_mutex_unlock(&e1_lock);
    if (!on || (g = OpenIMP_T32_CscGamut()) < 0 || (uint32_t)g == last)
        return;
    full = (uint32_t)g < 6 ? (((1u << g) & 0x2au) == 0) : 1;
    mc = (uint32_t)g < 6 ? ((((1u << g) & 3u) != 0) ? 5 : ((((1u << g) & 0x30u) != 0) ? 9 : 1)) : 1;
    if (hevc) {
        IMPEncoderH265Vui v;

        if (IMP_Encoder_GetH265Vui(encChn, &v) != 0)
            return;
        v.vuiVideoSignal.video_signal_type_present_flag = 1;
        v.vuiVideoSignal.video_full_range_flag = full;
        v.vuiVideoSignal.colour_description_present_flag = 1;
        v.vuiVideoSignal.matrix_coefficients = mc;
        if (last == 0xffffffffu)
            v.vuiVideoSignal.colour_primaries = v.vuiVideoSignal.transfer_characteristics = 1;
        if (IMP_Encoder_SetH265Vui(encChn, &v) != 0)
            return;
    } else {
        IMPEncoderH264Vui v;

        if (IMP_Encoder_GetH264Vui(encChn, &v) != 0)
            return;
        v.vuiVideoSignal.video_signal_type_present_flag = 1;
        v.vuiVideoSignal.video_full_range_flag = full;
        v.vuiVideoSignal.colour_description_present_flag = 1;
        v.vuiVideoSignal.matrix_coefficients = mc;
        if (last == 0xffffffffu)
            v.vuiVideoSignal.colour_primaries = v.vuiVideoSignal.transfer_characteristics = 1;
        if (IMP_Encoder_SetH264Vui(encChn, &v) != 0)
            return;
    }
    pthread_mutex_lock(&e1_lock);
    E1.auto_vui_last = (uint32_t)g;
    pthread_mutex_unlock(&e1_lock);
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
#if !defined(PLATFORM_T33)
        x->vui.desc = v->vuiVideoSignal.colour_description_present_flag ? 1 : 0;
        x->vui.prim = v->vuiVideoSignal.colour_primaries;
        x->vui.trans = v->vuiVideoSignal.transfer_characteristics;
        x->vui.matrix = v->vuiVideoSignal.matrix_coefficients;
        x->v5_pend.vuiVideoSignal.colour_description_present_flag = x->vui.desc;
        x->v5_pend.vuiVideoSignal.colour_primaries = x->vui.prim;
        x->v5_pend.vuiVideoSignal.transfer_characteristics = x->vui.trans;
        x->v5_pend.vuiVideoSignal.matrix_coefficients = x->vui.matrix;
#endif
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

/* ---- chroma QP offsets: taken at the next IDR (T33 H.264: PPS + HERA, #784; H.265 / T32: stored) - */

int IMP_Encoder_SetH264TransCfg(int encChn, const IMPEncoderH264TransCfg *t)
{
    T32EncChnInfo ci;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!t)
        return E1_ERR_NULL;
    pthread_mutex_lock(&e1_lock);
    int ok = e1_get(encChn, &ci) == 0;
    if (ok && ci.codec == 1) {             /* vendor: no channel or another codec answers 0 */
        E1.trans264_pend = t->chroma_qp_index_offset < -12 ? -12 : t->chroma_qp_index_offset > 12 ? 12 :
                              t->chroma_qp_index_offset;
#if !defined(PLATFORM_T33)
        E1.rc_new |= T32_E1RC_RESTART;      /* vendor 2.2.0 (QEMU A/B, CBR): the controller restarts at the next IDR */
#endif
    }
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
        /* vendor 2.0.2.1 (#784, QEMU v265t / v265n) and 2.2.0 (#99, QEMU apitest t33e1 get-back "cr 3 cb 3" after set 3/-2): the
         * pictures and the PPS use the larger of the two offsets, clipped to -12..12, for cb and cr ({13, 0} -> 12/12,
         * {-2, 3} -> 3/3, {-5, -2} -> -2) */
        int m = t->chroma_cr_qp_offset > t->chroma_cb_qp_offset ? t->chroma_cr_qp_offset : t->chroma_cb_qp_offset;

        m = m < -12 ? -12 : m > 12 ? 12 : m;
        E1.trans265_pend.chroma_cr_qp_offset = m;
        E1.trans265_pend.chroma_cb_qp_offset = m;
#ifndef PLATFORM_T33
        /* #977: vendor 2.2.0 (QEMU A/B, FIXQP): the i265e rate control restarts at the next IDR (its
         * first-IDR QP window 0x2001c) */
        E1.rc_new |= T32_E1RC_RESTART;
#endif
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
        E1.rc_new |= T32_E1RC_SF;
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
    if (!rc && ci.codec != 0 && c) {
        E1.grey_hw = c->enable ? 1 : 0;  /* #788: H.264 and H.265 (the vendor list changes on both) */
        E1.rc_new |= T32_E1RC_RESTART;  /* #784: rate control restarts at the next IDR */
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

/* #788: Color2Grey is on for the pictures coded from now on (H.264 and H.265).  The vendor 2.0.2.1
 * list then points EFE_RAWC at a user-space address; OpenIMP gives the engine a real grey plane. */
int OpenIMP_T32_E1GreyChroma(int encChn)
{
    int g = 0;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    g = E1.grey_hw;
    pthread_mutex_unlock(&e1_lock);
    return g;
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
    if (!rc) {
        /* vendor 0x1087a0: frames 2..10, else the configured frames (none: error) */
        if (gdrFrames >= 2 && gdrFrames <= 10)
            E1.gdr_req_frames = gdrFrames;
        else if (E1.gdr.gdrFrames)
            E1.gdr_req_frames = E1.gdr.gdrFrames;
        else
            rc = E1_ERR_PARAM;
        if (!rc)
            E1.gdr_req = 1;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

void OpenIMP_T32_E1GdrTake(int encChn, T32E1Gdr *g)
{
    memset(g, 0, sizeof(*g));
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return;
    pthread_mutex_lock(&e1_lock);
    g->gate = 1;
    g->en = E1.gdr.enable != 0;
    if (g->en) {
        g->frames = (uint32_t)E1.gdr.gdrFrames;
        g->cycle = (uint32_t)E1.gdr.gdrCycle;
    }
    g->req = E1.gdr_req != 0;
    E1.gdr_req = 0;
    g->req_frames = (uint32_t)E1.gdr_req_frames;
    pthread_mutex_unlock(&e1_lock);
}

int OpenIMP_T32_E1RcTake(int encChn, int idr, T32E1Rc *r)
{
    int f;

    memset(r, 0, sizeof(*r));
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    r->qpg = (uint32_t)E1.qpg_pend;
    f = idr ? E1.rc_new : 0;
    if (idr)
        E1.rc_new = 0;
    r->trig[0] = E1.trig_level;
    r->trig[1] = (uint32_t)E1.trig_mode;
    r->sf[0] = (uint32_t)E1.sf_pend.superFrmMode;
    r->sf[1] = E1.sf_pend.superIFrmBitsThr;
    r->sf[2] = E1.sf_pend.superPFrmBitsThr;
    r->sf[3] = E1.sf_pend.superBFrmBitsThr;
    r->sf[4] = (uint32_t)E1.sf_pend.rcPriority;
    r->sf[5] = E1.sf_pend.maxReEncodeTimes;
    pthread_mutex_unlock(&e1_lock);
    return f;
}

void OpenIMP_T32_E1PskipTake(int encChn, int rc_mode, uint32_t bitrate, T32E1Pskip *p)
{
    memset(p, 0, sizeof(*p));
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return;
    pthread_mutex_lock(&e1_lock);
    if (E1.pskip.enable) {
        p->en = 1;
        p->max_frames = (uint32_t)E1.pskip.pskipMaxFrames;
        p->thr = t32_pskip_thr(E1.pskip.pskipThr, rc_mode, bitrate);
    }
    p->req = E1.pskip_req != 0;
    E1.pskip_req = 0;
    pthread_mutex_unlock(&e1_lock);
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
        /* vendor 0x108a74: off keeps maxFrames / thr; maxFrames is stored
         * before thr is checked */
        if (!p->enable)
            E1.pskip.enable = 0;
        else if (p->pskipMaxFrames < 1)
            rc = T32_ENC_ERR_PARAM;
        else if (E1.pskip.pskipMaxFrames = p->pskipMaxFrames, p->pskipThr < 0)
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
        E1.rc_new |= T32_E1RC_TRIG;
    }
    pthread_mutex_unlock(&e1_lock);
    return rc;
}

#ifdef PLATFORM_T33
#define T33_ENC_ERR_PADQ ((int)0x80044000)

/* SetPadFrameAttr(on) of an H.264 channel: the encoder codes no picture (see IMP_Encoder_PadFrame) */
int OpenIMP_T33_EncPadOn(int encChn)
{
    int r = 0;

    if (encChn >= 0 && encChn < T32_E1_CHNS) {
        pthread_mutex_lock(&e1_lock);
        r = E1.pad;
        pthread_mutex_unlock(&e1_lock);
    }
    return r;
}
#endif

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
 * T32: pad-frame encoding is not implemented: with pad frames on the frame is handed back (0x80040080).
 * T33: as the vendor 2.0.2.1 (see below): 0x80044000 and the encoder codes no picture while pad frames are on. */
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
#ifdef PLATFORM_T33
        /* 2.0.2.1 (0xfed28, Ghidra; QEMU A/B pad-v1): the pad queue (semaphore 0x2df0, depth chn+0x1458) is only
         * built by CreateChn, which reads the enable that SetPadFrameAttr can set only on a created channel:
         * with pad frames on the free-slot wait (__jz_video_sem_timedwait, timeout words 0) fails at once,
         * 0x80044000, the frame is handed back (release callback) */
        if (ok && ci.recv && ci.codec == 1 && E1.pad)
            rc = T33_ENC_ERR_PADQ;
#endif
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
#if !defined(PLATFORM_T33)
        E1.rc_new |= T32_E1RC_RESTART;      /* vendor 2.2.0 (QEMU A/B, CBR): the controller restarts at the next IDR */
#endif
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
            if (ci.codec == 1)
                E1.rc_new |= T32_E1RC_RESTART;  /* #784: H.264 rate control restarts at the next IDR */
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

/* the record vendor 2.0.2.1 IMP_Encoder_SetChnROI (0xfc8c4) hands to
 * i264e_set_param / i265e_set_param (id 2) for region @i (the loop index):
 * type 0, index, enable, relative, QP, then the rectangle in macroblocks,
 * x and y each sorted (min, max), divided by 16 toward zero */
static void e1_roi_rec(uint8_t *b, unsigned i, const IMPEncoderROICfg *c)
{
    int x0 = c->rect.p0.x / 16, x1 = c->rect.p1.x / 16, y0 = c->rect.p0.y / 16, y1 = c->rect.p1.y / 16;
    int xs = c->rect.p0.x < c->rect.p1.x, ys = c->rect.p0.y < c->rect.p1.y;

    b[0] = 0;
    b[1] = (uint8_t)i;
    b[2] = (uint8_t)c->bEnable;
    b[3] = (uint8_t)c->bRelatedQp;
    b[4] = (uint8_t)c->s32Qp;
    b[5] = (uint8_t)(xs ? x0 : x1);
    b[6] = (uint8_t)(xs ? x1 : x0);
    b[7] = (uint8_t)(ys ? y0 : y1);
    b[8] = (uint8_t)(ys ? y1 : y0);
}

/* the encoder thread, after the controller's start of a picture (vendor
 * i264e_reconfig / I265eReconfig::ReconfigPFrame, flag 4): 1 = new region
 * records in @rec (16 x 9 bytes, the parameter block's ROI table) */
int OpenIMP_T32_E1RoiTake(int encChn, uint8_t rec[16][9])
{
    int n;

    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    n = E1.roi_new;
    E1.roi_new = 0;
    if (n)
        memcpy(rec, E1.roi_rec, sizeof(E1.roi_rec));
    pthread_mutex_unlock(&e1_lock);
    return n;
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
            e1_roi_rec(E1.roi_rec[i], (unsigned)i, c);
        }
    if (!rc && ci.codec != 0)
        E1.roi_new = 1;
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

int OpenIMP_T32_E1MapTake(int encChn, int take, const uint8_t **map, uint32_t *size)
{
    int n;

    *map = NULL;
    *size = 0;
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    n = e1_map[encChn].fresh;
    if (take)
        e1_map[encChn].fresh = 0;
    if (e1_map[encChn].size) {
        *map = e1_map[encChn].map;
        *size = e1_map[encChn].size < e1_map[encChn].cap ? e1_map[encChn].size : e1_map[encChn].cap;
    }
    pthread_mutex_unlock(&e1_lock);
    return n;
}

#ifndef PLATFORM_T33
int OpenIMP_T32_E1QpgAITake(int encChn, int take, T32E1QpgAI *q)
{
    int n;

    memset(q, 0, sizeof(*q));
    if (encChn < 0 || encChn >= T32_E1_CHNS)
        return 0;
    pthread_mutex_lock(&e1_lock);
    n = e1_qai[encChn].fresh;
    if (take)
        e1_qai[encChn].fresh = 0;
    if (e1_qai[encChn].size) {
        q->map = e1_qai[encChn].map;
        q->size = e1_qai[encChn].size < e1_qai[encChn].cap ? e1_qai[encChn].size : e1_qai[encChn].cap;
        q->width = e1_qai[encChn].w;
        q->height = e1_qai[encChn].h;
        q->fore = e1_qai[encChn].fore;
        q->mark = e1_qai[encChn].mark;
    }
    pthread_mutex_unlock(&e1_lock);
    return n;
}
#endif

int IMP_Encoder_SetChnMapRoi(int encChn, IMPEncoderMapRoiCfg *m, IMPEncoderMappingList *list)
{
    T32EncChnInfo ci;
    int rc;

    if (!m || !m->map)
        return E1_ERR_NULL;
    if ((uint32_t)m->type >= (uint32_t)IMP_MAPPINGTYPE_BUTT)
        return T32_ENC_ERR_PARAM;
    /* vendor 2.2.0 IMP_Encoder_MappingTransform (0xf5d9c; T33 2.0.2.1 0xfd32c), DEVIOUS only and
     * before the channel checks: every map byte (at most 0x10) becomes list->mapdata[byte], in the
     * caller's map.  Any transform failure, a NULL list too (the transform's own 0x80040010), is
     * 0x80040002 to the caller (SetChnMapRoi 0xf6xxx / T33 0xfd52c: "iVar3 < 0" -> 0x80040002) */
    if (m->type == IMP_MAPPINGTYPE_DEVIOUS) {
        if (!list)
            return T32_ENC_ERR_PARAM;
        if (list->length > 0x10u)
            return T32_ENC_ERR_PARAM;
        for (uint32_t i = 0; i < m->mapSize; i++) {
            if (m->map[i] > 0x10u)
                return T32_ENC_ERR_PARAM;
            m->map[i] = list->mapdata[m->map[i]];
        }
    }
    pthread_mutex_lock(&e1_lock);
    rc = e1_get(encChn, &ci);
    /* #977: i264e_set_param(2, {1, map, size}) / i265e_set_param(2) (I265eReconfig_setRoi 0x959ac;
     * T33 #99: 0x82660 / 0x98ef8); a JPEG channel: 0x80040002 */
    if (!rc && ci.codec == 0)
        rc = T32_ENC_ERR_PARAM;
    if (!rc && ci.codec != 0 && encChn < T32_E1_CHNS && m->mapSize) {
        if (!e1_map[encChn].map) {
            e1_map[encChn].map = malloc(m->mapSize);
            e1_map[encChn].cap = e1_map[encChn].map ? m->mapSize : 0;
        }
        if (!e1_map[encChn].map) {
            rc = (int)0xffffffff;
        } else {
            uint32_t n = m->mapSize < e1_map[encChn].cap ? m->mapSize : e1_map[encChn].cap;

            memcpy(e1_map[encChn].map, m->map, n);
            e1_map[encChn].size = m->mapSize;
            e1_map[encChn].fresh = 1;
        }
    }
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
#ifndef PLATFORM_T33
    /* #977: i264e_set_param(0x15) -> i264e_reconfig_qpgai_set (vendor 2.2.0 0x81874; P+0x260..0x26d,
     * i264e_reconfig flag 0x200000), i265e_set_param(0x16) -> I265eReconfig_setQpgAi (0x975a4) */
    if (!rc && ci.codec != 0) {
        uint32_t n = q->width * q->height;

        pthread_mutex_lock(&e1_lock);
        if (!e1_qai[encChn].map && n) {
            e1_qai[encChn].map = malloc(n);
            e1_qai[encChn].cap = e1_qai[encChn].map ? n : 0;
        }
        if (n && !e1_qai[encChn].map) {
            rc = T32_ENC_ERR_PARAM;     /* the vendor's -1 from set_param */
        } else {
            if (n && q->map)
                memcpy(e1_qai[encChn].map, q->map, n < e1_qai[encChn].cap ? n : e1_qai[encChn].cap);
            e1_qai[encChn].size = n;
            e1_qai[encChn].w = q->width;
            e1_qai[encChn].h = q->height;
            e1_qai[encChn].fore = q->foreBackMode > 2u ? 2u : q->foreBackMode;
            e1_qai[encChn].mark = q->markLvl > 3u ? 3u : q->markLvl;
            e1_qai[encChn].fresh = 1;
        }
        pthread_mutex_unlock(&e1_lock);
    }
#endif
    return rc;
}
