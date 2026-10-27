/* T31 IMP_Encoder_GetChnEvalInfo record (AVC): the 36 bytes of the vendor
 * libimp 1.1.6 (update_one_frmstrm copies slot bytes 356..391, which the
 * Allegro EndEncoding fills from the merged slice status):
 *   +0  6 x uint32  status +0x1c,0x20,0x24,0x28,0x2c,0x30 (hardware picture
 *                   counters of the status block: regs 0x114,0x11c,0x120,
 *                   0x124,0x128, u16 0x12c; summed over the slices)
 *   +24 int16       slice QP of the picture
 *   +26 int16       lowest QP   (status +0x3e, InitSliceStatus starts at 32767)
 *   +28 int16       highest QP  (status +0x40)
 *   +30 .. +35      zero
 * (The public IMPEncoderStreamInfo starts one word earlier, with iNumBytes at
 * slot byte 352; the vendor copy is shifted by that word. Bytes as the vendor.) */
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

static inline void t31_eval_pack(const uint8_t *merged, uint16_t slice_qp, uint8_t *out)
{
    uint16_t mn, mx;

    memset(out, 0, T31_EVAL_SIZE);
    memcpy(out, merged + 0x1c, 24);
    memcpy(&mn, merged + 0x3e, sizeof(mn));
    memcpy(&mx, merged + 0x40, sizeof(mx));
    memcpy(out + 24, &slice_qp, 2);
    memcpy(out + 26, &mn, 2);
    memcpy(out + 28, &mx, 2);
}

#endif
