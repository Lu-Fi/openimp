/* T23 encoder rate below the sensor rate (#986).
 *
 * The vendor T23 1.3.0 do_channel_process (0x4a248..0x4a300, 0x4aed0,
 * 0x4b234, 0x4b508) compares, before every picture, the sensor rate the
 * FrameSource wrote on the frame (channel +212/+216) with the configured
 * output rate (+220/+224).  The encoder rate (+56/+60) becomes the lower of
 * the two (c_reduce_fraction); when it changes, the GOP (+64) keeps its
 * duration in whole seconds:
 *     seconds = gop * den_old / num_old            (integer division)
 *     gop     = ceil(seconds * num_new / den_new)
 * and both go to i264e (i264e_set_param FPS 5 / GOP 7).  So a 25 fps
 * channel on a 15 fps sensor codes at 15 fps for the rate control and
 * with an IDR every 15 pictures at GOP 25 (every 30 at GOP 50).  The
 * user-visible attributes keep the configured values.
 *
 * Deviation: a GOP shorter than one second gives seconds = 0 and the
 * vendor would pass GOP 0 to i264e; here the GOP is then left unchanged.
 */
#ifndef OPENIMP_T23_ENC_RATE_H
#define OPENIMP_T23_ENC_RATE_H

#include <stdint.h>

#include "t40/p2_fps_mask.h"    /* p2_fps_reduce (c_reduce_fraction) */

typedef struct {
    uint32_t num, den;          /* encoder rate (vendor +56/+60) */
    uint32_t gop;               /* encoder GOP (vendor +64) */
    uint32_t src_num, src_den;  /* last sensor rate seen (+212/+216) */
    int valid;
} T23EncRate;

/* CreateChn, and again after SetChnFrmRate / SetChnGopLength: the encoder
 * starts from the configured rate and GOP; the next picture derives the
 * effective values as on the first picture of the vendor channel (its
 * +212/+216 start at 0, so the first picture always runs the rule). */
static inline void t23_enc_rate_init(T23EncRate *r, uint32_t out_num,
                                     uint32_t out_den, uint32_t gop)
{
    r->num = out_num ? out_num : 25u;
    r->den = out_den ? out_den : 1u;
    r->gop = gop;
    r->src_num = 0;
    r->src_den = 0;
    r->valid = 1;
}

/* One picture with the frame's sensor rate: 1 when the encoder rate or
 * GOP was set again (the caller hands both to the codec). */
static inline int t23_enc_rate_step(T23EncRate *r, uint32_t src_num,
                                    uint32_t src_den, uint32_t out_num,
                                    uint32_t out_den)
{
    uint32_t num, den, seconds, q;

    if (!r->valid || !src_num || !src_den || !out_num || !out_den ||
        !r->num || !r->den)
        return 0;
    p2_fps_reduce(&src_num, &src_den);
    if (r->src_num && src_num * r->src_den == src_den * r->src_num &&
        out_num * r->den == out_den * r->num)
        return 0;               /* same sensor rate, output = encoder rate */
    r->src_num = src_num;
    r->src_den = src_den;
    seconds = r->den * r->gop / r->num;
    if (src_num * out_den < out_num * src_den) {
        /* sensor below the output rate: code at the sensor rate */
        if (src_den * r->num == r->den * src_num)
            return 0;
        num = src_num;
        den = src_den;
    } else {
        num = out_num;
        den = out_den;
    }
    p2_fps_reduce(&num, &den);
    r->num = num;
    r->den = den;
    q = seconds * num / den;
    if (seconds * num % den)
        q++;
    if (q)
        r->gop = q;
    return 1;
}

#endif
