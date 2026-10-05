/* T23 IMP_Encoder_SetChnMaxPictureSize: the OEM decision (libimp 1.3.0
 * update_h264_one_frmstrm + get_idr_frameqp, src/t23/openimp_t23_maxpic.h).
 * Expected values are get_idr_frameqp worked out by hand from the
 * disassembly:
 *   qp = (int)log(size / limit) + qp_of_dropped + 5, clamped to
 *   [min_qp, max_qp]; a picture of the limit or more is dropped. */
#include <assert.h>
#include <stdio.h>

#include "t23/openimp_t23_maxpic.h"

int main(void)
{
    int qp = 0;

    /* ratio 1: log 0 */
    assert(t23_maxpic_idr_qp(0, 10000, 10000, 30, 10, 45) == 35);
    assert(t23_maxpic_idr_qp(0, 19999, 10000, 30, 10, 45) == 35);
    /* ratio 2: log 0.69 -> 0 ; ratio 3: log 1.10 -> 1 */
    assert(t23_maxpic_idr_qp(0, 29999, 10000, 30, 10, 45) == 35);
    assert(t23_maxpic_idr_qp(0, 30000, 10000, 30, 10, 45) == 36);
    /* ratio 7: 1.95 -> 1 ; 8: 2.08 -> 2 ; 20: 3.00 -> 2 (2.9957) ; 21 -> 3 */
    assert(t23_maxpic_idr_qp(0, 70000, 10000, 30, 10, 50) == 36);
    assert(t23_maxpic_idr_qp(0, 80000, 10000, 30, 10, 50) == 37);
    assert(t23_maxpic_idr_qp(0, 200000, 10000, 30, 10, 50) == 37);
    assert(t23_maxpic_idr_qp(0, 210000, 10000, 30, 10, 50) == 38);
    /* a 5 MB picture over a 1 kB limit: ratio 5000, log 8.5 -> 8 */
    assert(t23_maxpic_idr_qp(0, 5000000, 1000, 20, 10, 51) == 33);
    /* the QP range clamps both ways */
    assert(t23_maxpic_idr_qp(0, 30000, 10000, 48, 10, 51) == 51);
    assert(t23_maxpic_idr_qp(0, 30000, 10000, 48, 10, 50) == 50);
    assert(t23_maxpic_idr_qp(0, 10000, 10000, 5, 20, 45) == 20);
    /* FIXQP: the fixed QP */
    assert(t23_maxpic_idr_qp(1, 30000, 10000, 27, 0, 30) == 30);

    /* drop: size >= limit; 0 = no limit */
    assert(!t23_maxpic_drop(100000, 0, 0, 30, 10, 45, &qp));
    assert(!t23_maxpic_drop(9999, 10000, 0, 30, 10, 45, &qp));
    assert(t23_maxpic_drop(10000, 10000, 0, 30, 10, 45, &qp) && qp == 35);
    assert(t23_maxpic_drop(30000, 10000, 0, 30, 10, 45, &qp) && qp == 36);
    /* no progress possible (QP range used up): delivered, not dropped */
    assert(!t23_maxpic_drop(30000, 10000, 0, 45, 10, 45, &qp));
    assert(t23_maxpic_drop(30000, 10000, 0, 44, 10, 45, &qp) && qp == 45);
    assert(!t23_maxpic_drop(30000, 10000, 0, 45, 10, 45, &qp));
    /* FIXQP: an IDR is coded 3 below the fixed QP, so one re-code at the
     * fixed QP; at the fixed QP the picture is delivered */
    assert(t23_maxpic_drop(30000, 10000, 1, 27, 0, 30, &qp) && qp == 30);
    assert(!t23_maxpic_drop(30000, 10000, 1, 30, 0, 30, &qp));
    puts("maximum picture size decision ok");
    return 0;
}
