/* AVPU macroblock QP table for the ROI windows (IMP_Encoder_SetChnRoiAttr,
 * T31 and the experimental T41 path).
 *
 * The AVPU turns the table entry into the macroblock QP and the H.264
 * encoder then writes mb_qp_delta = QP(MB) - QP(previous MB in coding
 * order), which the syntax limits to -26..+25.  Skipped macroblocks keep the
 * previous QP.  A decoder that checks the range (VA-API, VLC, browsers) shows
 * broken blocks otherwise; a software decoder hides it.  So the table must
 * guarantee, for every pair of macroblocks that can follow each other:
 *   - every entry is a delta in -25..+25 (an entry of -26 against the
 *     neighbours at 0 would need +26 on the way out),
 *   - the spread between the highest and the lowest entry (uncovered
 *     macroblocks count as 0) is at most 25,
 *   - picture QP + entry stays inside 0..51 and the rate control's
 *     min_qp..max_qp (positive deltas above max_qp are only ignored by the
 *     hardware, but then the stream QP is not what the caller asked for).
 * Windows that ask for more are clamped (quality wins: when the spread is
 * too large the higher QPs are lowered).  Absolute windows are first turned
 * into a delta against the picture QP.
 *
 * Pure function, no hardware access: tests/t31/roi_clamp_test.c. */
#ifndef OPENIMP_AVPU_ROI_H
#define OPENIMP_AVPU_ROI_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t enable;
    uint8_t mode;   /* 0 relative (delta), 1 absolute (T31) */
    int8_t qp;
    uint32_t x, y, w, h;
} AvpuRoiWin;

#define AVPU_ROI_DELTA_MIN (-25)
#define AVPU_ROI_DELTA_MAX 25
#define AVPU_ROI_SPREAD_MAX 25

/* result bits of avpu_roi_fill */
#define AVPU_ROI_CLAMP_DELTA  0x1u  /* beyond -25..25 */
#define AVPU_ROI_CLAMP_RANGE  0x2u  /* picture QP + delta outside 0..51 or min/max QP */
#define AVPU_ROI_CLAMP_SPREAD 0x4u  /* highest/lowest entry more than 25 apart */

typedef struct {
    int req_min, req_max;   /* requested deltas before clamping (all windows) */
    unsigned int clamped;   /* AVPU_ROI_CLAMP_* */
} AvpuRoiResult;

/* The delta window [*lo, *hi] for a picture QP.  base_qp 0 = unknown: only
 * the -25..25 limit applies. */
static inline void avpu_roi_limits(uint32_t base_qp, uint32_t min_qp,
                                   uint32_t max_qp, int *lo, int *hi)
{
    int l = AVPU_ROI_DELTA_MIN, h = AVPU_ROI_DELTA_MAX;

    if (base_qp >= 1u && base_qp <= 51u) {
        int qmin = (int)(min_qp <= 51u ? min_qp : 0u);
        int qmax = (int)(max_qp >= 1u && max_qp <= 51u ? max_qp : 51u);

        if (qmin > qmax)
            qmin = 0;
        if (qmin - (int)base_qp > l)
            l = qmin - (int)base_qp;
        if (qmax - (int)base_qp < h)
            h = qmax - (int)base_qp;
        if (l > 0)      /* picture QP itself outside the range */
            l = 0;
        if (h < 0)
            h = 0;
    }
    *lo = l;
    *hi = h;
}

static inline int avpu_roi_get(const uint8_t *table, uint32_t stride,
                               uint32_t idx, unsigned int bits)
{
    unsigned int sh = 32u - bits;

    return (int)((uint32_t)table[idx * stride] << sh) >> sh;
}

/* One entry of a vendor T41 IMP_Encoder_SetChnMapRoi map: low 2 bits the
 * mode (0 none, 1 the block takes `quality`, 2 relative with the high 6
 * bits as two's complement), clamped to -25..25.  Returns 0 and the delta,
 * or -1 for mode 3 (undefined). */
static inline int avpu_roi_map_delta(uint8_t entry, int quality, int *delta)
{
    int d = 0;

    switch (entry & 3u) {
    case 0u:
        break;
    case 1u:
        d = quality;
        break;
    case 2u:
        d = (int)(entry >> 2);
        if (d > 31)
            d -= 64;
        break;
    default:
        return -1;
    }
    if (d < AVPU_ROI_DELTA_MIN)
        d = AVPU_ROI_DELTA_MIN;
    else if (d > AVPU_ROI_DELTA_MAX)
        d = AVPU_ROI_DELTA_MAX;
    *delta = d;
    return 0;
}

/* Limit the highest entry to lowest + 25 (uncovered macroblocks are 0):
 * H.264 mb_qp_delta between neighbours must stay within +-25.  Run it
 * again after anything else wrote entries (SetChnMapRoi). */
static inline void avpu_roi_spread_clamp(uint8_t *table, uint32_t stride,
                                         unsigned int bits, uint32_t mb,
                                         AvpuRoiResult *res)
{
    uint32_t emask = (1u << bits) - 1u, i;
    int amin = 0, amax = 0;

    for (i = 0; i < mb; i++) {
        int v = avpu_roi_get(table, stride, i, bits);

        if (v < amin)
            amin = v;
        if (v > amax)
            amax = v;
    }
    if (amax - amin > AVPU_ROI_SPREAD_MAX) {
        int cap = amin + AVPU_ROI_SPREAD_MAX;

        res->clamped |= AVPU_ROI_CLAMP_SPREAD;
        for (i = 0; i < mb; i++)
            if (avpu_roi_get(table, stride, i, bits) > cap)
                table[i * stride] = (uint8_t)((uint32_t)cap & emask);
    }
}

/* Fill cols*rows entries (entry i at table[i * stride], stride 1 for T31,
 * 4 for the T41 byte 0) from the windows.  bits: width of the two's
 * complement entry (6 for T31, 8 for T41).  A later window wins on overlap.
 * Entries are cleared first (only byte 0 of a stride is touched). */
static inline void avpu_roi_fill(uint8_t *table, uint32_t stride,
                                 unsigned int bits, uint32_t cols,
                                 uint32_t rows, const AvpuRoiWin *win,
                                 unsigned int nwin, uint32_t base_qp,
                                 uint32_t min_qp, uint32_t max_qp,
                                 int round_nearest, AvpuRoiResult *res)
{
    uint32_t mb = cols * rows, i, y, x, n;
    uint32_t emask = (1u << bits) - 1u;
    int lo, hi;

    res->req_min = res->req_max = 0;
    res->clamped = 0;
    avpu_roi_limits(base_qp, min_qp, max_qp, &lo, &hi);
    for (i = 0; i < mb; i++)
        table[i * stride] = 0;
    for (n = 0; n < nwin; n++) {
        uint32_t x0, y0, x1, y1;
        int d;

        if (!win[n].enable)
            continue;
        d = win[n].qp;
        if (win[n].mode)
            d -= (int)base_qp;
        if (d < res->req_min)
            res->req_min = d;
        if (d > res->req_max)
            res->req_max = d;
        if (d < AVPU_ROI_DELTA_MIN) {
            d = AVPU_ROI_DELTA_MIN;
            res->clamped |= AVPU_ROI_CLAMP_DELTA;
        } else if (d > AVPU_ROI_DELTA_MAX) {
            d = AVPU_ROI_DELTA_MAX;
            res->clamped |= AVPU_ROI_CLAMP_DELTA;
        }
        if (d < lo || d > hi) {
            d = d < lo ? lo : hi;
            res->clamped |= AVPU_ROI_CLAMP_RANGE;
        }
        x0 = win[n].x >> 4;
        y0 = win[n].y >> 4;
        if (round_nearest) {    /* vendor T41: size to the nearest block */
            x1 = x0 + ((win[n].w + 8u) >> 4);
            y1 = y0 + ((win[n].h + 8u) >> 4);
        } else {                /* every macroblock the window touches */
            x1 = (win[n].x + win[n].w + 15u) >> 4;
            y1 = (win[n].y + win[n].h + 15u) >> 4;
        }
        for (y = y0; y < y1 && y < rows; y++)
            for (x = x0; x < x1 && x < cols; x++)
                table[(y * cols + x) * stride] = (uint8_t)((uint32_t)d & emask);
    }
    avpu_roi_spread_clamp(table, stride, bits, mb, res);
}

#endif
