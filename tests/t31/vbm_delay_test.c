/*
 * vbm_delay_test - the FrameSource delay FIFO helpers (src/vbm_delay.h)
 * behind IMP_FrameSource_SetMaxDelay/SetDelay/SetChnFifoAttr and
 * IMP_FrameSource_GetTimedFrame (T20/T21/T23/T30/T31).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vbm_delay.h"

static int failures;

#define CHECK(cond, ...) do {                                         \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

static void test_push(void)
{
    VBMDelayRing ring;
    int out[2], n, i;

    /* delay 3: three frames are held, then each new one releases the one
     * captured three frames before it */
    vbm_delay_reset(&ring, 4, 3);
    for (i = 0; i < 3; i++)
        CHECK(vbm_delay_push(&ring, i, out) == 0, "frame %d published "
              "while the FIFO fills", i);
    for (i = 3; i < 10; i++) {
        n = vbm_delay_push(&ring, i, out);
        CHECK(n == 1 && out[0] == i - 3, "frame %d: %d out, first %d", i, n,
              out[0]);
    }
    CHECK(ring.count == 3, "held %d", ring.count);
    /* SetDelay 1 while running: two leave at once (the older is dropped by
     * the caller), then one per frame again */
    ring.delay = 1;
    n = vbm_delay_push(&ring, 10, out);
    CHECK(n == 2 && out[0] == 7 && out[1] == 8, "lowered delay: %d out "
          "(%d, %d)", n, out[0], out[1]);
    n = vbm_delay_push(&ring, 11, out);
    CHECK(n == 2 && out[0] == 9 && out[1] == 10, "lowered delay, 2nd: %d "
          "(%d, %d)", n, out[0], out[1]);
    n = vbm_delay_push(&ring, 12, out);
    CHECK(n == 1 && out[0] == 11 && ring.count == 1, "steady at delay 1");
    /* raised again: holds until it has that many */
    ring.delay = 3;
    CHECK(vbm_delay_push(&ring, 13, out) == 0 &&
          vbm_delay_push(&ring, 14, out) == 0, "raised delay refills");
    n = vbm_delay_push(&ring, 15, out);
    CHECK(n == 1 && out[0] == 12, "after refill: %d (%d)", n, out[0]);
    /* delay 0 with a FIFO: every frame straight through */
    vbm_delay_reset(&ring, 4, 0);
    n = vbm_delay_push(&ring, 5, out);
    CHECK(n == 1 && out[0] == 5 && ring.count == 0, "delay 0");
    /* reset bounds */
    vbm_delay_reset(&ring, 100, 100);
    CHECK(ring.max == VBM_DELAY_MAX_FRAMES - 1 && ring.delay == ring.max,
          "max clamp %d/%d", ring.max, ring.delay);
    vbm_delay_reset(&ring, 2, 5);
    CHECK(ring.delay == 2, "delay above max");
}

static void test_pick(void)
{
    const int64_t ts[4] = { 1000, 1040, 1080, 1120 };

    CHECK(vbm_delay_pick(ts, 4, 1000) == 0, "exact oldest");
    CHECK(vbm_delay_pick(ts, 4, 999) == -1, "older than every frame");
    CHECK(vbm_delay_pick(ts, 4, 1121) == -2, "newer than every frame");
    CHECK(vbm_delay_pick(ts, 0, 5) == -2, "empty FIFO");
    CHECK(vbm_delay_pick(ts, 4, 1040) == 1, "exact middle");
    CHECK(vbm_delay_pick(ts, 4, 1050) == 1, "nearer the earlier");
    CHECK(vbm_delay_pick(ts, 4, 1070) == 2, "nearer the later");
    CHECK(vbm_delay_pick(ts, 4, 1060) == 2, "tie goes to the later");
    CHECK(vbm_delay_pick(ts, 4, 1120) == 3, "exact newest");
}

static void test_copy(void)
{
    /* 8x10 NV12 in a buffer whose chroma starts after 16 luma lines */
    static uint8_t src[8 * 16 + 8 * 8];
    uint8_t dst[8 * 10 * 3 / 2 + 4];
    uint32_t n, i;

    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)i;
    memset(dst, 0xee, sizeof(dst));
    n = vbm_delay_copy(dst, src, 8, 10, 0x3231564eu, sizeof(src));
    CHECK(n == 120, "NV12 size %u", n);
    CHECK(!memcmp(dst, src, 80), "luma");
    CHECK(!memcmp(dst + 80, src + 128, 40), "chroma from the aligned plane");
    CHECK(dst[120] == 0xee, "wrote past the packed frame");
    CHECK(vbm_delay_copy(NULL, src, 8, 10, 0x3132564eu, sizeof(src)) == 120,
          "NV21 size without a buffer");
    CHECK(vbm_delay_copy(dst, src, 8, 10, 0x3231564eu, 100) == 0,
          "short source accepted");
    memset(dst, 0, sizeof(dst));
    n = vbm_delay_copy(dst, src, 8, 10, 0x56595559u /* YUYV */, 64);
    CHECK(n == 64 && !memcmp(dst, src, 64), "other format copied as is");
}

int main(void)
{
    test_push();
    test_pick();
    test_copy();
    if (failures) {
        fprintf(stderr, "vbm delay: %d check(s) failed\n", failures);
        return 1;
    }
    printf("VBM delay FIFO tests passed\n");
    return 0;
}
