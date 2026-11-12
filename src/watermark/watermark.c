/* See watermark.h: the vendor's luma watermark behind SetFisheyeEnableStatus. */
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "watermark.h"

#define WM_N        64          /* coefficients per 8x8 block */
#define WM_COEF_A   33          /* the two coefficients that carry the mark */
#define WM_COEF_B   12
#define WM_RANK     30          /* ranks 30, 31, 32 (largest first, from 0) are levelled */
#define WM_DC_MIN   80.0f       /* |DC| below: the block is replaced */
#define WM_FLOOR    0.5f        /* smallest levelled magnitude */
#define WM_STRENGTH 0.1f        /* weight of the mark */
#define WM_EPS      5e-6        /* "same magnitude" when searching the sorted ranks */

/* The vendor's 8x8 DCT-II basis: row k = frequency, column n = sample,
 * written with six decimals (so not exactly symmetric). */
static const float wm_dct[WM_N] = {
    0.353553f, 0.353553f, 0.353553f, 0.353553f, 0.353553f, 0.353553f, 0.353553f, 0.353553f,
    0.490393f, 0.415735f, 0.277785f, 0.097545f, -0.097545f, -0.277785f, -0.415735f, -0.490393f,
    0.46194f, 0.191341f, -0.191341f, -0.46194f, -0.46194f, -0.191342f, 0.191341f, 0.46194f,
    0.415735f, -0.097545f, -0.490393f, -0.277785f, 0.277785f, 0.490393f, 0.097545f, -0.415735f,
    0.353554f, -0.353554f, -0.353554f, 0.353554f, 0.353554f, -0.353554f, -0.353554f, 0.353554f,
    0.277785f, -0.490393f, 0.097545f, 0.415735f, -0.415735f, -0.097545f, 0.490393f, -0.277785f,
    0.191341f, -0.46194f, 0.46194f, -0.191341f, -0.191342f, 0.46194f, -0.46194f, 0.191341f,
    0.097545f, -0.277785f, 0.415735f, -0.490393f, 0.490393f, -0.415735f, 0.277785f, -0.097545f,
};

/* Coefficients that replace a dark block (raster order, as the vendor keeps them). */
static const float wm_dark_block[WM_N] = {
    74.99983f, 7.334908f, -6.836268f, -6.08955f,
    6.000004f, 5.526941f, -5.031826f, -4.7451f,
    -4.475156f, 4.379658f, -4.129858f, -4.037567f,
    -3.963046f, -3.781809f, -3.637949f, 3.447896f,
    3.29055f, 3.261708f, -3.098304f, 3.08027f,
    -2.846243f, 2.840293f, -2.772887f, -2.767764f,
    -2.636348f, 2.483991f, -2.473889f, 2.414223f,
    -2.307114f, 2.3016f, -2.0444f, -1.902556f,
    -1.736412f, -1.689251f, -1.566984f, 1.500001f,
    -1.490217f, 1.467793f, -1.465076f, -1.45776f,
    -1.344692f, 1.012724f, 1.000003f, -0.952022f,
    0.874869f, 0.857513f, 0.82141f, 0.767766f,
    0.672561f, -0.669244f, -0.668013f, -0.599824f,
    -0.544652f, 0.426042f, 0.41422f, 0.405658f,
    0.402439f, -0.318998f, -0.285764f, 0.234587f,
    -0.219605f, -0.21224f, -0.057854f, 0.000293f,
};

/* The mark: one pair per block (block = column / 8 + 8 * (row / 8)). */
static const float wm_mark[32] = {
    126.0f, 24.0f, 24.0f, 24.0f, 24.0f, 24.0f, 24.0f, 126.0f,
    62.0f, 127.0f, 102.0f, 102.0f, 102.0f, 102.0f, 102.0f, 248.0f,
    254.0f, 230.0f, 118.0f, 62.0f, 126.0f, 67.0f, 127.0f, 60.0f,
    24.0f, 126.0f, 102.0f, 102.0f, 127.0f, 6.0f, 102.0f, 126.0f,
};

/* The result must match the vendor bit for bit, and the vendor builds differ
 * in one point: the multiply-adds of the DCTs and of the mark gain.
 *   - unfused (fused = 0): the product is rounded to single precision, then
 *     added (separate mul.s/add.s: the gcc 5.4 builds T21/T23/T30/T31);
 *   - fused (fused = 1): madd.s, which the Ingenic FPU computes with one
 *     rounding (the gcc 4.7.2 builds, the only one of T10/T20 3.12.0, and the
 *     gcc 7.2 T40 build): measured on the T20 camera, where the vendor result
 *     equals fmaf() and differs from the two-rounding result.
 * On the camera the fused form is the madd.s instruction itself; elsewhere
 * (host tests) fmaf().  The volatile store keeps a compiler from fusing the
 * unfused form (gcc 16 contracts despite -ffp-contract pragmas). */
static float wm_madd(float c, float a, float b, int fused)
{
    if (fused) {
#if defined(__mips__) && !defined(__mips_soft_float)
        float r;

        __asm__("madd.s %0, %1, %2, %3" : "=f"(r) : "f"(c), "f"(a), "f"(b));
        return r;
#else
        return fmaf(a, b, c);
#endif
    } else {
        volatile float p = a * b;

        return c + p;
    }
}

static float wm_fabs(float x)
{
    return x < 0.0f ? -x : x;
}

/* coef = D * pix * D^T; each sum runs from 0 over the inner index in order */
static void wm_forward(const float pix[WM_N], float coef[WM_N], int fused)
{
    float t[WM_N];
    int i, j, k;

    for (i = 0; i < 8; i++)
        for (k = 0; k < 8; k++) {
            float s = 0.0f;

            for (j = 0; j < 8; j++) {
                s = wm_madd(s, wm_dct[i * 8 + j], pix[j * 8 + k], fused);
            }
            t[i * 8 + k] = s;
        }
    for (i = 0; i < 8; i++)
        for (k = 0; k < 8; k++) {
            float s = 0.0f;

            for (j = 0; j < 8; j++) {
                s = wm_madd(s, wm_dct[k * 8 + j], t[i * 8 + j], fused);
            }
            coef[i * 8 + k] = s;
        }
}

/* pix = D^T * coef * D */
static void wm_inverse(const float coef[WM_N], float pix[WM_N], int fused)
{
    float t[WM_N];
    int i, j, k;

    for (i = 0; i < 8; i++)
        for (k = 0; k < 8; k++) {
            float s = 0.0f;

            for (j = 0; j < 8; j++) {
                s = wm_madd(s, wm_dct[j * 8 + k], coef[i * 8 + j], fused);
            }
            t[i * 8 + k] = s;
        }
    for (i = 0; i < 8; i++)
        for (k = 0; k < 8; k++) {
            float s = 0.0f;

            for (j = 0; j < 8; j++) {
                s = wm_madd(s, wm_dct[j * 8 + i], t[j * 8 + k], fused);
            }
            pix[i * 8 + k] = s;
        }
}

/* Largest first; any correct sort gives the same ranks (magnitudes: no NaN, no -0). */
static void wm_sort(float v[WM_N])
{
    int i, j;

    for (i = 1; i < WM_N; i++) {
        float x = v[i];

        for (j = i; j > 0 && v[j - 1] < x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
}

/* First coefficient whose magnitude is within WM_EPS of `m` (always found:
 * m is one of the magnitudes). */
static int wm_find(const float mag[WM_N], float m)
{
    int i;

    for (i = 0; i < WM_N; i++) {
        float d = m - mag[i];

        if ((double)wm_fabs(d) < WM_EPS)
            return i;
    }
    return 0;
}

static double wm_sign(float x)
{
    return x > 0.0f ? 1.0 : -1.0;
}

/* Back to 8 bits: add 0.5 and truncate, no clamp (the vendor's double to
 * unsigned conversion stores the low byte). */
static uint8_t wm_pixel(float x)
{
    double v = (double)x + 0.5;

    if (v >= 2147483648.0)
        return (uint8_t)(int32_t)(v - 2147483648.0);
    if (v <= -2147483649.0 || v != v)
        return 0xffu;           /* MIPS trunc.w.d of an invalid value */
    return (uint8_t)(int32_t)v;
}

static void wm_block(float coef[WM_N], int block, int fused)
{
    float mag[WM_N], sorted[WM_N];
    double s_a, s_b, s_r0, s_r2;
    float r1, g;
    int i, k0, k1, k2;

    if (wm_fabs(coef[0]) < WM_DC_MIN)
        memcpy(coef, wm_dark_block, sizeof(wm_dark_block));
    for (i = 0; i < WM_N; i++)
        mag[i] = wm_fabs(coef[i]);
    memcpy(sorted, mag, sizeof(sorted));
    wm_sort(sorted);
    k0 = wm_find(mag, sorted[WM_RANK]);
    k1 = wm_find(mag, sorted[WM_RANK + 1]);
    k2 = wm_find(mag, sorted[WM_RANK + 2]);

    /* signs of the coefficients as they were */
    s_a = wm_sign(coef[WM_COEF_A]);
    s_b = wm_sign(coef[WM_COEF_B]);
    s_r0 = wm_sign(coef[k0]);
    s_r2 = wm_sign(coef[k2]);

    /* rank 31 sets the level; a small one is raised to 0.5 with its sign */
    r1 = coef[k1];
    if (!(wm_fabs(r1) > WM_FLOOR))
        r1 = (float)(wm_sign(r1) * 0.5);
    coef[k1] = r1;
    coef[k0] = (float)((double)wm_fabs(r1) * s_r0);
    /* the stores go in this order: a coefficient found twice takes the last */
    coef[k2] = (float)((double)wm_fabs(coef[k1]) * s_r2);

    g = wm_madd(1.0f, WM_STRENGTH, wm_mark[2 * block], fused);
    coef[WM_COEF_A] = (float)((double)g * ((double)wm_fabs(coef[k2]) * s_a));
    g = wm_madd(1.0f, wm_mark[2 * block + 1], WM_STRENGTH, fused);
    coef[WM_COEF_B] = (float)((double)g * ((double)wm_fabs(coef[k2]) * s_b));
}

int watermark_embed_mode(uint8_t *luma, int stride, int height, int fused)
{
    float pix[WM_N], coef[WM_N];
    int by, bx, i;

    if (!luma || stride < WATERMARK_WIDTH || height < WATERMARK_HEIGHT)
        return -1;
    for (by = 0; by < WATERMARK_HEIGHT; by += 8)
        for (bx = 0; bx < WATERMARK_WIDTH; bx += 8) {
            uint8_t *p = luma + (size_t)by * (size_t)stride + (size_t)bx;

            for (i = 0; i < WM_N; i++)
                pix[i] = (float)p[(i >> 3) * stride + (i & 7)];
            wm_forward(pix, coef, fused);
            wm_block(coef, bx / 8 + (by / 8) * 8, fused);
            wm_inverse(coef, pix, fused);
            for (i = 0; i < WM_N; i++)
                p[(i >> 3) * stride + (i & 7)] = wm_pixel(pix[i]);
        }
    return 0;
}

int watermark_embed(uint8_t *luma, int stride, int height)
{
    return watermark_embed_mode(luma, stride, height, WATERMARK_FUSED_MADD);
}
