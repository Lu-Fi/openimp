/*
 * OpenIMP T32 rate control on HERA (#295, planned; not a release gate).
 *
 * The T32 API carries the T23-family rate-control attributes (CBR, VBR,
 * SMART) plus CVBR, AVBR and QVBR (imp_encoder.h).  Vendor libimp 2.2.0 runs
 * them through its "eprc" T33 revision (JZ_VPU_RC_*_H264_T33 / _T33), which
 * reads the same Helix-family statistics registers after each picture as the
 * T23 revision: HERA keeps them at the Helix offsets (0x13200000 + 0x80120 ..
 * = the first 25 entries of the H.264 statistics list).  Until the T33
 * revision is ported (docs/T32_RC.md), OpenIMP drives the T23 revision
 * (src/eprc/eprc.c, bit-exact with the T23 OEM controller) with the T32
 * attributes mapped onto it.  Platform neutral: no hardware access, so the
 * host tests run it on recorded or synthetic statistics.
 */
#ifndef OPENIMP_T32_RC_H
#define OPENIMP_T32_RC_H

#include <stdint.h>
#include <imp/imp_encoder.h>
#include "eprc/eprc.h"

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
} T32Rc;

/* The EprcParams of a T32 channel (exposed for the host tests): 0, or -1
 * for FIXQP / an unknown mode (no controller). */
int t32_rc_params(const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height, uint32_t gop,
                  uint32_t fps_num, uint32_t fps_den, EprcParams *p);

/* Start (or restart, after IMP_Encoder_SetChnAttrRcMode) the controller.
 * 0 with rc->on 0 for FIXQP, 0 with rc->on 1 for a rate-controlled mode,
 * -1 on allocation failure (rc->on 0: the caller codes at the initial QP). */
int t32_rc_init(T32Rc *rc, const IMPEncoderAttrRcMode *m, uint32_t width, uint32_t height,
                uint32_t gop, uint32_t fps_num, uint32_t fps_den, int hevc);
void t32_rc_free(T32Rc *rc);

/* The QP of the next picture (FRAME_START): @idr 1 for an IDR picture,
 * @gop_pos the pictures since the IDR.  -1 when no controller runs. */
int t32_rc_start(T32Rc *rc, int idr, uint32_t gop_pos);

/* Finish the picture (FRAME_END) with its coded size in bytes and the
 * engine statistics the encoder read (vendor 2.2.0 order: H.264 the 48
 * registers of hera_stat_regs, whose first 25 are EPRC_StatRegs; H.265 has
 * no Helix-family statistics, @stats NULL).  No re-encoding: the HERA path
 * cannot code a picture again (FRAME_REPEATE_JUDGE skipped). */
void t32_rc_end(T32Rc *rc, uint32_t bytes, const uint32_t *stats, unsigned nstats);

#endif
