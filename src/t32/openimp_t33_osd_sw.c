/*
 * OpenIMP T33: software OSD drawing as vendor libimp 2.0.2.1 does it on the
 * T33 (no IPU), #675.  See openimp_t33_osd_sw.h; vendor addresses below are
 * file offsets of libimp-t33 2.0.2.1 (md5 818b0f11).
 */
#include <string.h>

#include "t32/openimp_t33_osd_sw.h"

#define T33_OSD_ERR_PARAM ((int)0x80020040)

static inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static inline uint32_t al16(int32_t v) { return ((uint32_t)v + 15u) & ~15u; }

/* (x + 127) / 255 for x >= 0 (vendor: the 0x80808081 multiply) */
static inline uint8_t div255(int32_t x) { return (uint8_t)((x + 127) / 255); }

/*
 * The scaled, offset and clamped rectangle of osd_draw_pic_rgb (0x16f8ec..
 * 0x16f9d8) and of the NV12 branch of osd_draw_pic: each corner times the
 * scale (float, truncated), min / max per axis, plus the offset, then clamped
 * to [0, size - 1].
 */
static void scaled_rect(const T33OsdFrame *f, const T33OsdRgn *r, const T33OsdGrp *g,
                        int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int32_t a = (int32_t)((float)r->x1 * g->scalex), b = (int32_t)((float)r->x0 * g->scalex);
    int32_t c = (int32_t)((float)r->y0 * g->scaley), d = (int32_t)((float)r->y1 * g->scaley);
    int32_t lx = imin(a, b) + g->offx, hx = imax(a, b) + g->offx;
    int32_t ly = imin(c, d) + g->offy, hy = imax(c, d) + g->offy;

    lx = imax(lx, 0);
    hx = imax(hx, 0);
    ly = imax(ly, 0);
    hy = imax(hy, 0);
    *x0 = imin(lx, f->width - 1);
    *x1 = imin(hx, f->width - 1);
    *y0 = imin(ly, f->height - 1);
    *y1 = imin(hy, f->height - 1);
}

/* the luma and chroma of one RGB picture pixel, its alpha (vendor fetch cases
 * of the jump table at 0x1dbfb0 and the common BT.601 integer conversion at
 * 0x16fbc8..0x16fc9c); 0 for a format it does not draw */
static int rgb_pixel(int fmt, const uint8_t *p, int *A, int *R, int *G, int *B)
{
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;

    switch (fmt) {
    case 14:    /* ARGB, 0x16fe68 */
        *A = p[0]; *R = p[1]; *G = p[2]; *B = p[3];
        return 4;
    case 15:    /* RGBA, 0x16fe4c */
        *R = p[0]; *G = p[1]; *B = p[2]; *A = p[3];
        return 4;
    case 16:    /* ABGR, 0x16fe30 */
        *A = p[0]; *B = p[1]; *G = p[2]; *R = p[3];
        return 4;
    case 17:    /* BGRA, 0x16fee4 */
        *B = p[0]; *G = p[1]; *R = p[2]; *A = p[3];
        return 4;
    case 19:    /* RGB565LE, 0x16feb8: opaque */
        *R = (int)((v >> 11) << 3); *G = (int)((v >> 3) & 0xfc); *B = (int)((v << 3) & 0xff); *A = 255;
        return 2;
    case 6:     /* PIX_FMT_RGB555LE as remapped by the vendor (0x16fe84): R5 G5 B5 A1, alpha in bit 0 */
        *R = (int)((v >> 11) << 3); *G = (int)((v >> 3) & 0xf8); *B = (int)((v << 2) & 0xf8); *A = (v & 1) ? 255 : 0;
        return 2;
    case 26:    /* PIX_FMT_BGR555LE as remapped by the vendor (0x16fb54): B5 G5 R5 A1 */
        *B = (int)((v >> 11) << 3); *G = (int)((v >> 3) & 0xf8); *R = (int)((v << 2) & 0xf8); *A = (v & 1) ? 255 : 0;
        return 2;
    default:
        return 0;
    }
}

/* vendor osd_draw_pic_rgb (0x16f894) */
int t33_osd_draw_pic_rgb(const T33OsdFrame *f, const T33OsdRgn *r, const T33OsdGrp *g)
{
    int32_t x0, y0, x1, y1, cols, x, y;
    uint32_t stride, uv0, srow = 0, bpp;
    int fmt, galpha;
    const uint8_t *src;

    if (!f || !r || !r->data)
        return -1;
    scaled_rect(f, r, g, &x0, &y0, &x1, &y1);
    if (x0 == x1 || y0 == y1)
        return T33_OSD_ERR_PARAM;
    if (g->alpha_en > 0 && (uint8_t)g->fg_alpha == 0)
        return 0;
    fmt = r->fmt;
    if ((uint32_t)fmt > 25u || !((1u << fmt) & (0x2280000u | 0x3c000u)))
        return -1;
    if (fmt == 25)
        fmt = 26;
    else if (fmt == 21)
        fmt = 6;
    bpp = ((1u << r->fmt) & 0x3c000u) ? 4u : 2u;
    galpha = g->alpha_en > 0 ? (uint8_t)g->fg_alpha : -1;
    stride = al16(f->width);
    uv0 = stride * al16(f->height);
    cols = x1 - x0 + 1;
    src = r->data;
    for (y = y0; y <= y1; y++, srow += (uint32_t)cols * bpp) {
        uint8_t *yl = f->virt + (uint32_t)y * stride;
        uint8_t *uvl = f->virt + uv0 + (uint32_t)(y / 2) * stride;

        for (x = x0; x <= x1; x++) {
            int A, R, G, B, a, Y, U, V;

            if (!rgb_pixel(fmt, src + srow + (uint32_t)(x - x0) * bpp, &A, &R, &G, &B))
                return -1;
            if (galpha >= 0)
                a = galpha;
            else if (A == 0)
                continue;
            else
                a = A;
            Y = (66 * R + 129 * G + 25 * B + 128) >> 8;
            U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
            V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
            yl[x] = div255(yl[x] * (255 - a) + (Y & 0xff) * a);
            if (((x | y) & 1) == 0) {
                uvl[x] = div255(uvl[x] * (255 - a) + (U & 0xff) * a);
                uvl[x + 1] = div255(uvl[x + 1] * (255 - a) + (V & 0xff) * a);
            }
        }
    }
    return 0;
}

/* vendor osd_draw_pic (0x170108) */
int t33_osd_draw_pic(const T33OsdFrame *f, T33OsdRgn *r, T33OsdGrp *g)
{
    int32_t x0, y0, x1, y1, rows, cols, picw, i;
    uint32_t stride, a = 0xff;
    const uint8_t *src;
    uint8_t *dst;

    if (!f || !r || !g)
        return -1;
    if (g->scalex == 0.0f)
        g->scalex = 1.0f;
    if (g->scaley == 0.0f)
        g->scaley = 1.0f;
    if ((uint32_t)r->fmt < 26u && ((1u << r->fmt) & 0x22bc000u))
        return t33_osd_draw_pic_rgb(f, r, g);
    if (r->fmt != 10)
        return 0;                       /* the vendor logs "not support" and returns 0 */
    /* PIX_FMT_NV12 (0x400 branch) */
    if (r->x1 <= r->x0 || r->y1 <= r->y0)
        return T33_OSD_ERR_PARAM;
    scaled_rect(f, r, g, &x0, &y0, &x1, &y1);
    if (x0 == x1 || y0 == y1)
        return T33_OSD_ERR_PARAM;
    if (g->alpha_en > 0) {
        a = (uint8_t)g->fg_alpha;
        if (!a)
            return 0;
    }
    stride = al16(f->width);
    picw = r->x1 - r->x0 + 1;
    rows = y1 - y0 + 1;
    cols = x1 - x0 + 1;
    src = r->data;
    dst = f->virt + (uint32_t)y0 * stride + (uint32_t)x0;
    for (i = 0; i < rows; i++, dst += stride, src += picw) {
        if (a == 0xff)
            memcpy(dst, src, (size_t)cols);
        else
            for (int k = 0; k < cols; k++)
                dst[k] = (uint8_t)((dst[k] * (0xffu - a) + src[k] * a) >> 8);
    }
    if (rows / 2 < 1)
        return 0;
    dst = f->virt + (uint32_t)(y0 / 2) * stride + al16(f->height) * stride + (uint32_t)x0;
    src = (const uint8_t *)r->data + (uint32_t)((r->y1 - r->y0 + 1) * picw);
    for (i = 0; i < rows / 2; i++, dst += stride, src += picw) {
        if (a == 0xff)
            memcpy(dst, src, (size_t)cols);
        else {
            int pairs = (int)(((uint32_t)(x1 - x0) & ~1u) / 2u) + 1;

            for (int k = 0; k < pairs; k++) {
                dst[2 * k + 1] = (uint8_t)((src[2 * k + 1] * a + dst[2 * k + 1] * (0xffu - a)) >> 8);
                dst[2 * k] = (uint8_t)((dst[2 * k] * (0xffu - a) + src[2 * k] * a) >> 8);
            }
        }
    }
    return 0;
}

/* the cover of the vendor type-1 path (0x170d8c..0x171054, 0x1712f4..0x171450) */
int t33_osd_draw_cover(const T33OsdFrame *f, T33OsdRgn *r, T33OsdGrp *g, uint32_t colour)
{
    int32_t x0, y0, x1, y1, a, b, c, d, y, k;
    uint32_t stride, uv0, al = 255;
    uint8_t Y, U, V;

    if (g->scalex == 0.0f)
        g->scalex = 1.0f;
    if (g->scaley == 0.0f)
        g->scaley = 1.0f;
    a = (int32_t)((float)r->x0 * g->scalex);
    b = (int32_t)((float)r->x1 * g->scalex);
    c = (int32_t)((float)r->y0 * g->scaley);
    d = (int32_t)((float)r->y1 * g->scaley);
    x0 = imax(imin(a, b) + g->offx, 0);
    x1 = imax(imax(a, b) + g->offx, 0);
    y0 = imax(imin(c, d) + g->offy, 0);
    y1 = imax(imax(c, d) + g->offy, 0);
    x0 = imin(x0, f->width - 1);
    x1 = imin(x1, f->width - 1);
    y0 = imin(y0, f->height - 1);
    y1 = imin(y1, f->height - 1);
    if (x0 == x1 || y0 == y1)
        return T33_OSD_ERR_PARAM;       /* the vendor logs and draws nothing */
    if (g->alpha_en > 0) {
        al = (uint8_t)g->fg_alpha;
        if (!al)
            return 0;
    }
    switch (colour) {
    case 0xff00ff00u: Y = 149; U = 43; V = 54; break;      /* green */
    case 0xff0000ffu: Y = 36; U = 240; V = 117; break;     /* blue */
    case 0xffff0000u: Y = 81; U = 90; V = 240; break;      /* red */
    case 0xffffffffu: Y = 235; U = 128; V = 128; break;    /* white */
    default: Y = 16; U = 128; V = 128; break;              /* black, also every other colour */
    }
    stride = al16(f->width);
    uv0 = al16(f->height) * stride;
    for (y = y0; y <= y1; y++) {
        uint8_t *p = f->virt + (uint32_t)y * stride + (uint32_t)x0;

        if (al == 255)
            memset(p, Y, (size_t)(x1 - x0 + 1));
        else
            for (k = 0; k <= x1 - x0; k++)
                p[k] = (uint8_t)((p[k] * (255u - al) + Y * al) >> 8);
    }
    for (y = y0 / 2; y <= y1 / 2; y++) {
        uint8_t *p = f->virt + uv0 + (uint32_t)y * stride + 2u * (uint32_t)(x0 / 2);

        for (k = 0; k <= x1 / 2 - x0 / 2; k++, p += 2) {
            if (al == 255) {
                p[0] = U;
                p[1] = V;
            } else {
                p[0] = (uint8_t)((p[0] * (255u - al) + U * al) >> 8);
                p[1] = (uint8_t)((p[1] * (255u - al) + V * al) >> 8);
            }
        }
    }
    return 0;
}

/* the bitmap of the vendor update (0x17105c..0x171204, both chip types) */
int t33_osd_draw_bitmap(const T33OsdFrame *f, T33OsdRgn *r, const T33OsdGrp *g)
{
    uint32_t wm = (uint32_t)f->width - 1u, hm = (uint32_t)f->height - 1u, stride, uvh, off = 0;
    int32_t lx, hx, ly, hy, x0, x1, y0, y1, w, y;
    const uint8_t *src = r->data;

    /* the rectangle clamped to the frame in the attribute itself (unsigned compares) */
    if (wm < (uint32_t)r->x0)
        r->x0 = (int32_t)wm;
    if (wm < (uint32_t)r->x1)
        r->x1 = (int32_t)wm;
    if (hm < (uint32_t)r->y0)
        r->y0 = (int32_t)hm;
    if (hm < (uint32_t)r->y1)
        r->y1 = (int32_t)hm;
    lx = imin(r->x0, r->x1) + g->offx;
    hx = imax(r->x0, r->x1) + g->offx;
    ly = imin(r->y0, r->y1) + g->offy;
    hy = imax(r->y0, r->y1) + g->offy;
    x0 = imin(imax(lx, 0), (int32_t)wm);
    x1 = imin(imax(hx, 0), (int32_t)wm);
    y0 = imin(imax(ly, 0), (int32_t)hm);
    y1 = imin(imax(hy, 0), (int32_t)hm);
    if (y1 < y0 || !src)
        return 0;
    w = x1 - x0 + 1;
    stride = al16(f->width);
    uvh = al16(f->height);
    for (y = y0; y <= y1; y++, off += (uint32_t)w) {
        uint8_t *yl = f->virt + (uint32_t)y * stride;
        uint8_t *uvl = f->virt + ((uint32_t)(y / 2) + uvh) * stride;

        if (x1 < x0)
            continue;
        for (int32_t x = x0; x <= x1; x++) {
            uint8_t v = src[off + (uint32_t)(x - x0)];

            if (v) {
                yl[x] = v;
                uvl[x] = 0x80;
            }
        }
    }
    return 0;
}
