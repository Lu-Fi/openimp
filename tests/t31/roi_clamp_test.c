/* ROI QP validity (src/avpu_roi.h, Helix_H264_RoiSanitize): whatever the
 * windows ask for, the macroblock QPs the table produces never need an
 * mb_qp_delta outside -26..+25 between consecutive macroblocks, stay inside
 * 0..51 and the RC min/max QP. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avpu_roi.h"
#include "t30/helix_roi.h"

static int failures;

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

#define COLS 20u
#define ROWS 12u

/* walk the table like the encoder: mb_qp_delta against the previous MB,
 * the first one against the slice QP */
static int walk(const uint8_t *t, uint32_t stride, unsigned int bits,
                int base, int minq, int maxq, int *worst)
{
    int prev = base, bad = 0;
    uint32_t i;

    *worst = 0;
    (void)worst;
    for (i = 0; i < COLS * ROWS; i++) {
        int q = base + avpu_roi_get(t, stride, i, bits);
        int d = q - prev;

        if (q < 0 || q > 51 || (maxq && q > maxq && base <= maxq) ||
            (q < minq && base >= minq))
            bad |= 2;
        if (d < -26 || d > 25)
            bad |= 1;
        prev = q;
    }
    /* the outgoing step of the last window is covered by the loop */
    return bad;
}

static AvpuRoiWin win(int en, int abs, int qp, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h)
{
    AvpuRoiWin r = { (uint8_t)en, (uint8_t)abs, (int8_t)qp, x, y, w, h };
    return r;
}

static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

int main(void)
{
    uint8_t t[COLS * ROWS * 4];
    AvpuRoiWin w[10];
    AvpuRoiResult r;
    int worst, lo, hi;
    uint32_t seed = 12345, it, n;

    /* relative -26 (valid on the API) becomes -25: the step back to 0 is +25 */
    memset(w, 0, sizeof(w));
    w[0] = win(1, 0, -26, 160, 64, 64, 64);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 30, 0, 51, 0, &r);
    CHECK(avpu_roi_get(t, 1, 4 * COLS + 10, 6) == -25, "-26 -> -25");
    CHECK(r.clamped == AVPU_ROI_CLAMP_DELTA, "flag %x", r.clamped);
    CHECK(!walk(t, 1, 6, 30, 0, 51, &worst), "walk -26");

    /* -25..25 pass unchanged */
    w[0] = win(1, 0, 25, 160, 64, 64, 64);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 20, 0, 51, 0, &r);
    CHECK(avpu_roi_get(t, 1, 4 * COLS + 10, 6) == 25 && r.clamped == 0,
          "+25 unchanged");

    /* absolute 15 at picture QP 42: distance 27 -> -25 */
    w[0] = win(1, 1, 15, 160, 64, 64, 64);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 42, 0, 51, 0, &r);
    CHECK(avpu_roi_get(t, 1, 4 * COLS + 10, 6) == -25, "abs 15 @42");
    CHECK(!walk(t, 1, 6, 42, 0, 51, &worst), "walk abs");

    /* positive delta capped by max_qp 45 at QP 40 */
    w[0] = win(1, 0, 20, 0, 0, 640, 192);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 40, 10, 45, 0, &r);
    CHECK(avpu_roi_get(t, 1, 0, 6) == 5 && (r.clamped & AVPU_ROI_CLAMP_RANGE),
          "max_qp: %d", avpu_roi_get(t, 1, 0, 6));
    /* negative below min_qp 18 at QP 30 */
    w[0] = win(1, 0, -20, 0, 0, 640, 192);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 30, 18, 45, 0, &r);
    CHECK(avpu_roi_get(t, 1, 0, 6) == -12, "min_qp");
    /* slice QP + delta stays in 0..51 with no usable RC range */
    w[0] = win(1, 0, 25, 0, 0, 640, 192);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 48, 0, 0, 0, &r);
    CHECK(avpu_roi_get(t, 1, 0, 6) == 3, "51 limit");
    w[0] = win(1, 0, -25, 0, 0, 640, 192);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 8, 0, 51, 0, &r);
    CHECK(avpu_roi_get(t, 1, 0, 6) == -8, "0 limit");

    /* two windows +25 and -25: spread 50, the high one is lowered to 0 */
    memset(w, 0, sizeof(w));
    w[0] = win(1, 0, 25, 0, 0, 128, 64);
    w[1] = win(1, 0, -25, 192, 64, 128, 64);
    avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, 30, 0, 51, 0, &r);
    CHECK(avpu_roi_get(t, 1, 0, 6) == 0 && avpu_roi_get(t, 1, 4 * COLS + 12, 6) == -25 &&
          (r.clamped & AVPU_ROI_CLAMP_SPREAD), "spread");
    CHECK(!walk(t, 1, 6, 30, 0, 51, &worst), "walk spread");

    /* T41 layout: 4-byte stride, 8-bit entries, nearest-block rounding */
    memset(t, 0x20, sizeof(t));
    w[0] = win(1, 0, -26, 16, 16, 40, 40);     /* 40 -> 3 blocks */
    w[1] = win(0, 0, 0, 0, 0, 0, 0);
    avpu_roi_fill(t, 4, 8, COLS, ROWS, w, 10, 0, 0, 0, 1, &r);
    CHECK((int8_t)t[4 * (COLS + 1)] == -25 && (int8_t)t[4 * (COLS + 3)] == -25 &&
          (int8_t)t[4 * (COLS + 4)] == 0 && (int8_t)t[4 * (3 * COLS + 1)] == -25 &&
          (int8_t)t[4 * (4 * COLS + 1)] == 0,
          "t41 window");
    CHECK(t[3] == 0x20, "byte 3 of the T41 entry untouched");

    /* limits */
    avpu_roi_limits(0, 0, 0, &lo, &hi);
    CHECK(lo == -25 && hi == 25, "unknown base");
    avpu_roi_limits(60, 0, 51, &lo, &hi);   /* bogus QP: only the delta rule */
    CHECK(lo == -25 && hi == 25, "bogus base");

    /* fuzz: random windows, modes, QPs, ranges: always valid */
    for (it = 0; it < 20000; it++) {
        int base = 1 + (int)(rnd(&seed) % 51u);
        int minq = (int)(rnd(&seed) % 40u), maxq = minq + (int)(rnd(&seed) % 40u);

        if (maxq > 51)
            maxq = 51;
        memset(w, 0, sizeof(w));
        for (n = 0; n < 10u; n++) {
            uint32_t x = rnd(&seed) % 300u, y = rnd(&seed) % 180u;

            w[n] = win(rnd(&seed) % 3u != 0, rnd(&seed) % 2u,
                       (int)(rnd(&seed) % 64u) - 32, x, y,
                       1 + rnd(&seed) % 200u, 1 + rnd(&seed) % 100u);
            if (w[n].mode)
                w[n].qp = (int8_t)(rnd(&seed) % 52u);
            else if (w[n].qp < -26 || w[n].qp > 25)
                w[n].qp = (int8_t)((int)(rnd(&seed) % 52u) - 26);
        }
        avpu_roi_fill(t, 1, 6, COLS, ROWS, w, 10, (uint32_t)base,
                      (uint32_t)minq, (uint32_t)maxq, 0, &r);
        if (walk(t, 1, 6, base, minq, maxq, &worst)) {
            CHECK(0, "fuzz T31 it=%u base=%d min=%d max=%d", it, base, minq, maxq);
            break;
        }
        avpu_roi_fill(t, 4, 8, COLS, ROWS, w, 10, 0, 0, 0, 1, &r);
        {
            int prev = 0, i;
            for (i = 0; i < (int)(COLS * ROWS); i++) {
                int q = avpu_roi_get(t, 4, (uint32_t)i, 8);
                if (q - prev < -26 || q - prev > 25) {
                    CHECK(0, "fuzz T41 it=%u", it);
                    it = 20000;
                    break;
                }
                prev = q;
            }
        }
    }

    /* Helix: relative -26, absolute 15 at QP 42 (T10/T20 range 30..51),
     * two regions far apart */
    {
        uint8_t in[8][7], out[8][7];
        unsigned int f;

        memset(in, 0, sizeof(in));
        Helix_H264_RoiEntry(1, 1, -26, 640, 320, 960, 560, in[0]);
        f = Helix_H264_RoiSanitize((const uint8_t (*)[7])in, out, 30, 18, 43);
        CHECK((int8_t)out[0][2] == -25 && (f & AVPU_ROI_CLAMP_DELTA), "helix -26");
        CHECK(out[0][3] == in[0][3] && out[0][6] == in[0][6], "helix geometry");

        memset(in, 0, sizeof(in));
        Helix_H264_RoiEntry(1, 0, 15, 640, 320, 960, 560, in[0]);
        f = Helix_H264_RoiSanitize((const uint8_t (*)[7])in, out, 42, 30, 51);
        CHECK((int8_t)out[0][2] == 26 && (f & AVPU_ROI_CLAMP_RANGE),
              "helix abs 15 @42 -> %d", (int8_t)out[0][2]);
        /* an absolute QP inside the range is kept */
        Helix_H264_RoiEntry(1, 0, 40, 640, 320, 960, 560, in[0]);
        f = Helix_H264_RoiSanitize((const uint8_t (*)[7])in, out, 42, 30, 51);
        CHECK((int8_t)out[0][2] == 40 && f == 0, "helix abs in range");
        /* spread */
        memset(in, 0, sizeof(in));
        Helix_H264_RoiEntry(1, 1, 25, 0, 0, 160, 160, in[0]);
        Helix_H264_RoiEntry(1, 1, -25, 320, 0, 480, 160, in[1]);
        f = Helix_H264_RoiSanitize((const uint8_t (*)[7])in, out, 30, 18, 43);
        CHECK((int8_t)out[0][2] == 0 && (int8_t)out[1][2] == -25 &&
              (f & AVPU_ROI_CLAMP_SPREAD), "helix spread");
        /* disabled regions stay byte for byte */
        CHECK(memcmp(out[2], in[2], 7) == 0, "helix disabled");
        /* a huge s32Qp keeps its sign */
        Helix_H264_RoiEntry(1, 1, 300, 0, 0, 160, 160, in[3]);
        CHECK((int8_t)in[3][2] == 127, "helix entry clamp");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("roi_clamp_test: OK");
    return 0;
}
