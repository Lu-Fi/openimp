/*
 * T23 IMP_Encoder_Setframelossthd: the frame-loss rule of the OEM libimp
 * 1.3.0 stream thread, as pure C (host test: tests/t23/frameloss_test.c).
 *
 * OEM (update_h264_one_frmstrm 0x4d648, get_idr_frameqp 0x4d508; channel
 * record +0xa18 threshold, +0xa1c drop count, +0xa20 largest dropped
 * picture, +0xa24 "lost" state, +0xa28 the QP for the next IDR): with a
 * non-zero threshold every coded H.264 picture whose access unit (all NAL
 * units) has threshold bytes or more is not handed to the application;
 * the channel requests an IDR and the QP of that IDR.  While the channel is
 * in the "lost" state every following picture that is not an IDR is
 * dropped as well (it would reference the dropped one) and requests the IDR
 * again; the first IDR ends the state and is delivered.  The OEM channel
 * thread hands the QP to the encoder with the IDR request (picture +0x1a4,
 * i264e_update_fenc clamps it to 51, 0 or a negative value = none) and the
 * picture rate control codes the IDR at that QP (eprc E+1828).
 *
 * The IDR QP (get_idr_frameqp): FIXQP: the fixed QP; CBR/VBR/SMART:
 * trunc(ln(bytes / threshold)) + (slice QP of the picture - pic_init_qp)
 * + 26 + 5, at least minQp, then at most maxQp; other modes: none.  The
 * division is an integer division; for a picture below the threshold (the
 * "lost" state) the logarithm of 0 is -inf, which the MIPS trunc.w.d turns
 * into 0x7fffffff, and the sum wraps (32-bit addu), so such a picture gets
 * minQp.  OpenIMP's PPS has pic_init_qp 26, so the term is the slice QP.
 */
#ifndef OPENIMP_T23_FRAMELOSS_H
#define OPENIMP_T23_FRAMELOSS_H

#include <math.h>
#include <stdint.h>

typedef struct {
    int32_t threshold;      /* bytes, 0 = off (OEM: int compare) */
    int lost;               /* dropping until the next IDR */
    uint32_t drops;         /* pictures not delivered */
    int32_t max_bytes;      /* the largest dropped picture */
} T23FrameLoss;

/* OEM get_idr_frameqp.  rc_mode 0 FIXQP, 1 CBR, 2 VBR, 3 SMART. */
static inline int t23_frameloss_idr_qp(int rc_mode, int qp, int min_qp,
                                       int max_qp, int slice_qp,
                                       int32_t bytes, int32_t threshold)
{
    int32_t lo, hi, l, v;
    int32_t ratio;

    if (rc_mode == 0) {
        lo = hi = qp;
    } else if (rc_mode >= 1 && rc_mode <= 3) {
        lo = min_qp;
        hi = max_qp;
    } else {
        return -1;
    }
    if (threshold == 0)
        return -1;          /* the OEM traps (never reached: threshold set) */
    ratio = bytes / threshold;
    if (ratio <= 0) {
        l = INT32_MAX;      /* trunc.w.d(log(0) = -inf): invalid -> 2^31-1 */
    } else {
        double d = log((double)ratio);

        l = (int32_t)d;
    }
    v = (int32_t)((uint32_t)l + (uint32_t)slice_qp + 5u);
    if (v < lo)
        v = lo;
    return hi < v ? hi : v;
}

/* The forced QP the encoder gets for the requested IDR (i264e_update_fenc):
 * 0 = none, else 1..51. */
static inline int t23_frameloss_fenc_qp(int idr_qp)
{
    int8_t c = (int8_t)idr_qp;

    if (idr_qp == 0 || c <= 0)
        return 0;
    return c > 51 ? 51 : c;
}

/*
 * One coded picture.  Returns 1 when the picture is dropped; *idr_qp is
 * then the QP for the IDR to request (t23_frameloss_fenc_qp of it is what
 * the encoder gets).  0: deliver.
 */
static inline int t23_frameloss_picture(T23FrameLoss *s, int32_t bytes,
                                        int idr, int rc_mode, int qp,
                                        int min_qp, int max_qp, int slice_qp,
                                        int *idr_qp)
{
    if (!s->threshold)
        return 0;
    if (s->threshold <= bytes || (s->lost == 1 && !idr)) {
        s->lost = 1;
        *idr_qp = t23_frameloss_idr_qp(rc_mode, qp, min_qp, max_qp, slice_qp,
                                       bytes, s->threshold);
        s->drops++;
        if (bytes > s->max_bytes)
            s->max_bytes = bytes;
        return 1;
    }
    if (s->lost == 1)
        s->lost = 0;
    return 0;
}

#endif
