/* #899 host test: the H.264 slice header turns deblocking off below QP 16 like the vendor
 * i264e_slice_init (2.0.2.1 0x78ab4): disable_deblocking_filter_idc 1 and no offsets, else 0, 0, 0.
 * Reads the header back bit by bit (Exp-Golomb) after the slice_qp_delta. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_h264.h"

static const uint8_t *P; static unsigned bit, len;
static unsigned rb(void) { unsigned v = (P[bit / 8] >> (7 - bit % 8)) & 1u; bit++; return v; }
static unsigned rbits(unsigned n) { unsigned v = 0; while (n--) v = v << 1 | rb(); return v; }
static unsigned ue(void) { unsigned z = 0; while (!rb()) z++; return (1u << z) - 1u + rbits(z); }

static unsigned idc_of(int qp, int idr)
{
    T32H264Seq s;
    T32H264Slice sl;
    uint8_t out[64];
    int esc = 0;
    size_t n;

    memset(&s, 0, sizeof(s));
    memset(&sl, 0, sizeof(sl));
    s.init_qp = 26;
    sl.qp = qp;
    sl.idr = idr;
    n = t32_h264_slice_head(&s, &sl, out, &esc);
    assert(n > 5);
    /* skip the start code and the NAL header byte */
    P = out; len = (unsigned)n; bit = 0;
    while (!(P[bit / 8] == 1 && bit / 8 >= 2 && P[bit / 8 - 1] == 0)) bit += 8;
    bit += 16;
    ue(); ue(); ue();                                   /* first_mb, slice_type, pps id */
    rbits(T32_H264_LOG2_MAX_FRAME_NUM);
    if (idr) { ue(); rbits(2); } else { rbits(3); ue(); }
    ue();                                               /* slice_qp_delta (se) */
    return ue();
}

int main(void)
{
    assert(idc_of(15, 0) == 1 && idc_of(15, 1) == 1 && idc_of(0, 0) == 1);
    assert(idc_of(16, 0) == 0 && idc_of(30, 1) == 0 && idc_of(51, 0) == 0);
    printf("h264dbk_test: OK\n");
    return 0;
}
