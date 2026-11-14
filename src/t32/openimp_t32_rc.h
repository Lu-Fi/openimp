/*
 * OpenIMP T32 rate control on HERA (#295, planned; not a release gate).
 *
 * The T32 API carries the T23-family rate-control attributes (CBR, VBR,
 * SMART) plus CVBR, AVBR and QVBR (imp_encoder.h).  Vendor libimp 2.2.0 runs
 * them through its "eprc" T33 revision (JZ_VPU_RC_*_H264_T33 / _T33), which
 * reads the same Helix-family statistics registers after each picture as the
 * T23 revision: HERA keeps them at the Helix offsets (0x13200000 + 0x80120 ..
 * = the first 25 entries of the H.264 statistics list).  H.264 runs the
 * port of the T33 revision (src/eprc/eprc_t33_h264.c behind the vendor's
 * i264e glue, openimp_t32_rc33.c; QP-exact with the vendor on the reference
 * runs, docs/T32_RC.md), H.265 its H.265 build (src/eprc/eprc_t33_h265.c
 * behind the i265e glue, t32_rc65_*).  QVBR (refused by the vendor) runs the
 * T23 revision (src/eprc/eprc.c) with the T32 attributes mapped onto it.  Platform neutral: no hardware access, so
 * the host tests run it on recorded or synthetic statistics.
 */
#ifndef OPENIMP_T32_RC_H
#define OPENIMP_T32_RC_H

#include <stdint.h>
#include <imp/imp_encoder.h>
#include "eprc/eprc.h"
#include "t32/openimp_t32_rc33.h"

typedef struct {
    int on;                     /* a controller runs (not FIXQP) */
    int hevc;
    int mode;                   /* IMP_ENC_RC_MODE_* the channel asked for */
    Eprc eprc;
    uint8_t *slice;             /* EPRC slice block (EPRC_SLICE_SIZE) */
    int i_min, i_max;           /* IDR QP window (minIQp / maxIQp), from the attributes */
    int min_qp, max_qp;
    int qp;                     /* QP of the picture in work */
    int idr;
    uint32_t pictures;
    uint64_t bits;              /* coded bits since init (statistics) */
    EprcParams params;          /* what the controller was started with */
    T32Rc33 *t33;               /* H.264: the vendor's T33 revision (NULL: T23 revision) */
    T32Rc65 *t65;               /* H.265: the vendor's T33 revision (NULL: T23 revision) */
    /* H.264: the i264e reconfiguration copy of the parameters (vendor
     * i264e_reconfig_init / _rc_set) that SetChnAttrRcMode works on */
#define T32_RC_PWORDS (0x360 / 4)
    uint32_t stash[T32_RC_PWORDS];
    int have_stash;
    uint32_t width, height;     /* the coded size */
    T33Pending pend;            /* run-time changes for the next IDR without a T33 controller (FIXQP) */
} T32Rc;

/* The EprcParams of a T32 channel (exposed for the host tests): 0, or -1
 * for FIXQP / an unknown mode (no controller). */
int t32_rc_params(const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t gop,
                  uint32_t fps_num, uint32_t fps_den, EprcParams *p);

/* The i264e parameters of a T32 H.264 channel for the T33 revision, as
 * vendor 2.2.0 makes them (exposed for the host tests): @width / @height the
 * attribute's picture size, @bufsize its encAttr.bufSize, @gop maxGop.  0,
 * -1 for FIXQP / QVBR / an unknown mode. */
int t32_rc33_params(T32Rc33 *c, const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t bufsize,
                    uint32_t gop, uint32_t fps_num, uint32_t fps_den);

/* The i265e parameters of a T32 H.265 channel for the T33 revision, as
 * vendor 2.2.0 makes them (channel_i265e_encoder_init + I265e_configure;
 * exposed for the host tests).  @hs the attribute's attrHSkip (NULL: none).
 * 0, -1 for QVBR / an unknown mode. */
int t32_rc65_params(T32Rc65 *c, const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t bufsize,
                    uint32_t gop, uint32_t fps_num, uint32_t fps_den, const IMPEncoderAttrInitHSkip *hs);

/* vendor 2.2.0 H.264 channel set-up: the frame rate reduced (0 -> 25/1) and
 * the GOP rounded up to a multiple of its numerator (0 -> the frame rate),
 * and the stream buffer size IMP_Encoder_CreateChn makes of encAttr.bufSize */
void t32_rc_vendor_rate(uint32_t *fps_num, uint32_t *fps_den, uint32_t *gop);
uint32_t t32_rc_vendor_bufsize(uint32_t width, uint32_t height, uint32_t bufsize);

/* IMP_Encoder_SetDefaultParam's rate-control defaults for rc->rcMode
 * (vendor 2.2.0); @type the payload (PT_JPEG: the FIXQP JPEG QP). */
void t32_rc_default_attr(IMPEncoderAttrRcMode *rc, IMPPayloadType type, int initqp, uint32_t kbps);

/* Start (or restart, after IMP_Encoder_SetChnAttrRcMode) the controller.
 * 0 with rc->on 0 for FIXQP, 0 with rc->on 1 for a rate-controlled mode,
 * -1 on allocation failure (rc->on 0: the caller codes at the initial QP). */
int t32_rc_init(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t pic_w, uint32_t pic_h, uint32_t bufsize,
                uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc);
void t32_rc_free(T32Rc *rc);

/* IMP_Encoder_SetChnAttrRcMode at the next IDR: an H.264 channel set up
 * by t32_rc_init takes @m as vendor 2.2.0 does (on top of its current
 * parameters, i264e_reconfig_rc_set + i264e_idr_reconfig); anything else
 * starts over (t32_rc_init).  Same returns as t32_rc_init. */
int t32_rc_reconfig(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t pic_w, uint32_t pic_h, uint32_t bufsize,
                    uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc);

/* The QP of the next picture (FRAME_START): @idr 1 for an IDR picture,
 * @gop_pos the pictures since the IDR.  -1 when no controller runs. */
int t32_rc_start(T32Rc *rc, int idr, uint32_t gop_pos);

/* Finish the picture (FRAME_END) with its coded size in bytes and the
 * engine statistics the encoder read (vendor 2.2.0 order: H.264 the 48
 * registers of hera_stat_regs, whose first 25 are EPRC_StatRegs; H.265 the
 * 38 of hera5_stat_regs).  No re-encoding: the HERA path
 * cannot code a picture again (FRAME_REPEATE_JUDGE skipped). */
void t32_rc_end(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats);

/* after an IDR: the HERA per-row coded sizes the controller wants
 * (t32_rc_idr_rows rows, read as T33_HERA_ROW_SEL / T33_HERA_ROW) */
unsigned t32_rc_idr_rows(T32Rc *rc);
void t32_rc_end_rows(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats, const uint32_t *rows,
                     unsigned nrows);

/* H.264 run-time changes (IMP_Encoder_SetChnBitRate / _SetChnQpBounds /
 * _SetChnQpBoundsPerFrame / _SetChnMaxPictureSize: vendor i264e_set_param id
 * 0x11..0x14, see t32_rc33_set_param): into the reconfiguration copy and
 * pending for the next IDR, where t32_rc_start (or t32_rc_reconfig, after the
 * copy) takes them.  0, -1 without an H.264 channel. */
int t32_rc_set_param(T32Rc *rc, unsigned id, const uint32_t *a);
/* FIXQP channel at an IDR: the fixed I / P QPs a pending per-picture QP bound
 * sets (vendor: P+0xe0 / +0xe4 = max I / max P), 1 when there were any; the
 * pending changes are taken either way */
int t32_rc_fixqp_take(T32Rc *rc, int *qp_i, int *qp_p);

/* H.264 (T33 revision): the slice block of the picture in work (after
 * t32_rc_start) for the HERA command list, NULL without it; and its per-MB
 * maps (T33_MAP_*, NULL when not in use) */
const uint8_t *t32_rc_slice(T32Rc *rc);
const uint8_t *t32_rc_map(T32Rc *rc, int which, uint32_t *len);

#endif
