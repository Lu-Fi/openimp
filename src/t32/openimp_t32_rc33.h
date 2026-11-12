/*
 * OpenIMP T32 H.264 rate control, eprc T33 revision (#295, planned).
 *
 * The vendor 2.2.0 encoder (i264e) drives the controller through its
 * parameter block: i264e_param_default fills it, i264e_ratecontrol_init
 * copies it into the controller block (+0x360) and runs
 * eprc_default_set_H264_T33 / JZ_VPU_RC_VIDEO_CFG_H264_T33,
 * i264e_ratecontrol_start / _end / _is_reenc wrap FRAME_START / FRAME_END.
 * This layer reimplements those four i264e functions on the arena of
 * eprc_t33_mem.h, so the controller (src/eprc/eprc_t33_h264.c) sees the
 * same memory as under the vendor encoder.  Platform neutral: no hardware
 * access (the per-row HERA statistics the vendor reads after an IDR are
 * passed in, zero by default).
 */
#ifndef OPENIMP_T32_RC33_H
#define OPENIMP_T32_RC33_H

#include <stdint.h>
#include "eprc/eprc_t33.h"

/* i264e parameter block offsets (vendor 2.2.0) */
enum {
    T33P_REV = 0x4,             /* engine revision: 6 HERA (T32), 7 */
    T33P_GOP = 0x2c,
    T33P_WIDTH = 0x3c,
    T33P_HEIGHT = 0x40,
    T33P_MODE = 0xdc,           /* 0 FIXQP 1 CBR 2 VBR 3 SMART 4 CVBR 5 AVBR */
    T33P_FIX_IQP = 0xe0,
    T33P_FIX_PQP = 0xe4,
    T33P_MIN_QP = 0xec,
    T33P_MAX_QP = 0xf0,
    T33P_INIT_QP = 0xf8,        /* -1: none */
    T33P_MAX_IQP = 0xfc,
    T33P_MIN_IQP = 0x100,
    T33P_KBPS = 0x110,
    T33P_LONG_MAX_KBPS = 0x178, /* CVBR */
    T33P_LONG_MIN_KBPS = 0x17c,
    T33P_FPS_NUM = 0x2a4,
    T33P_FPS_DEN = 0x2a8,
};

#define T33_RC_STATS 48         /* engine statistics after each picture (vendor read list) */

/* Run-time changes (vendor IMP_Encoder_SetChnBitRate / _SetChnQpBounds /
 * _SetChnQpBoundsPerFrame / _SetChnMaxPictureSize -> i264e_set_param 0x11..
 * 0x14 -> i264e_reconfig_*_set): flags of the handle word +0x26cc, taken at
 * the next IDR by i264e_idr_reconfig, which copies them into the parameter
 * block, runs i264e_validate_parameters and, these four needing no
 * i264e_ratecontrol_init, a second i264e_ratecontrol_start for the IDR. */
#define T33_SET_BITRATE  0x20000u       /* P+0x110 <- kbps (+0x2b54) */
#define T33_SET_QPBOUND  0x40000u       /* P+0xf0 / +0xec <- max / min QP (+0x2b34 / +0x2b38) */
#define T33_SET_IQPBOUND 0x80000u       /* P+0xfc / +0x100 <- max / min I QP; FIXQP: P+0xe0 / +0xe4 <- max I / max P */
#define T33_SET_PICSIZE  0x100000u      /* P+0x158 / +0x15c <- max I / P picture size, kbit (+0x2bc4 / +0x2bc8) */
typedef struct {
    uint32_t flags;
    uint32_t kbps;
    uint32_t max_qp, min_qp;
    uint32_t max_iqp, min_iqp, max_pqp, min_pqp;
    uint32_t pic_i, pic_p;
} T33Pending;

typedef struct {
    E33 X;
    uint8_t *mem;               /* region 0: parameter block, encoder blocks, slice block, maps */
    uint8_t *state;             /* region 1: the controller's state block (its calloc) */
    uint8_t tpl[0x228];         /* region 2: the vendor's md_ctrl_set global the controller patches */
    uint32_t n;                 /* pictures since init */
    int qp;                     /* QP of the last FRAME_START */
    int type;                   /* picture type of the last FRAME_START (2 IDR) */
    int mb_flags;               /* the MB map carried flags at the last FRAME_START */
    T33Pending pend;            /* run-time changes for the next IDR */
} T32Rc33;

/* Arena and i264e_param_default for a @width x @height channel; the caller
 * then sets the parameters (t32_rc33_set) and calls t32_rc33_init.
 * 0, -1 on allocation failure. */
int t32_rc33_open(T32Rc33 *c, uint32_t width, uint32_t height);
/* the parameter words the controller reads (host test: IMP mapping vs vendor) */
unsigned t32_rc33_param_words(const uint16_t **words);
void t32_rc33_close(T32Rc33 *c);
/* a 32-bit / 8-bit word of the i264e parameter block */
void t32_rc33_set(T32Rc33 *c, uint32_t off, uint32_t v);
void t32_rc33_set8(T32Rc33 *c, uint32_t off, uint8_t v);
uint32_t t32_rc33_get(T32Rc33 *c, uint32_t off);
/* i264e_validate_parameters: the clamps of the words the controller reads */
void t32_rc33_validate(T32Rc33 *c);
/* i264e_set_param @id (0x11 bit rate {target, max}: the target in CBR, else
 * the max; 0x12 QP bounds {max, min}; 0x13 per-picture QP bounds {max I, min I,
 * max P, min P}; 0x14 max picture sizes {I, P}) for the next IDR; 0, -1 for
 * another id */
int t32_rc33_set_param(T32Rc33 *c, unsigned id, const uint32_t *a);
/* i264e_idr_reconfig's copy of the pending changes into the parameter block
 * (no clamps); the flags it took */
uint32_t t32_rc33_take_pending(T32Rc33 *c, const T33Pending *p);
/* i264e_ratecontrol_init (controller defaults, parameter copy, VIDEO_CFG) */
void t32_rc33_init(T32Rc33 *c);
/* the per-picture motion mode (F+0x164): i264e_update_fenc sets 2, the
 * unicorn reference runs leave 0 */
void t32_rc33_set_motion(T32Rc33 *c, uint8_t mode);
/* i264e_ratecontrol_start for the picture @since_idr pictures after the
 * last IDR (0: IDR): the picture QP, -1 when the controller's picture type
 * does not match (the vendor logs and keeps the last QP).  At an IDR with
 * pending changes it goes on as i264e_idr_reconfig does: the changes into the
 * parameter block, the clamps, the second start (its QP). */
int t32_rc33_start(T32Rc33 *c, uint32_t since_idr);
/* i264e_ratecontrol_is_reenc after the picture (no re-encoding): coded size
 * in bytes, @stats the T33_RC_STATS engine statistics or NULL */
void t32_rc33_end(T32Rc33 *c, uint32_t bytes, const uint32_t *stats);

/* HERA (revision 6) after an IDR: vendor i264e_ratecontrol_is_reenc reads
 * the coded size of every 64-line row, for row i by writing 0x13240804 + 8 i
 * to 0x13240044 and reading 0x13240804 + 8 i.  t32_rc33_idr_rows gives the
 * rows the picture in work needs (0: none), t32_rc33_end_rows takes them. */
#define T33_RC_ROWS 64
#define T33_HERA_ROW_SEL 0x13240044u
#define T33_HERA_ROW(i) (0x13240804u + 8u * (uint32_t)(i))
unsigned t32_rc33_idr_rows(T32Rc33 *c);
void t32_rc33_end_rows(T32Rc33 *c, uint32_t bytes, const uint32_t *stats, const uint32_t *rows, unsigned nrows);

/* The slice block the controller fills at every FRAME_START (vendor
 * VPI_set_sliceinfo into the encoder's rc memory H+0xf9c, which
 * hwicodec_pf_h264e_t32v_enc copies and H264E_T32V_SliceInit turns into the
 * HERA command list): T33_SLICE_BYTES bytes in the vendor layout. */
#define T33_SLICE_BYTES 0x878u
const uint8_t *t32_rc33_slice(T32Rc33 *c);

/* The per-MB maps the slice block points at, as the controller left them
 * after FRAME_START (NULL with *len 0 when the picture does not use it):
 * T33_MAP_SMART (s+0x24c, a byte a MB, on with s+0x159), T33_MAP_ROI (s+0x254,
 * 16 bytes per 4 MBs, on with s+0x112), T33_MAP_MB (s+0x250, 16 bits a MB:
 * intra-refresh / forced-intra flags; given while the controller writes
 * flags and once more after it cleared them). */
enum { T33_MAP_SMART, T33_MAP_ROI, T33_MAP_MB };
const uint8_t *t32_rc33_map(T32Rc33 *c, int which, uint32_t *len);

#endif
