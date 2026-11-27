/*
 * Encoder output rate below the sensor rate: the vendor frame-count keep
 * mask (#962-#968), shared by every SoC of the p2 encoder.
 *
 * Vendor libimp (all of T10/T20/T21/T23/T30/T31/T40/T41) steps one mask per
 * input frame in the encoder group update, with the sensor rate the
 * FrameSource writes on every frame (set_framesource_fps: EnableTuning,
 * Get/SetSensorFPS) as the source rate:
 *  - T30 libimp encoder_get_fps_mask 0xb6524, T20 do_channel_process_h264e
 *    0x512fc (inlined), T31 FUN_0008ece0, T40 on_encoder_group_data_update
 *    0x74ae0, T41 FUN_0004272c (audit/issue921x/);
 *  - src = frame rate words, reduced (c_reduce_fraction); out = the
 *    channel's outFrmRate; effective = out when out <= src, else src;
 *  - len = eff_den * src_num input frames, keep = eff_num * src_den of them
 *    at the positions ((len * 1000 / keep) * k + 500) / 1000; len == keep
 *    keeps every frame; len > 64 is an error (no encode);
 *  - the mask is rebuilt when src or out changes and rotated by one per
 *    frame, the frame is coded when the bit that wraps around is 1 (the
 *    first frame after a rebuild is kept).
 * The mask itself is the T23 IVDC one (#807, openimp_t23_ivdc_pace.h);
 * this adds the rational rates and the vendor's effective rate.
 */
#ifndef OPENIMP_P2_FPS_MASK_H
#define OPENIMP_P2_FPS_MASK_H

#include <stdint.h>
#include "t23/openimp_t23_ivdc_pace.h"

typedef struct {
    T23IvdcPace pace;       /* mask + len (len 0: keep every frame) */
    uint32_t src_num, src_den;
    uint32_t out_num, out_den;
    int state;              /* 0 none, 1 mask, -1 not usable (len > 64) */
} P2FpsMask;

static inline uint32_t p2_fps_gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

static inline void p2_fps_reduce(uint32_t *num, uint32_t *den)
{
    uint32_t g = p2_fps_gcd(*num, *den);

    if (g > 1u) {
        *num /= g;
        *den /= g;
    }
}

/* Rates of this frame.  Returns 1 when the mask decides (output below the
 * source rate), 0 when every frame is coded at the source rate (out >= src)
 * and -1 when the vendor rule does not apply (a rate is 0 or the window is
 * longer than 64 frames: the caller keeps its own pacing). */
static inline int p2_fps_mask_update(P2FpsMask *m, uint32_t src_num,
                                     uint32_t src_den, uint32_t out_num,
                                     uint32_t out_den)
{
    uint32_t eff_num, eff_den, len, keep;

    if (!src_num || !src_den || !out_num || !out_den) {
        m->state = -1;
        m->src_num = 0;
        return -1;
    }
    p2_fps_reduce(&src_num, &src_den);
    p2_fps_reduce(&out_num, &out_den);
    if (m->state && src_num == m->src_num && src_den == m->src_den &&
        out_num == m->out_num && out_den == m->out_den)
        return m->state;
    m->src_num = src_num;
    m->src_den = src_den;
    m->out_num = out_num;
    m->out_den = out_den;
    if ((uint64_t)src_num * out_den <= (uint64_t)out_num * src_den) {
        /* out >= src: the vendor codes every frame (len == keep) */
        m->pace.len = 0;
        m->pace.mask = 0;
        m->state = 0;
        return 0;
    }
    eff_num = out_num;
    eff_den = out_den;
    if ((uint64_t)eff_den * src_num > 64u) {
        m->pace.len = 0;
        m->pace.mask = 0;
        m->state = -1;
        return -1;
    }
    len = eff_den * src_num;
    keep = eff_num * src_den;
    m->pace.len = len;
    m->pace.mask = t23_ivdc_rate_mask(len, keep);
    m->pace.in_fps = len;
    m->pace.out_fps = keep;
    m->state = 1;
    return 1;
}

/* One input frame: 1 = code it, 0 = skip it. */
static inline int p2_fps_mask_take(P2FpsMask *m)
{
    if (m->state != 1)
        return 1;
    return t23_ivdc_pace_take(&m->pace);
}

#endif /* OPENIMP_P2_FPS_MASK_H */
