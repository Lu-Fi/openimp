/* #784 host test: H.265 SetMotionQuality P lists like vendor 2.0.2.1 (QEMU 0731 v5mq1 vs v5mq0):
 * 0x20000 bit 14, 0x30000 bit 27 clear, 0x30040 0x073a5049, 0x301c4..0x301e8 written, 0x90000 bit 7; I lists and
 * motion quality off unchanged. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_hera_h265.h"
#include "t32/openimp_t33_hera_h265.h"

static uint32_t list[T32_HERA5_LIST_MAX_PAIRS * 2];

static uint32_t val(size_t n, uint32_t reg)
{
    for (size_t i = 0; i < n; i++)
        if ((list[2 * i + 1] & 0xffffcu) == reg)
            return list[2 * i];
    return 0xdeadbeefu;
}

static size_t mk(int p_frame, int mq)
{
    T33HeraH265Pic t;

    memset(&t, 0, sizeof(t));
    t.pic.width = 1920;
    t.pic.height = 1088;
    t.pic.qp = 28;
    t.pic.src_stride = 1920;
    t.pic.p_frame = p_frame;
    t.pic.motion_quality = mq;
    return t33_hera_h265_list(&t, list);
}

int main(void)
{
    static const uint32_t r[14] = { 0x20000, 0x30000, 0x30040, 0x301c4, 0x301c8, 0x301cc, 0x301d0, 0x301d4,
                                    0x301d8, 0x301dc, 0x301e0, 0x301e4, 0x301e8, 0x90000 };
    static const uint32_t want[10] = { 0x00920000u, 0x01b60124u, 0x00201008u, 0x00000208u, 0x00202010u,
                                       0x00000208u, 0x00404020u, 0x00000410u, 0x00808040u, 0x00000820u };
    uint32_t off[14], on[14], i0[14], i1[14];
    size_t n;

    n = mk(1, 0); assert(n); for (int i = 0; i < 14; i++) off[i] = val(n, r[i]);
    n = mk(1, 1); assert(n); for (int i = 0; i < 14; i++) on[i] = val(n, r[i]);
    n = mk(0, 0); assert(n); for (int i = 0; i < 14; i++) i0[i] = val(n, r[i]);
    n = mk(0, 1); assert(n); for (int i = 0; i < 14; i++) i1[i] = val(n, r[i]);
    assert(on[0] == (off[0] | 1u << 14) && (on[0] & 1u << 14));
    assert(on[1] == (off[1] & ~(1u << 27)) && (off[1] & 1u << 27));
    assert(off[2] == 0x070d5049u && on[2] == 0x073a5049u);
    for (int i = 0; i < 10; i++)
        assert(off[3 + i] == 0 && on[3 + i] == want[i]);
    assert(off[13] == 0x637u && on[13] == 0x6b7u);
    assert(!memcmp(i0, i1, sizeof(i0)));
    printf("mq784_test: OK (P 0x20000 0x%x -> 0x%x, 0x30000 0x%x -> 0x%x)\n", off[0], on[0], off[1], on[1]);
    return 0;
}
