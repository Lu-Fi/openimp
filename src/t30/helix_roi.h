#ifndef OPENIMP_HELIX_ROI_H
#define OPENIMP_HELIX_ROI_H

#include <stdint.h>

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
    entry[2] = (uint8_t)(int8_t)qp;
    entry[3] = (uint8_t)(x0 / 16);
    entry[4] = (uint8_t)(x1 / 16);
    entry[5] = (uint8_t)(y0 / 16);
    entry[6] = (uint8_t)(y1 / 16);
}

#endif
