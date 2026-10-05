#ifndef OPENIMP_T23_MAXPIC_H
#define OPENIMP_T23_MAXPIC_H

/* T23 1.3.0 IMP_Encoder_SetChnMaxPictureSize / Setframelossthd: the OEM
 * channel thread (update_h264_one_frmstrm) compares the byte size of every
 * encoded access unit with the channel threshold.  A picture of that size
 * or more is NOT delivered: its stream is released, the next picture is
 * forced to an IDR and that IDR is coded at the QP get_idr_frameqp()
 * returns.  Pure decision code, shared by the encoder and the host test
 * (tests/t23/maxpic_test.c). */

#include <math.h>
#include <stdint.h>

/* OEM get_idr_frameqp for the rate-control modes 1..3 (CBR, VBR, SMART):
 *   qp = (int)log(frame_bytes / max_bytes) + (cur_qp + 5)
 * with an integer division inside the log, bounded to [min_qp, max_qp]:
 * min(max_qp, max(qp, min_qp)).  FIXQP (mode 0): the fixed QP itself.
 * cur_qp is the QP the dropped picture was coded at (the OEM reads
 * slice_qp_delta of the stream info and adds the H.264 base of 26). */
static inline int t23_maxpic_idr_qp(int fixqp, uint32_t frame_bytes,
                                    uint32_t max_bytes, int cur_qp,
                                    int min_qp, int max_qp)
{
    uint32_t ratio;
    int qp;

    if (fixqp)
        return max_qp;              /* the OEM: fixed QP in both bounds */
    ratio = max_bytes ? frame_bytes / max_bytes : 0u;
    /* log(0) is -inf in the OEM (never reached: only oversized pictures
     * get here, ratio >= 1) */
    qp = (int)log((double)ratio) + cur_qp + 5;
    if (qp < min_qp)
        qp = min_qp;
    if (qp > max_qp)
        qp = max_qp;
    return qp;
}

/* 1: drop the picture and re-code an IDR at *new_qp.  The OEM drops every
 * picture of max_bytes or more (0 = no limit).  Not in the OEM: when the new
 * QP is no higher than the one the picture was coded at, a re-code cannot
 * get smaller (the QP range is exhausted); the picture is delivered instead
 * of dropping every following IDR for ever. */
static inline int t23_maxpic_drop(uint32_t frame_bytes, uint32_t max_bytes,
                                  int fixqp, int cur_qp, int min_qp,
                                  int max_qp, int *new_qp)
{
    int qp;

    if (!max_bytes || frame_bytes < max_bytes)
        return 0;
    qp = t23_maxpic_idr_qp(fixqp, frame_bytes, max_bytes, cur_qp, min_qp,
                           max_qp);
    if (qp <= cur_qp)
        return 0;
    *new_qp = qp;
    return 1;
}

#endif
