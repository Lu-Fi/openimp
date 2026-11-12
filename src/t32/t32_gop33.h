/*
 * T32 2.2.0 IMP glue: frame rate and GOP handed to the encoder (#295), as
 * the vendor encoder_get_fps_mask does before every picture for H.264
 * (i264e_set_param 5 / 7) and H.265 (i265e_set_param 5 / 7).
 *
 * - The encoder runs at the channel rate asked for (IMP_Encoder_SetChnFrmRate,
 *   reduced), at most the source rate.  A new rate is sent and the next
 *   picture is an IDR; the GOP in pictures stays, unless a GOP was asked for
 *   within the last two pictures: then the whole seconds of the running GOP
 *   (den * gop / num, truncated) times the new rate (truncated).
 * - A GOP asked for (IMP_Encoder_SetGOPSize, also the created one) becomes
 *   ceil(den * gop / num) * num / den (truncated): whole seconds of the rate
 *   the encoder runs with; it is always sent, with an IDR.
 * - The first picture sends both (the source rate is new) and is an IDR.
 *
 * Under one second the vendor reaches GOP 0 there and traps at the next
 * IDR (i264e_ratecontrol_start divides by it); OpenIMP keeps the GOP in
 * pictures (T32_GOP33_ZERO, docs/OPENIMP_BEYOND_VENDOR.md, pending user OK).
 *
 * The encoder takes rate and GOP at that IDR (i264e_idr_reconfig: fps
 * +0x2a4.., keyint +0x2c, then i264e_ratecontrol_init).  The vendor getters
 * (i264e_reconfig_fps_get / _gop_get) show what was sent.
 * tests/t32/gop33_test.c checks this against tools/eprc_t33_gop_oracle.py.
 */
#ifndef OPENIMP_T32_GOP33_H
#define OPENIMP_T32_GOP33_H

#include <stdint.h>

typedef struct {
    uint32_t num, den, gop;     /* what the encoder was sent (+0x38, +0x3c, +0x40) */
    uint32_t src_num, src_den;  /* the source rate (+0xe4, +0xe8), 0: not seen yet */
    uint32_t user_num, user_den;/* SetChnFrmRate (+0xec, +0xf0) */
    uint32_t gop_req;           /* SetGOPSize, 0: none (+0xf4) */
    uint64_t mark;              /* the picture count at SetGOPSize (+0xf8) */
    uint64_t pics;              /* pictures coded (+0x108) */
} T32Gop33;

static inline void t32_gop33_reduce(uint32_t *num, uint32_t *den)
{
    uint32_t a = *num, b = *den, t;

    while (b) {
        t = a % b;
        a = b;
        b = t;
    }
    if (a > 1) {
        *num /= a;
        *den /= a;
    }
}

/* CreateChn: the rate and GOP channel_i264e_encoder_init set up (num/den
 * reduced, GOP a multiple of num); the GOP is asked for again */
static inline void t32_gop33_create(T32Gop33 *s, uint32_t num, uint32_t den, uint32_t gop)
{
    s->num = s->user_num = num;
    s->den = s->user_den = den;
    s->gop = s->gop_req = gop;
    s->src_num = s->src_den = 0;
    s->mark = s->pics = 0;
}

/* IMP_Encoder_SetChnFrmRate / IMP_Encoder_SetGOPSize (arguments checked by the caller) */
static inline void t32_gop33_set_rate(T32Gop33 *s, uint32_t num, uint32_t den)
{
    t32_gop33_reduce(&num, &den);
    s->user_num = num;
    s->user_den = den;
}

static inline void t32_gop33_set_gop(T32Gop33 *s, uint32_t gop)
{
    s->gop_req = gop;
    s->mark = s->pics;
}

#define T32_GOP33_RATE 1u       /* a new rate was sent */
#define T32_GOP33_GOP  2u       /* a GOP was sent */
#define T32_GOP33_IDR  4u       /* the picture is an IDR */
#define T32_GOP33_ZERO 8u       /* the vendor would send GOP 0 here (under one second) */

/* before a picture from a source running at src_num/src_den: what is sent */
static inline unsigned t32_gop33_step(T32Gop33 *s, uint32_t src_num, uint32_t src_den)
{
    unsigned r = 0;
    int src_new = 0, recent = 0, rate = 0;
    uint32_t secs = 0;

    if (!src_num || !src_den)
        return 0;
    if (!s->src_num || !s->src_den ||
        (uint64_t)s->src_num * src_den != (uint64_t)s->src_den * src_num) {
        s->src_num = src_num;
        s->src_den = src_den;
        t32_gop33_reduce(&s->src_num, &s->src_den);
        src_new = 1;
        r |= T32_GOP33_IDR;
    }
    if (src_new || (uint64_t)s->user_den * s->num != (uint64_t)s->user_num * s->den) {
        uint32_t nn, nd;

        secs = s->num ? s->den * s->gop / s->num : 0u;
        if ((uint64_t)s->user_num * s->src_den <= (uint64_t)s->src_num * s->user_den) {
            nn = s->user_num;
            nd = s->user_den;
            rate = 1;
        } else if ((uint64_t)s->num * s->src_den != (uint64_t)s->src_num * s->den) {
            nn = s->src_num;
            nd = s->src_den;
            rate = 1;
        }
        if (rate) {
            uint64_t d = s->pics - s->mark;

            recent = d <= 2u && s->mark != 0u;
            s->num = nn;
            s->den = nd;
            t32_gop33_reduce(&s->num, &s->den);
            r |= T32_GOP33_RATE | T32_GOP33_IDR;
        }
    }
    if (s->gop_req || (rate && recent)) {
        uint32_t g;

        if (s->gop_req) {
            uint32_t k = s->num ? s->den * s->gop_req / s->num + ((s->den * s->gop_req) % s->num != 0u) : 0u;

            g = s->den ? k * s->num / s->den : 0u;
            s->gop_req = 0;
        } else {
            g = s->den ? secs * s->num / s->den : 0u;
        }
        /* GOP 0: i264e_ratecontrol_start divides by it (the vendor traps);
         * OpenIMP keeps the GOP in pictures */
        if (g)
            s->gop = g;
        else
            r |= T32_GOP33_ZERO;
        r |= T32_GOP33_GOP | T32_GOP33_IDR;
    }
    s->pics++;
    return r;
}

#endif
