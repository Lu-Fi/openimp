/*
 * T23 IVDC direct mode: when an encoder channel kicks its job (#807).
 * Pure helpers, shared with tests/t23/ivdc_pace_test.c.
 *
 * Vendor libimp 1.3.0 (audit/t23dl-alfonso/v/libimp_all.c):
 *  - push, not timer: the frame-start event (driver event 0x3000006 for
 *    channel 0, tx-isp-t23.ko ko_all.c:54208-54224) reaches the encoder
 *    through on_encoder_group_data_update (:34169) and a semaphore the
 *    channel thread waits on (:34042); the job is kicked when it arrives;
 *  - the output rate comes from a frame-count mask (do_channel_process
 *    :33612-33640): over len input frames, out of them are kept, at the
 *    positions ((len * 1000 / out) * k + 500) / 1000; the mask is read at
 *    bit len-1 and rotated by one per input frame (:33687ff).  30 -> 25:
 *    one of six frames is skipped, none is kicked late.
 * OpenIMP paced every channel by its own 40 ms timer, which beats against
 * the 33.3 ms frames: the kick moved 150 lines per frame and repeated every
 * 200 ms (FINDINGS-fable2.md).
 */
#ifndef OPENIMP_T23_IVDC_PACE_H
#define OPENIMP_T23_IVDC_PACE_H

#include <stdint.h>

typedef struct {
    uint64_t mask;
    unsigned int len;      /* input frames per window (<= 64), 0 = keep all */
    unsigned int in_fps;   /* what the mask was built for */
    unsigned int out_fps;
    uint64_t last_ts;      /* previous input frame time (us) */
    uint64_t period_us;    /* smoothed input frame period (us) */
} T23IvdcPace;

/* vendor mask: len input frames, out kept */
static inline uint64_t t23_ivdc_rate_mask(unsigned int len, unsigned int out)
{
    uint64_t m = 0;
    unsigned int i, k = 0, next = 0;

    if (!len || !out)
        return 0;
    for (i = 0; i < len; i++) {
        m <<= 1;
        if (i == next) {
            k++;
            m |= 1u;
            next = (((len * 1000u) / out) * k + 500u) / 1000u;
        }
    }
    return m;
}

static inline void t23_ivdc_pace_build(T23IvdcPace *p, unsigned int in_fps,
                                       unsigned int out_fps)
{
    p->in_fps = in_fps;
    p->out_fps = out_fps;
    if (!in_fps || !out_fps || out_fps >= in_fps || in_fps > 64u) {
        p->len = 0;
        p->mask = 0;
        return;
    }
    p->len = in_fps;
    p->mask = t23_ivdc_rate_mask(in_fps, out_fps);
}

/* one input frame: 1 = code it, 0 = skip it (released without a kick) */
static inline int t23_ivdc_pace_take(T23IvdcPace *p)
{
    uint64_t top, all;

    if (!p->len)
        return 1;
    top = (p->mask >> (p->len - 1u)) & 1u;
    all = p->len >= 64u ? ~0ull : ((1ull << p->len) - 1u);
    p->mask = ((p->mask << 1) | top) & all;
    return (int)top;
}

/* Input rate from the frame-start times (the driver completes channel 0 at
 * every ISP frame start; the FrameSource channel rate is not that rate).
 * Rebuilds the mask when the rounded rate changes.  Returns the rate. */
static inline unsigned int t23_ivdc_pace_input(T23IvdcPace *p, uint64_t ts_us,
                                               unsigned int out_fps)
{
    unsigned int fps;

    if (p->last_ts && ts_us > p->last_ts) {
        uint64_t d = ts_us - p->last_ts;

        if (d < 1000000u) {
            /* ignore gaps (skipped events, start): only near-period steps */
            if (!p->period_us)
                p->period_us = d;
            else if (d < p->period_us * 3u / 2u)
                p->period_us = (p->period_us * 7u + d) / 8u;
        }
    }
    p->last_ts = ts_us;
    if (!p->period_us)
        return p->in_fps;
    fps = (unsigned int)((1000000u + p->period_us / 2u) / p->period_us);
    if (fps != p->in_fps || out_fps != p->out_fps)
        t23_ivdc_pace_build(p, fps, out_fps);
    return fps;
}

#endif
