/* CPU drawing for OSD_REG_LINE, OSD_REG_RECT and OSD_REG_BITMAP on NV12
 * frames (T31, T20, T21, T30 share this).  Header-only and free of hardware
 * dependencies so it can be unit tested on the host (tests/t31/osd_draw_test.c).
 *
 * Semantics follow the stock libimp osd_draw_line / OSD bitmap path
 * (libimp.so_hlil.txt, docs/T31_OSD_RE.md section 3.6):
 *  - opaque writes: Y byte per pixel, U,V pair at (x & ~1) of chroma row y/2
 *  - colour is the stock word 0xAAYYUUVV (alpha ignored when drawing)
 *  - line width lw: a pixel position p covers [p - lw/2, p - lw/2 + lw - 1]
 *  - a line whose two endpoints are equal draws nothing
 *  - endpoints are clamped to the frame, the group offPos is added, and the
 *    result is clamped again (stock clamps negative raw coordinates to w-1
 *    through an unsigned compare; here they go to 0)
 *  - near-vertical lines (|dx| < 2*lw) are one filled box centred on the
 *    midpoint x; other lines step the dominant axis from p0 towards p1 (p1
 *    itself excluded) and stamp lw x lw squares at y0 + floor(s * dy / dx)
 *    (stock: float slope, truncated). Only the part of each stamp that the
 *    previous one did not cover is written, so a line costs O(len * lw).
 *  - RECT is the four edges p0.x/p1.x/p0.y/p1.y
 * Every write is clipped to the frame; nothing outside [0,w) x [0,h) of the
 * Y plane or the matching chroma bytes is touched. */
#ifndef OPENIMP_T31_OSD_DRAW_H
#define OPENIMP_T31_OSD_DRAW_H

#include <stdint.h>
#include <string.h>

/* Box the dry pass wrote, in Y rows/columns (inclusive, inside the frame).
 * Recorded so the caller can sync only what the CPU touched instead of the
 * whole ymin..ymax band: a RECT records one per edge, a LINE one (or, for a
 * diagonal, one per stamp), a BITMAP one. */
struct osd_span {
    uint16_t ya, yb;
    uint16_t xa, xb;
};

/* Boxes a dry pass may record before it gives up on the fine-grained sync and
 * falls back to the whole touched band. 64 RECT regions (the region limit)
 * use four spans each, so a frame of full-frame borders still fits; a diagonal
 * line (one span per stamp) or a large bitmap overflows and keeps the old
 * behaviour. */
#define OSD_DRAW_MAX_SPANS 256

struct osd_canvas {
    uint8_t *base;      /* Y plane; UV plane at base + uv_off */
    uint32_t width;     /* visible size, width must be even */
    uint32_t height;
    uint32_t stride;    /* >= width, even */
    size_t uv_off;      /* byte offset of the UV plane */
    int ymin, ymax;     /* touched Y rows (ymin > ymax: nothing) */
    int dry;            /* 1: only compute ymin/ymax, write nothing */
    struct osd_span spans[OSD_DRAW_MAX_SPANS];  /* dry pass: written boxes */
    int nspans;         /* valid when !span_overflow */
    int span_overflow;  /* more boxes than spans[]: sync the whole band */
};

/* Canvas limit, and the line width cap. With lw >= 2 * max(w, h) every
 * stamp and box already covers the whole frame axis, so capping there
 * changes nothing and keeps 2 * lw and the int math in range. */
#define OSD_DRAW_MAX_DIM 8192
#define OSD_DRAW_MAX_LW (2 * OSD_DRAW_MAX_DIM)

static inline void osd_canvas_init(struct osd_canvas *c, uint8_t *base,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride, size_t uv_off)
{
    c->base = base;
    c->width = width;
    c->height = height;
    c->stride = stride;
    c->uv_off = uv_off;
    c->ymin = (int)height;
    c->ymax = -1;
    c->dry = 0;
    c->nspans = 0;
    c->span_overflow = 0;
}

static inline int osd_canvas_valid(const struct osd_canvas *c)
{
    return c->base && c->width >= 2 && c->height >= 1 && !(c->width & 1u) &&
           c->width <= OSD_DRAW_MAX_DIM && c->height <= OSD_DRAW_MAX_DIM &&
           c->stride >= c->width && !(c->stride & 1u);
}

static inline int osd_clampi(int64_t v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

/* Record one written box during the dry pass (a no-op in a real pass: the
 * plan comes from the dry pass and must not change while it is used). */
static inline void osd_note_span(struct osd_canvas *c, int xa, int ya, int xb,
                                 int yb)
{
    if (!c->dry || c->span_overflow)
        return;
    if (c->nspans >= OSD_DRAW_MAX_SPANS) {
        c->span_overflow = 1;
        return;
    }
    c->spans[c->nspans].xa = (uint16_t)xa;
    c->spans[c->nspans].xb = (uint16_t)xb;
    c->spans[c->nspans].ya = (uint16_t)ya;
    c->spans[c->nspans].yb = (uint16_t)yb;
    c->nspans++;
}

/* Fill the inclusive box [xa,xb] x [ya,yb], already inside the frame. */
static inline void osd_fill_box(struct osd_canvas *c, int xa, int ya, int xb,
                                int yb, uint32_t word)
{
    uint8_t y = (uint8_t)(word >> 16), u = (uint8_t)(word >> 8),
            v = (uint8_t)word;
    int cx0, cx1, row;

    if (xa > xb || ya > yb || xa < 0 || ya < 0 ||
        xb >= (int)c->width || yb >= (int)c->height)
        return;
    osd_note_span(c, xa, ya, xb, yb);
    if (c->dry)
        goto band;
    cx0 = xa & ~1;
    cx1 = (xb & ~1) + 1;            /* width is even: always < width */
    for (row = ya; row <= yb; row++) {
        memset(c->base + (size_t)row * c->stride + xa, y, (size_t)(xb - xa + 1));
        /* both rows of a chroma pair write the same bytes: do it once */
        if (!(row & 1) || row == ya) {
            uint8_t *uv = c->base + c->uv_off + (size_t)(row >> 1) * c->stride;
            int x;

            for (x = cx0; x <= cx1; x += 2) {
                uv[x] = u;
                uv[x + 1] = v;
            }
        }
    }
band:
    if (ya < c->ymin)
        c->ymin = ya;
    if (yb > c->ymax)
        c->ymax = yb;
}

/* Range clip (the near-vertical box rows) */
static inline void osd_box_clip(struct osd_canvas *c, int64_t xa, int64_t ya,
                                int64_t xb, int64_t yb, uint32_t word)
{
    int w1 = (int)c->width - 1, h1 = (int)c->height - 1;

    if (xb < 0 || yb < 0 || xa > w1 || ya > h1)
        return;
    osd_fill_box(c, osd_clampi(xa, 0, w1), osd_clampi(ya, 0, h1),
                 osd_clampi(xb, 0, w1), osd_clampi(yb, 0, h1), word);
}

struct osd_box {
    int xa, ya, xb, yb;     /* inclusive, inside the frame */
};

/* lw x lw stamp whose top-left is (x - lw/2, y - lw/2); a start before the
 * frame is moved to 0 (the stamp keeps its width), as in stock. */
static inline struct osd_box osd_stamp_box(const struct osd_canvas *c, int64_t x,
                                           int64_t y, int lw)
{
    int w1 = (int)c->width - 1, h1 = (int)c->height - 1;
    struct osd_box b;

    b.xa = osd_clampi(x - lw / 2, 0, w1);
    b.ya = osd_clampi(y - lw / 2, 0, h1);
    b.xb = osd_clampi((int64_t)b.xa + lw - 1, 0, w1);
    b.yb = osd_clampi((int64_t)b.ya + lw - 1, 0, h1);
    return b;
}

static inline int osd_maxi(int a, int b) { return a > b ? a : b; }
static inline int osd_mini(int a, int b) { return a < b ? a : b; }

/* Fill n minus o (o == NULL: all of n). The union of all stamps is the same
 * as stamping each one fully, since every pixel of n not written here is in
 * o, which was written before. */
static inline void osd_fill_new(struct osd_canvas *c, const struct osd_box *n,
                                const struct osd_box *o, uint32_t word)
{
    int ox0, ox1;

    if (!o || n->xa > o->xb || n->xb < o->xa || n->ya > o->yb || n->yb < o->ya) {
        osd_fill_box(c, n->xa, n->ya, n->xb, n->yb, word);
        return;
    }
    /* columns of n outside o, all rows of n */
    osd_fill_box(c, n->xa, n->ya, osd_mini(n->xb, o->xa - 1), n->yb, word);
    osd_fill_box(c, osd_maxi(n->xa, o->xb + 1), n->ya, n->xb, n->yb, word);
    /* columns shared with o, rows of n outside o */
    ox0 = osd_maxi(n->xa, o->xa);
    ox1 = osd_mini(n->xb, o->xb);
    osd_fill_box(c, ox0, n->ya, ox1, osd_mini(n->yb, o->ya - 1), word);
    osd_fill_box(c, ox0, osd_maxi(n->ya, o->yb + 1), ox1, n->yb, word);
}

/* floor(n / d), d != 0 */
static inline int64_t osd_floordiv(int64_t n, int64_t d)
{
    int64_t q = n / d;

    if ((n % d) != 0 && ((n < 0) != (d < 0)))
        q--;
    return q;
}

static inline void osd_draw_line(struct osd_canvas *c, int x0, int y0, int x1,
                                 int y1, uint32_t linewidth, uint32_t word,
                                 int offx, int offy)
{
    int w1 = (int)c->width - 1, h1 = (int)c->height - 1;
    int lw = linewidth > OSD_DRAW_MAX_LW ? OSD_DRAW_MAX_LW : (int)linewidth;
    int ax, ay, bx, by, dx, dy, adx, ady, k;
    struct osd_box prev = {0, 0, -1, -1}, cur;

    /* stock osd_draw_line returns at once for p0 == p1 */
    if (lw <= 0 || (x0 == x1 && y0 == y1) || !osd_canvas_valid(c))
        return;
    /* clamp, add offPos, clamp again (stock order) */
    ax = osd_clampi((int64_t)osd_clampi(x0, 0, w1) + offx, 0, w1);
    ay = osd_clampi((int64_t)osd_clampi(y0, 0, h1) + offy, 0, h1);
    bx = osd_clampi((int64_t)osd_clampi(x1, 0, w1) + offx, 0, w1);
    by = osd_clampi((int64_t)osd_clampi(y1, 0, h1) + offy, 0, h1);
    dx = bx - ax;
    dy = by - ay;
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;

    if (adx < 2 * lw) {
        /* near vertical: one box, columns centred on the midpoint (start
         * clamped first, so the box keeps its width at the border), rows
         * clipped */
        int mid = (ax + bx) / 2;
        int xs = osd_clampi((int64_t)mid - lw / 2, 0, w1);
        int ylo = ay < by ? ay : by, yhi = ay < by ? by : ay;

        osd_box_clip(c, xs, (int64_t)ylo - lw / 2, (int64_t)xs + lw - 1,
                     (int64_t)yhi - lw / 2 + lw - 1, word);
        return;
    }
    /* here adx >= 2 * lw > 0 */
    if (ady == 0) {
        /* horizontal: the union of the stamps at p0 .. p1 (p1 excluded),
         * i.e. positions [pmin, pmax] */
        int pmin = ax < bx ? ax : bx + 1;
        int pmax = ax < bx ? bx - 1 : ax;
        struct osd_box lo = osd_stamp_box(c, pmin, ay, lw);
        struct osd_box hi = osd_stamp_box(c, pmax, ay, lw);

        osd_fill_box(c, lo.xa, lo.ya, hi.xb, lo.yb, word);
        return;
    }
    if (ady < adx) {
        int dir = dx > 0 ? 1 : -1;

        for (k = 0; k < adx; k++) {
            int s = k * dir;

            cur = osd_stamp_box(c, (int64_t)ax + s,
                                ay + osd_floordiv((int64_t)dy * s, dx), lw);
            osd_fill_new(c, &cur, k ? &prev : NULL, word);
            prev = cur;
        }
    } else {
        int dir = dy > 0 ? 1 : -1;

        for (k = 0; k < ady; k++) {
            int s = k * dir;

            cur = osd_stamp_box(c, ax + osd_floordiv((int64_t)dx * s, dy),
                                (int64_t)ay + s, lw);
            osd_fill_new(c, &cur, k ? &prev : NULL, word);
            prev = cur;
        }
    }
}

static inline void osd_draw_rect(struct osd_canvas *c, int x0, int y0, int x1,
                                 int y1, uint32_t linewidth, uint32_t word,
                                 int offx, int offy)
{
    osd_draw_line(c, x0, y0, x1, y0, linewidth, word, offx, offy);
    osd_draw_line(c, x0, y0, x0, y1, linewidth, word, offx, offy);
    osd_draw_line(c, x0, y1, x1, y1, linewidth, word, offx, offy);
    osd_draw_line(c, x1, y0, x1, y1, linewidth, word, offx, offy);
}

/* 8-bit mask bitmap of bw x bh at frame position (x0,y0)+off: each non-zero
 * byte writes Y = byte and chroma 0x80 at byte x of chroma row y/2 (stock).
 * Clipped to the frame; the bitmap keeps its own row stride bw. (Stock
 * clamps the rect first and then reads the bitmap packed with the clipped
 * width from its first byte, which shears a bitmap that hangs over the
 * frame edge; for a bitmap inside the frame both are the same.) */
static inline void osd_draw_bitmap(struct osd_canvas *c, const uint8_t *bmp,
                                   int x0, int y0, uint32_t bw, uint32_t bh,
                                   int offx, int offy)
{
    int64_t ox = (int64_t)x0 + offx, oy = (int64_t)y0 + offy;
    int64_t row, col, r0, r1, c0, c1;

    if (!bmp || !bw || !bh || bw > OSD_DRAW_MAX_DIM || bh > OSD_DRAW_MAX_DIM ||
        !osd_canvas_valid(c))
        return;
    r0 = oy < 0 ? -oy : 0;
    r1 = oy + (int64_t)bh > (int64_t)c->height ? (int64_t)c->height - oy : (int64_t)bh;
    c0 = ox < 0 ? -ox : 0;
    c1 = ox + (int64_t)bw > (int64_t)c->width ? (int64_t)c->width - ox : (int64_t)bw;
    if (r0 >= r1 || c0 >= c1)
        return;
    /* The whole clipped rect is one span: the flush may cover bytes the mask
     * left zero, which is safe and still no more than the old band. */
    osd_note_span(c, (int)(ox + c0), (int)(oy + r0), (int)(ox + c1 - 1),
                  (int)(oy + r1 - 1));
    for (row = c->dry ? r1 : r0; row < r1; row++) {
        uint8_t *yp = c->base + (size_t)(oy + row) * c->stride + ox;
        uint8_t *uv = c->base + c->uv_off + (size_t)((oy + row) >> 1) * c->stride + ox;
        const uint8_t *src = bmp + (size_t)row * bw;

        for (col = c0; col < c1; col++) {
            if (src[col]) {
                yp[col] = src[col];
                uv[col] = 0x80;
            }
        }
    }
    if ((int)(oy + r0) < c->ymin)
        c->ymin = (int)(oy + r0);
    if ((int)(oy + r1 - 1) > c->ymax)
        c->ymax = (int)(oy + r1 - 1);
}

/*
 * Cache maintenance for the CPU-drawn regions.
 *
 * The dry pass records every box it would write (osd_note_span); this walks
 * that plan and calls fn() once per contiguous byte range of the frame buffer
 * (offset from c->base, so Y and UV come out of the same plan). Everything the
 * CPU writes is covered, and nothing outside the frame buffer is touched:
 *  - Y: a span that covers the full row and a tight stride is one range over
 *    its rows, any other span is one range per row over [xa, xb].
 *  - UV: rows ya/2..yb/2, the bytes the box writes there are
 *    [xa & ~1, (xb & ~1) + 1] (a superset of the BITMAP path, which writes one
 *    chroma byte per column), again one range per row unless the full chroma
 *    row is written and the stride is tight.
 * Without a plan (nothing recorded, or more boxes than spans[] - a diagonal
 * LINE, a large BITMAP) it syncs the whole ymin..ymax band of both planes,
 * exactly what this used to do unconditionally.
 */
typedef void (*osd_sync_fn)(void *ctx, uint32_t off, uint32_t len);

static inline void osd_canvas_sync(const struct osd_canvas *c, osd_sync_fn fn,
                                   void *ctx)
{
    uint32_t stride = c->stride, width = c->width;
    int i;

    if (!c->base || c->ymin > c->ymax)
        return;
    if (c->span_overflow || c->nspans <= 0) {
        uint32_t y0 = (uint32_t)c->ymin, y1 = (uint32_t)c->ymax + 1u;

        fn(ctx, y0 * stride, (y1 - y0) * stride);
        fn(ctx, (uint32_t)c->uv_off + (y0 / 2u) * stride,
           ((y1 + 1u) / 2u - y0 / 2u) * stride);
        return;
    }
    for (i = 0; i < c->nspans; i++) {
        int xa = c->spans[i].xa, xb = c->spans[i].xb;
        int ya = c->spans[i].ya, yb = c->spans[i].yb;
        int cxa = xa & ~1, cxb = (xb & ~1) + 1, y, cy;
        uint32_t clen = (uint32_t)(cxb - cxa) + 1u;

        if (xa == 0 && xb == (int)width - 1 && stride == width) {
            fn(ctx, (uint32_t)ya * stride, (uint32_t)(yb - ya + 1) * stride);
        } else {
            for (y = ya; y <= yb; y++)
                fn(ctx, (uint32_t)y * stride + (uint32_t)xa,
                   (uint32_t)(xb - xa) + 1u);
        }
        if (cxa == 0 && clen >= width && stride == width) {
            fn(ctx, (uint32_t)c->uv_off + (uint32_t)(ya / 2) * stride,
               (uint32_t)(yb / 2 - ya / 2 + 1) * stride);
        } else {
            for (cy = ya / 2; cy <= yb / 2; cy++)
                fn(ctx, (uint32_t)c->uv_off + (uint32_t)cy * stride +
                        (uint32_t)cxa, clen);
        }
    }
}

#endif
