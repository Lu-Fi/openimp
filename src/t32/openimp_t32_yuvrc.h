/* T32/T33 stand-alone YUV / JPEG encoder (#752): the vendor 2.0.2.1 / 2.2.0 argument checks, return codes and
 * parameter maps as pure functions (host test tests/t33/yuv_rc_test.c), used by openimp_t32_enc_e3.c.
 * Vendor 2.0.2.1 (Ghidra base): YuvInit 0x1040c4, YuvEncode 0x104b4c, YuvExit 0x105248, InputJpege 0x105430,
 * SetChnQpgAI_Yuv 0x1048ac, the rate-control map FUN_000e780c, i264e_reconfig_qpgai_set 0x843f4; the T32 2.2.0
 * functions (0xec740, 0xed1c8, 0xed8c4, 0xedaac, 0xecf28 in the camera library) have the same checks and codes. */
#ifndef OPENIMP_T32_YUVRC_H
#define OPENIMP_T32_YUVRC_H

#include <stdint.h>

#define YUVRC_E_PARAM  ((int)0x80040002)  /* codec not 1/2, alignment, rate-control mode, size mismatch */
#define YUVRC_E_NULL   ((int)0x80040010)  /* NULL handle */
#define YUVRC_E_NOMEM  ((int)0x80040100)  /* YuvInit: calloc of the context */
#define YUVRC_E_BS     ((int)0x80040400)  /* YuvEncode: i264e_get_bitstream failed (here: over the output buffer) */
#define YUVRC_E_ENC    ((int)0x80042000)  /* YuvEncode: no encoder / i264e_encode failed */
#define YUVRC_E_DEV    ((int)0x80048000)  /* i264e_init / i265e_init / ijpege_init failed */
#define YUVRC_PT_H264  1
#define YUVRC_PT_H265  2
#define YUVRC_RC_MODES 6                  /* FUN_000e780c: rcMode < 6 (jump table), else 0x80040002 */

/* YuvInit before the encoder init: codec 1 / 2 (log 0x22fb), width % 16 and odd height (0x2283 / 0x22c9),
 * rcMode (FUN_000e780c, 0x2150 / 0x2247), maxGop 0 (#998: FUN_000e780c, attr +76, H.264 0xd8498 beqz -> log
 * 0x2157, H.265 0xd7a74 -> 0x224e; T32 2.2.0 IMP_Encoder_YuvCheckParam 0xd45d0 the same), all 0x80040002.
 * A zero outFrmRate is no error (the vendor takes 25/1).  NULL h / encIn: the vendor dereferences them
 * (crash); here 0x80040002.  0: go on to the encoder init (failure 0x80048000). */
static inline int yuvrc_init(int have_args, int type, int w, int h, uint32_t rc_mode, uint32_t max_gop)
{
    if (!have_args || (type != YUVRC_PT_H264 && type != YUVRC_PT_H265))
        return YUVRC_E_PARAM;
    if ((w & 0xf) != 0 || (h & 1) != 0)
        return YUVRC_E_PARAM;
    if (rc_mode >= YUVRC_RC_MODES || max_gop == 0)
        return YUVRC_E_PARAM;
    return 0;
}

/* YuvEncode / YuvExit: NULL 0x80040010 (logs 0x2356 / 0x23cc); a codec word other than 1 / 2 0x80040002
 * (0x23c0 / 0x23ec).  @ours: the handle is one of this library's (the vendor reads any pointer; a foreign
 * handle with a codec word of 1 / 2 is 0x80040002 here instead of a crash).  0: a live handle. */
static inline int yuvrc_handle(int have_handle, int ours, uint32_t type)
{
    if (!have_handle)
        return YUVRC_E_NULL;
    if (type != YUVRC_PT_H264 && type != YUVRC_PT_H265)
        return YUVRC_E_PARAM;
    return ours ? 0 : YUVRC_E_PARAM;
}

/* YuvEncode: the frame's size against the one of YuvInit (0x236d / 0x239e, 0x80040002) */
static inline int yuvrc_frame(uint32_t fw, uint32_t fh, uint32_t w, uint32_t h)
{
    return fw == w && fh == h ? 0 : YUVRC_E_PARAM;
}

/* InputJpege: width % 16 or odd height 0x80040002 (log 0x242e) */
static inline int yuvrc_jpege(int w, int h)
{
    return ((w & 0xf) != 0 || (h & 1) != 0) ? YUVRC_E_PARAM : 0;
}

/* InputJpege's quality into the ijpege parameter byte +0x55 (the channel's jpegQp scale, ijpege_param_default
 * 40): q < 0 -> 10, q > 100 -> 98, else q (99 and 100 kept; the encoder's table stops at 98) */
static inline int yuvrc_jpege_q(int q)
{
    if (q < 0)
        return 10;
    return q > 100 ? 98 : q;
}

/* i264e_reconfig_qpgai_set: foreBackMode clip3(0, 2), markLvl clip3(0, 3) */
static inline uint8_t yuvrc_clip(int v, int lo, int hi)
{
    return (uint8_t)(v < lo ? lo : v > hi ? hi : v);
}

#endif
