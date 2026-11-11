/* T23 IMP_Encoder_Setframelossthd (src/t23/t23_frameloss.h): the IDR QP
 * against the vendor get_idr_frameqp vectors (tools/frameloss_oracle.py),
 * the drop rule of update_h264_one_frmstrm and the clamp of
 * i264e_update_fenc. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t23/t23_frameloss.h"

static void vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned int n = 0;

    assert(f);
    while (fgets(line, sizeof(line), f)) {
        int mode, qp, mn, mx, delta, bytes, thr, want, got;

        if (line[0] != 'F')
            continue;
        assert(sscanf(line, "F %d %d %d %d %d %d %d : %d", &mode, &qp, &mn,
                      &mx, &delta, &bytes, &thr, &want) == 8);
        /* the oracle puts maxQp into the FIXQP word for modes 1..3 */
        got = t23_frameloss_idr_qp(mode, qp, mn, mx, delta + 26, bytes, thr);
        if (got != want) {
            fprintf(stderr, "mismatch: %s got %d\n", line, got);
            assert(0);
        }
        n++;
    }
    fclose(f);
    assert(n > 6000u);
    printf("frameloss: %u vendor get_idr_frameqp vectors equal\n", n);
}

static void rule(void)
{
    T23FrameLoss s;
    int qp = -7;

    memset(&s, 0, sizeof(s));
    /* off: everything is delivered */
    assert(!t23_frameloss_picture(&s, 1 << 20, 0, 1, 30, 15, 45, 30, &qp));
    assert(qp == -7 && s.drops == 0u);

    s.threshold = 10000;
    assert(!t23_frameloss_picture(&s, 9999, 1, 1, 30, 15, 45, 30, &qp));
    /* at the threshold: dropped, IDR at trunc(ln(2)) + 30 + 5 = 35 */
    assert(t23_frameloss_picture(&s, 20000, 0, 1, 30, 15, 45, 30, &qp));
    assert(qp == 35 && s.lost == 1 && s.drops == 1u && s.max_bytes == 20000);
    /* small P pictures until the IDR: dropped, IDR at minQp */
    assert(t23_frameloss_picture(&s, 500, 0, 1, 30, 15, 45, 35, &qp));
    assert(qp == 15 && s.lost == 1 && s.drops == 2u && s.max_bytes == 20000);
    /* the IDR ends the state */
    assert(!t23_frameloss_picture(&s, 8000, 1, 1, 30, 15, 45, 35, &qp));
    assert(s.lost == 0);
    assert(!t23_frameloss_picture(&s, 8000, 0, 1, 30, 15, 45, 35, &qp));
    /* an oversized IDR is dropped as well */
    assert(t23_frameloss_picture(&s, 10000, 1, 0, 28, 15, 45, 28, &qp));
    assert(qp == 28 && s.lost == 1 && s.drops == 3u);
    assert(!t23_frameloss_picture(&s, 100, 1, 0, 28, 15, 45, 28, &qp));

    /* i264e_update_fenc: 0 and negative = none, at most 51 */
    assert(t23_frameloss_fenc_qp(0) == 0);
    assert(t23_frameloss_fenc_qp(-1) == 0);
    assert(t23_frameloss_fenc_qp(35) == 35);
    assert(t23_frameloss_fenc_qp(60) == 51);
    assert(t23_frameloss_fenc_qp(200) == 0);    /* (char)200 < 0 */
    printf("frameloss: drop rule and IDR QP clamp\n");
}

int main(int argc, char **argv)
{
    rule();
    vectors(argc > 1 ? argv[1] : "frameloss_vectors.txt");
    return 0;
}
