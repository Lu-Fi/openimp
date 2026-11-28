/* #962-#968: encoder output rate below the sensor rate, vendor keep mask
 * (src/t40/p2_fps_mask.h) against a transcription of the vendor code
 * (T30 libimp encoder_get_fps_mask 0xb6524: build loop and the 64-bit
 * rotate on two 32-bit words, as the decompile does it). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/t40/p2_fps_mask.h"

/* vendor channel words: +0xc0/+0xc4 mask, len = eff_den * src_num */
typedef struct {
    uint32_t lo, hi;
    uint32_t len;
} VendorMask;

static void vendor_build(VendorMask *v, uint32_t len, uint32_t keep)
{
    uint32_t lo = 0, hi = 0, i = 0, next = 0;
    int k = 0;

    for (i = 0; i < len; i++) {
        hi = (lo >> 31) | (hi << 1);
        lo <<= 1;
        if (i == next) {
            uint32_t n = lo + 1u;

            k++;
            hi += n < lo;
            lo = n;
            next = (uint32_t)((int)(((len * 1000u) / keep) * (uint32_t)k + 500u) / 1000);
        }
    }
    v->lo = lo;
    v->hi = hi;
    v->len = len;
}

/* the rotate of encoder_get_fps_mask (LAB_000b6758), returns bit 0 */
static int vendor_step(VendorMask *v)
{
    uint64_t m = ((uint64_t)v->hi << 32) | v->lo;
    uint32_t n = v->len, n1 = n - 1u;
    uint64_t top = n1 >= 64u ? 0 : m >> n1;
    uint64_t clr = n >= 64u ? 0 : 1ull << n;

    m = ((m << 1) | top) & ~clr;
    v->lo = (uint32_t)m;
    v->hi = (uint32_t)(m >> 32);
    return (int)(v->lo & 1u);
}

static void expect(uint32_t src, uint32_t out, const char *pattern)
{
    P2FpsMask m;
    size_t i, n = strlen(pattern);
    char got[130];

    memset(&m, 0, sizeof(m));
    for (i = 0; i < 2u * n; i++) {
        assert(p2_fps_mask_update(&m, src, 1, out, 1) == 1);
        got[i] = p2_fps_mask_take(&m) ? 'x' : '.';
    }
    got[2u * n] = 0;
    printf("%2u -> %2u: %s\n", src, out, got);
    assert(!strncmp(got, pattern, n));
    assert(!strncmp(got + n, pattern, n));      /* the window repeats */
}

static void against_vendor(uint32_t sn, uint32_t sd, uint32_t on, uint32_t od)
{
    P2FpsMask m;
    VendorMask v;
    uint32_t a = sn, b = sd, c = on, d = od, len, keep;
    int i, kept = 0, rc;

    memset(&m, 0, sizeof(m));
    rc = p2_fps_mask_update(&m, sn, sd, on, od);
    p2_fps_reduce(&a, &b);
    p2_fps_reduce(&c, &d);
    if ((uint64_t)a * d <= (uint64_t)c * b) {
        assert(rc == 0);
        for (i = 0; i < 200; i++)
            assert(p2_fps_mask_take(&m) == 1);
        return;
    }
    len = d * a;
    keep = c * b;
    if (len > 64u) {
        assert(rc == -1);
        return;
    }
    assert(rc == 1);
    vendor_build(&v, len, keep);
    for (i = 0; i < 4 * (int)len; i++) {
        int x = p2_fps_mask_take(&m);

        assert(x == vendor_step(&v));
        assert(p2_fps_mask_update(&m, sn, sd, on, od) == 1);
        kept += x;
    }
    assert(kept == 4 * (int)keep);
}

int main(void)
{
    uint32_t s, o, sd, od;
    P2FpsMask m;

    /* exact vendor keep patterns (positions ((len*1000/keep)*k+500)/1000) */
    expect(25, 15, "x.xx.x.xx.x.xx.x.xx.x.xx.");
    expect(25, 10, "x..x.x..x.x..x.x..x.x..x.");
    expect(15, 5, "x..x..x..x..x..");
    expect(30, 25, "xxx.xxxxx.xxxxx.xxxxx.xxxxx.xx");
    expect(25, 1, "x........................");

    /* every integer and a few fractional rate pairs equal the vendor code */
    for (s = 1; s <= 60; s++)
        for (o = 1; o <= 60; o++)
            against_vendor(s, 1, o, 1);
    for (sd = 1; sd <= 3; sd++)
        for (od = 1; od <= 4; od++)
            for (s = 1; s <= 30; s++)
                for (o = 1; o <= 30; o++)
                    against_vendor(s, sd, o, od);
    against_vendor(25, 1, 15, 2);       /* 7.5 fps: len 50 */
    against_vendor(30000, 1001, 15, 1); /* window > 64: vendor error */

    /* rate change: rebuilt, the first frame after it is kept; same rate in
     * another fraction (50/2) is no change */
    memset(&m, 0, sizeof(m));
    assert(p2_fps_mask_update(&m, 25, 1, 10, 1) == 1);
    assert(p2_fps_mask_take(&m) == 1);
    assert(p2_fps_mask_update(&m, 50, 2, 10, 1) == 1);
    assert(p2_fps_mask_take(&m) == 0);
    assert(p2_fps_mask_update(&m, 15, 1, 10, 1) == 1);
    assert(p2_fps_mask_take(&m) == 1);
    assert(p2_fps_mask_update(&m, 15, 1, 15, 1) == 0);
    assert(p2_fps_mask_take(&m) == 1);
    assert(p2_fps_mask_update(&m, 0, 1, 15, 1) == -1);
    assert(p2_fps_mask_take(&m) == 1);
    printf("p2 fps mask: vendor keep patterns OK\n");
    return 0;
}
