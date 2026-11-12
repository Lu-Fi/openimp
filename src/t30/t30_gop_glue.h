/*
 * T30 1.0.5 IMP glue GOP arithmetic (encoder_get_fps_mask, #367), shared by
 * the H.264 (Helix) and H.265 (Radix) encoders: the vendor runs the same code
 * for both channel types and hands frame rate and GOP to i264e / i265e as
 * parameters 5 / 7.
 *
 * - CreateChn: the GOP in whole seconds of the reduced rate (gop * den / num
 *   truncated, times num / den rounded up; GOP 30 at 25 fps -> 25).
 * - A new frame rate keeps the whole seconds of the running GOP.
 * - A GOP asked for (SetGOPSize, channel +0xd8) is rounded up to a multiple
 *   of num * den of the reduced rate (30 at 25/1 -> 50) and stored back.
 * - Every rate or GOP sent sets channel +0xe0, which makes the next coded
 *   picture an IDR (H.264: picture +8 -> fenc +0x44,
 *   i264e_decide_slice_type_and_rd; H.265: picture +0x18).
 *
 * Under one second the vendor reaches GOP 0 and divides by it (SIGFPE);
 * OpenIMP keeps the GOP in pictures (docs/BEYOND_VENDOR.md, pending user OK).
 */
#ifndef OPENIMP_T30_GOP_GLUE_H
#define OPENIMP_T30_GOP_GLUE_H

#include <stdint.h>

typedef struct {
    uint32_t vgop;          /* the GOP handed to the encoder (+0x40) */
    uint32_t vgop_req;      /* the GOP asked for (+0xd8) */
    uint32_t gop_hw;        /* the last IMP value seen */
    uint32_t gop_requests;  /* and its request count */
} T30GopGlue;

static inline void t30_gop_reduce(uint32_t *num, uint32_t *den)
{
    uint32_t a = *num, b = *den, t;

    while (b) {
        t = a % b;
        a = b;
        b = t;
    }
    if (a) {
        *num /= a;
        *den /= a;
    }
}

static inline uint32_t t30_gop_rescale(uint32_t gop, uint32_t on, uint32_t od,
                                       uint32_t nn, uint32_t nd)
{
    uint32_t frames = (on ? gop * od / on : 0u) * nn;
    uint32_t g = nd ? frames / nd + (frames % nd != 0u) : 0u;

    /* under one second the vendor gets GOP 0 (SIGFPE); keep the pictures */
    return g ? g : gop;
}

static inline uint32_t t30_gop_round(uint32_t gop, uint32_t num, uint32_t den)
{
    uint32_t unit = num * den;
    uint32_t g = unit && gop % unit ? (gop / unit + 1u) * unit : gop;

    return g ? g : gop;
}

/* CreateChn: returns the GOP the encoder starts with */
static inline uint32_t t30_gop_glue_create(T30GopGlue *s, uint32_t gop,
                                           uint32_t requests, uint32_t num,
                                           uint32_t den)
{
    t30_gop_reduce(&num, &den);
    s->gop_hw = gop;
    s->gop_requests = requests;
    s->vgop = t30_gop_rescale(gop, num, den, num, den);
    s->vgop_req = s->vgop;
    return s->vgop;
}

/* before a picture, from the rate the encoder runs with (on/od) to the one
 * asked for (nn/nd) and the IMP GOP request: the GOP sent to the encoder
 * (which then forces an IDR), or 0 when nothing is sent */
static inline uint32_t t30_gop_glue_step(T30GopGlue *s, uint32_t on,
                                         uint32_t od, uint32_t nn,
                                         uint32_t nd, uint32_t requested,
                                         uint32_t requests)
{
    uint32_t g = 0u;

    t30_gop_reduce(&on, &od);
    t30_gop_reduce(&nn, &nd);
    if (requested && (requested != s->gop_hw || requests != s->gop_requests)) {
        s->gop_hw = requested;
        s->gop_requests = requests;
        s->vgop_req = requested;
    }
    if ((uint64_t)on * nd != (uint64_t)nn * od) {
        g = t30_gop_rescale(s->vgop, on, od, nn, nd);
        s->vgop_req = g;
    } else if (s->vgop != s->vgop_req) {
        g = t30_gop_round(s->vgop_req, nn, nd);
        s->vgop_req = g;
    }
    if (g)
        s->vgop = g;
    return g;
}

#endif
