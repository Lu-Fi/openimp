/* T31 IMP_Encoder_GetChnEvalInfo record (AVC), 36 bytes laid out as the
 * public IMPEncoderStreamInfo of the vendor header (imp_encoder.h 1.1.x):
 *   +0  int32       iNumBytes    entropy bytes of the picture (completion
 *                                status +0x104, 30 bit; the published access
 *                                unit adds the generated Annex-B headers)
 *   +4  uint32      uNumIntra    \
 *   +8  uint32      uNumSkip      |  merged slice status +0x1c..+0x30
 *   +12 uint32      uNumCU8x8     |  (status regs 0x114, 0x11c, 0x120,
 *   +16 uint32      uNumCU16x16   |  0x124, 0x128, u16 0x12c; summed over
 *   +20 uint32      uNumCU32x32   |  the slices)
 *   +24 uint32      uNumCU64x64  /
 *   +28 int16       iSliceQP     slice QP of the picture
 *   +30 int16       iMinQP       lowest QP  (status +0x3e, InitSliceStatus starts at 32767)
 *   +32 int16       iMaxQP       highest QP (status +0x40)
 *   +34 .. +35      zero (struct padding)
 *
 * Beyond vendor: the vendor libimp 1.1.6 (update_one_frmstrm) copies slot
 * bytes 356..391, one word after the slot's iNumBytes at 352, so its record
 * starts with uNumIntra and every field sits 4 bytes before its declared
 * offset (iNumBytes is missing, the QPs land in uNumCU64x64 / iSliceQP).
 * OpenIMP returns the header layout.  A streamer that compensated for the
 * vendor shift must not do so with OpenIMP.
 *
 * JPEG record (8 bytes, same principle): +0 int32 iNumBytes (JPEG size),
 * +4 int16 quality factor (where the vendor's shifted record has it at +0),
 * +6 zero. */
#ifndef T31_EVAL_INFO_H
#define T31_EVAL_INFO_H

#include <stdint.h>
#include <string.h>

#define T31_EVAL_SIZE        36
#define T31_EVAL_JPEG_SIZE    8
#define T31_SLICE_STATUS_SIZE 0x70
#define T31_SLICE_MIN_QP_INIT 32767

/* a merged slice status block as InitSliceStatus leaves it (before the first Merge) */
static inline void t31_eval_status_init(uint8_t *merged)
{
    uint16_t mn = T31_SLICE_MIN_QP_INIT;

    memset(merged, 0, T31_SLICE_STATUS_SIZE);
    memcpy(merged + 0x3e, &mn, sizeof(mn));
}

static inline void t31_eval_pack(const uint8_t *merged, uint32_t num_bytes,
                                 uint16_t slice_qp, uint8_t *out)
{
    int32_t nb = (int32_t)num_bytes;
    uint16_t mn, mx;

    memset(out, 0, T31_EVAL_SIZE);
    memcpy(out, &nb, 4);
    memcpy(out + 4, merged + 0x1c, 24);
    memcpy(&mn, merged + 0x3e, sizeof(mn));
    memcpy(&mx, merged + 0x40, sizeof(mx));
    memcpy(out + 28, &slice_qp, 2);
    memcpy(out + 30, &mn, 2);
    memcpy(out + 32, &mx, 2);
}

/* JPEG channel record (T31_EVAL_JPEG_SIZE bytes) */
static inline void t31_eval_pack_jpeg(uint32_t num_bytes, int16_t quality,
                                      uint8_t *out)
{
    int32_t nb = (int32_t)num_bytes;

    memset(out, 0, T31_EVAL_JPEG_SIZE);
    memcpy(out, &nb, 4);
    memcpy(out + 4, &quality, 2);
}

#endif
