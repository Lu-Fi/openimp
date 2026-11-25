/* T32/T33 stand-alone ("unbound") encoder contexts (#752): the back end of IMP_Encoder_YuvInit / YuvEncode /
 * YuvExit and IMP_Encoder_InputJpege (openimp_t32_enc_e3.c, vendor 2.x headers).  A context is a hidden
 * encoder channel of openimp_t32_encoder.c (no channel number, no group, no FrameSource, no thread): the same
 * HERA H.264 / H.265 and JPEG code paths as the bound channels, coding the caller's NV12 frame synchronously. */
#ifndef OPENIMP_T32_YUV_H
#define OPENIMP_T32_YUV_H

#include <stddef.h>
#include <stdint.h>

#define T32_YUV_JPEG 0                  /* PT_JPEG */
#define T32_YUV_H264 1                  /* PT_H264 */
#define T32_YUV_H265 2                  /* PT_H265 */

typedef struct {
    int codec;                          /* T32_YUV_* */
    uint32_t width, height;
    const void *rc;                     /* IMPEncoderAttrRcMode (64 bytes, H.264 / H.265) */
    size_t rc_size;                     /* sizeof of the caller's type: checked against the encoder's */
    uint32_t fps_num, fps_den, gop;
    int jpeg_qp;                        /* JPEG: 0..98 (the channel's jpegQp scale) */
} T32YuvCfg;

/* a context, or NULL (no VPU channel / memory / unsupported size) */
void *OpenIMP_T32_EncYuvOpen(const T32YuvCfg *c);

/* code one NV12 picture (Y at @y_pa / @y_va, UV after the even height, stride ALIGN16(w); @y_va is written back
 * from the cache first when set) into @out; @cap 0: no limit known.  0, T32_YUV_E_CODE (the engine failed)
 * or T32_YUV_E_ROOM (the picture is over @cap) */
#define T32_YUV_E_CODE (-1)
#define T32_YUV_E_ROOM (-2)
int OpenIMP_T32_EncYuvFrame(void *ctx, const void *y_va, uint32_t y_pa, uint32_t w, uint32_t h, int64_t ts,
                            void *out, uint32_t cap, uint32_t *len);

void OpenIMP_T32_EncYuvClose(void *ctx);
void OpenIMP_T32_EncYuvSetMq(void *ctx, int on);   /* #784 MotionQuality_Yuv */

#endif
