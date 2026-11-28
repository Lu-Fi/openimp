/*
 * T33 encoder-side picture scaling (#99): the vendor libimp 2.0.2.1 input thread (FUN_000edba8 in the Ghidra
 * project t33lib, 0xedba8) scales a FrameSource picture whose size differs from the encoder channel with
 * c_resize_c (0x34540): NV12 -> NV12, optional crop, bilinear in 1/16 steps.  QEMU A/B (audit/t33-gap-1128 rs/):
 * this code reproduces the HERA input picture of the vendor run (FrameSource 1280x720, encoder 640x360, 353280
 * bytes incl. the padding rows) byte for byte; the algorithm is the T30 1.0.5 c_resize_c port
 * (framesource_tseries.c), which is therefore the same routine on the T33.
 */
#ifdef PLATFORM_T33
#include <stdint.h>
#include <string.h>

#include "t32/openimp_t33_resize.h"

static unsigned int t33_resize_pos(unsigned int i, int src, int dst, int *ix)
{
    float fx = (float)i * ((float)src / (float)dst);
    int k = (int)fx;

    *ix = k;
    return (unsigned int)(int)((double)((fx - (float)k) * 16.0f) + 0.5);
}

/* libimp 1.0.5 c_resize_c (0x39750), sw_resize on a machine without SIMD128:
 * NV12 -> NV12, optional crop, bilinear with 4-bit weights.
 *   src, p3   source picture and its pitch (= width)
 *   p4        source height, 16-aligned
 *   dst, p5, p6  destination, width, 16-aligned height
 *   crop      crop enabled; p8 = pitch * top + left, p9 / p10 = width and
 *             16-aligned height of the cut-out
 *   tab       T33_RESIZE_TAB(p5, p6) uint16
 * The tables are laid out as in libimp: weight of x0 (16 - w), weight of x1
 * (w), vertical (16 - w), vertical w, x0, x1, y0, y1.  The chroma rows start
 * at pitch * (p4 - (p8 / (2 * pitch))) below the cut-out origin (libimp).
 * 1280x720 -> 1280x960 is libimp's nearest-row special case. */
void openimp_t33_resize_c(const uint8_t *src, uint8_t *dst, int p3, int p4,
                             int p5, int p6, int crop, int p8, int p9, int p10,
                             uint16_t *tab)
{
    const uint8_t *s = src;
    int cw = p3, chh = p4, y0off, hh, i, j;
    uint16_t *wa = tab, *wb = wa + p5, *vc = wb + p5, *vd = vc + p6;
    uint16_t *e = vd + p6, *f = e + p5, *g = f + p5, *h = g + p6;

    y0off = p4 - p8 / (p3 * 2);
    if (crop) {
        s = src + p8;
        chh = p10;
        cw = p9;
    }
    hh = chh >> 1;

    if (cw == p5 && chh == p6) {
        const uint8_t *cs;
        uint8_t *cd = dst + (size_t)cw * chh;

        for (j = 0; j < chh; j++)
            memcpy(dst + (size_t)j * cw, s + (size_t)j * p3, (size_t)cw);
        cs = s + (size_t)p3 * y0off;
        for (j = 0; j < hh; j++)
            memcpy(cd + (size_t)j * cw, cs + (size_t)j * p3, (size_t)cw);
        return;
    }
    if (cw == 0x500 && p5 == 0x500 && chh == 0x2d0 && p6 == 0x3c0) {
        for (j = 0; j < 0x3c0; j++)
            memcpy(dst + (size_t)j * 0x500, s + (size_t)(int)((float)j * 0.75f) * p3,
                   0x500);
        for (j = 0; j < 0x1e0; j++)
            memcpy(dst + (size_t)0x500 * 0x3c0 + (size_t)j * 0x500,
                   s + (size_t)p3 * chh + (size_t)(int)((float)j * 0.75f) * p3, 0x500);
        return;
    }

    /* luma tables */
    for (i = 0; i < p5; i++) {
        int k;
        unsigned int w = t33_resize_pos((unsigned int)i, cw, p5, &k);

        wb[i] = (uint16_t)w;
        wa[i] = (uint16_t)(16u - w);
        e[i] = (uint16_t)k;
        f[i] = (uint16_t)(k < cw - 1 ? k + 1 : k);
    }
    for (j = 0; j < p6; j++) {
        int k;
        unsigned int w = t33_resize_pos((unsigned int)j, chh, p6, &k);

        vd[j] = (uint16_t)w;
        vc[j] = (uint16_t)(16u - w);
        g[j] = (uint16_t)k;
        h[j] = (uint16_t)(k < chh - 1 ? k + 1 : k);
    }
    for (j = 0; j < p6; j++) {
        uint8_t *o = dst + (size_t)j * p5;
        const uint8_t *r0 = s + (size_t)g[j] * p3, *r1 = s + (size_t)h[j] * p3;
        unsigned int wy = vd[j], wc = vc[j];

        for (i = 0; i < p5; i++) {
            unsigned int a = wa[i], b = wb[i];

            if (wy == 0)
                o[i] = (uint8_t)((r0[f[i]] * b + r0[e[i]] * a) >> 4);
            else
                o[i] = (uint8_t)((r1[e[i]] * a * wy +
                                  (r0[f[i]] * b + r0[e[i]] * a) * wc +
                                  r1[f[i]] * b * wy) >> 8);
        }
    }

    /* chroma tables (they reuse the same storage) */
    for (i = 0; i < p5 / 2; i++) {
        int k;
        unsigned int w = t33_resize_pos((unsigned int)i, cw, p5, &k);

        wb[2 * i] = wb[2 * i + 1] = (uint16_t)w;
        wa[2 * i] = wa[2 * i + 1] = (uint16_t)(16u - w);
        e[i] = (uint16_t)k;
        f[i] = (uint16_t)(k < (cw >> 1) - 1 ? k + 1 : k);
    }
    for (j = 0; j < p6 / 2; j++) {
        int k;
        unsigned int w = t33_resize_pos((unsigned int)j, chh, p6, &k);

        vd[j] = (uint16_t)w;
        vc[j] = (uint16_t)(16u - w);
        g[j] = (uint16_t)(y0off + k);
        h[j] = (uint16_t)(k < hh - 1 ? y0off + k + 1 : y0off + k);
    }
    for (j = 0; j < p6 / 2; j++) {
        uint8_t *o = dst + (size_t)p5 * p6 + (size_t)j * p5;
        const uint8_t *r0 = s + (size_t)g[j] * p3, *r1 = s + (size_t)h[j] * p3;
        unsigned int wy = vd[j], wc = vc[j];

        for (i = 0; i < p5; i += 2) {
            const uint8_t *p00 = r0 + 2u * e[i >> 1], *p01 = r0 + 2u * f[i >> 1];
            const uint8_t *p10_ = r1 + 2u * e[i >> 1], *p11 = r1 + 2u * f[i >> 1];
            int c;

            for (c = 0; c < 2; c++) {
                unsigned int a = wa[i + c], b = wb[i + c];

                if (wy == 0)
                    o[i + c] = (uint8_t)((p01[c] * b + p00[c] * a) >> 4);
                else
                    o[i + c] = (uint8_t)((p10_[c] * a * wy +
                                          (p01[c] * b + p00[c] * a) * wc +
                                          p11[c] * b * wy) >> 8);
            }
        }
    }
}


#endif /* PLATFORM_T33 */
