/* #904 host test: the stand-alone (Main, no8x8) H.264 P list clears 0x30000 bit 14 like the vendor 2.0.2.1
 * YuvEncode lists (0x04042081); the bound-channel P list keeps it (vendor 0x04046081). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t33_hera.h"

static uint32_t reg_val(const uint32_t *l, size_t n, uint32_t reg)
{
    for (size_t i = 0; i < n; i++)
        if ((l[2 * i + 1] & 0xffffcu) == reg)
            return l[2 * i];
    return 0xdeadbeefu;
}

static uint32_t p_word(int no8x8)
{
    static uint32_t list[T32_HERA_LIST_MAX_PAIRS * 2];
    T33HeraH264Pic t;
    size_t n;

    memset(&t, 0, sizeof(t));
    t.pic.width = 640;
    t.pic.height = 368;
    t.pic.qp = 30;
    t.pic.src_stride = 640;
    t.pic.p_frame = 1;
    t.no8x8 = no8x8;
    n = t33_hera_h264_list(&t, list);
    assert(n);
    return reg_val(list, n, 0x30000u);
}

int main(void)
{
    uint32_t bound = p_word(0), yuv = p_word(1);

    assert(bound != 0xdeadbeefu && yuv != 0xdeadbeefu);
    assert(bound & (1u << 14));
    assert(!(yuv & (1u << 14)) && (yuv | (1u << 14)) == bound);
    printf("yuvp904_test: OK (bound 0x%08x, stand-alone 0x%08x)\n", bound, yuv);
    return 0;
}
