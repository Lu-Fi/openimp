/*
 * T32/T33 H.264 picture type with intra refresh (GDR, #784): the vendor
 * libimp 2.0.2.1 i264e_decide_slice_type_and_rd (0x7062c) for a picture that
 * is not a forced IDR.  @pos is the picture counter of the last picture (no
 * wrap while GDR runs), @d the pictures since the last RequestGDR (-1 none),
 * @gop the IDR interval (0: none).  With the picture's request (F+0x152) the
 * picture is a P and @d restarts; with GDR enabled (F+0x153) the counter runs
 * on without periodic IDRs; else the GOP counter wraps (IDR at 0), or, after
 * a request, the IDR comes @gop pictures after it.  Returns 1 for an IDR (the
 * caller resets @pos to 0 and @d to -1 after it), 0 for a P (@pos, @d of
 * this picture).
 */
#ifndef OPENIMP_T32_GDR_H
#define OPENIMP_T32_GDR_H

#include <stdint.h>

static inline int t32_gdr_slice_type(uint32_t *pos, int32_t *d, uint32_t gop, int req, int en)
{
    if (req) {
        *pos += 1u;
        *d = 0;
        return 0;
    }
    if (en) {
        *pos += 1u;
        if (gop && (int64_t)gop < (int64_t)*d)
            *d = -1;
        else if (*d >= 0)
            *d += 1;
        return 0;
    }
    if (!gop) {                 /* no IDR interval */
        *pos += 1u;
        return 0;
    }
    if (*d == -1) {
        *pos = (*pos + 1u) % gop;
        return *pos == 0;
    }
    if ((int64_t)*d < (int64_t)gop) {
        *pos += 1u;
        *d += 1;
        return 0;
    }
    return 1;
}

#endif
