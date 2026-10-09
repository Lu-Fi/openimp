#ifndef OPENIMP_HELIX_ROI_H
#define OPENIMP_HELIX_ROI_H

#include <stdint.h>
#include <string.h>

#include "../avpu_roi.h"

/*
 * ROI registers of the Ingenic T10/T20/T21 H.264 EFE, as the OEM
 * H264E_T10_SliceInit / H264E_T20_SliceInit / H264E_T21_SliceInit (T20
 * 3.12.0, T21 1.0.33) encode the i264e ROI table: per region 7 bytes
 * {enable, relative, qp (s8), x0, x1, y0, y1} (macroblocks, as
 * IMP_Encoder_SetChnROI computes them).
 *
 *   words[0] = 0x40044: regions 0..3, one byte each, qp << 2 | rel << 1 | en
 *   words[1] = 0x40048: regions 4..7
 *   words[2 + i] = 0x4004c + 4 * i: x0 | x1 << 8 | y0 << 16 | y1 << 24
 */
static inline void Helix_H264_RoiRegisters(const uint8_t roi[8][7],
                                           uint32_t words[10])
{
    unsigned int i;

    words[0] = 0;
    words[1] = 0;
    for (i = 0; i < 8u; i++) {
        const uint8_t *r = roi[i];
        uint32_t flags = (((uint32_t)r[2] << 2) |
                          ((uint32_t)(r[1] & 1u) << 1) |
                          (uint32_t)(r[0] & 1u)) & 0xffu;

        words[i >> 2] |= flags << (8u * (i & 3u));
        words[2 + i] = (uint32_t)r[3] | ((uint32_t)r[4] << 8) |
                       ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 24);
    }
}

/* Validity of the ROI QP (same reason as src/avpu_roi.h): the EFE codes
 * mb_qp_delta = QP(MB) - QP(previous MB), H.264 allows -26..+25 only.
 * Measured on T10/T20 (docs/ROI.md): relative -26, or an absolute QP more
 * than 25 away from the macroblock QP (absolute 15 at QP 42) gives a stream
 * that VA-API/VLC/browsers show as broken blocks; ffmpeg hides it.
 * Applied when the command list is built, with the slice QP of that picture:
 *   - relative QP -25..+25,
 *   - absolute QP inside [max_qp - 25, min_qp + 25] (every macroblock QP
 *     the rate control can produce is then at most 25 away) and 0..51,
 *   - the spread of all windows' deltas (absolute: against the slice QP;
 *     uncovered = 0) at most 25, the higher QPs are lowered.
 * min_qp/max_qp: the picture's QP range (slice QP -12/+13 in the T21/T30/
 * T10 command lists, the cap of the T20).  Returns AVPU_ROI_CLAMP_* bits;
 * out may equal in. */
static inline unsigned int Helix_H264_RoiSanitize(const uint8_t in[8][7],
                                                  uint8_t out[8][7],
                                                  int slice_qp, int min_qp,
                                                  int max_qp)
{
    unsigned int flags = 0, i;
    int d[8], lo = 0, hi = 0, cap, alo, ahi;

    if (out != in)
        memcpy(out, in, 8u * 7u);
    alo = max_qp - AVPU_ROI_SPREAD_MAX;
    ahi = min_qp + AVPU_ROI_SPREAD_MAX;
    if (alo < 0)
        alo = 0;
    if (ahi > 51)
        ahi = 51;
    if (alo > ahi)      /* range wider than 25: nothing satisfies both */
        alo = ahi = slice_qp;
    for (i = 0; i < 8u; i++) {
        int q = (int8_t)in[i][2];

        d[i] = 0;
        if (!(in[i][0] & 1u))
            continue;
        if (in[i][1] & 1u) {
            if (q < AVPU_ROI_DELTA_MIN || q > AVPU_ROI_DELTA_MAX) {
                q = q < 0 ? AVPU_ROI_DELTA_MIN : AVPU_ROI_DELTA_MAX;
                flags |= AVPU_ROI_CLAMP_DELTA;
            }
            d[i] = q;
        } else {
            if (q < alo || q > ahi) {
                q = q < alo ? alo : ahi;
                flags |= AVPU_ROI_CLAMP_RANGE;
            }
            d[i] = q - slice_qp;
        }
        if (d[i] < lo)
            lo = d[i];
        if (d[i] > hi)
            hi = d[i];
    }
    cap = lo + AVPU_ROI_SPREAD_MAX;
    if (hi > cap) {
        flags |= AVPU_ROI_CLAMP_SPREAD;
        for (i = 0; i < 8u; i++)
            if ((in[i][0] & 1u) && d[i] > cap)
                d[i] = cap;
    }
    for (i = 0; i < 8u; i++) {
        if (!(in[i][0] & 1u))
            continue;
        out[i][2] = (uint8_t)(int8_t)((in[i][1] & 1u) ? d[i]
                                                       : slice_qp + d[i]);
    }
    return flags;
}

/* The i264e ROI table entry IMP_Encoder_SetChnROI makes of an
 * IMPEncoderROICfg (T20 3.12.0 0x4899c, T21 1.0.33 0x467d0): corners
 * sorted, divided by 16 with C (truncating) division, kept as bytes. */
static inline void Helix_H264_RoiEntry(int enable, int relative, int qp,
                                       int x_a, int y_a, int x_b, int y_b,
                                       uint8_t entry[7])
{
    int x0 = x_a < x_b ? x_a : x_b, x1 = x_a < x_b ? x_b : x_a;
    int y0 = y_a < y_b ? y_a : y_b, y1 = y_a < y_b ? y_b : y_a;

    entry[0] = (uint8_t)enable;
    entry[1] = (uint8_t)relative;
    entry[2] = (uint8_t)(int8_t)(qp < -128 ? -128 : qp > 127 ? 127 : qp);
    entry[3] = (uint8_t)(x0 / 16);
    entry[4] = (uint8_t)(x1 / 16);
    entry[5] = (uint8_t)(y0 / 16);
    entry[6] = (uint8_t)(y1 / 16);
}

#endif
