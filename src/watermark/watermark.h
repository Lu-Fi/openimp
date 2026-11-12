/*
 * IMP_Encoder_SetFisheyeEnableStatus, picture part: the vendor's invisible
 * luma watermark (issues #357, #397).
 *
 * In every vendor libimp that has the call (T10/T20 3.12.0, T21 1.0.33,
 * T23 1.3.0, T30 1.0.5, T31 1.1.6, T40 1.3.1; T41 1.2.6 has neither the call
 * nor the routine) the "fisheye" flag does no lens correction.  The encoder
 * allocates a watermark context when the channel is created with the flag
 * set and, for every I picture, rewrites the top-left 64x16 luma pixels of
 * the source picture in place before the hardware reads it:
 *
 *   - the area is cut into 16 8x8 blocks (8 across, 2 down);
 *   - each block gets an orthonormal 8x8 DCT (single precision, the vendor's
 *     six-digit cosine table); a block whose DC magnitude is below 80 (mean
 *     luma below 10) is replaced by a fixed coefficient block;
 *   - the coefficients with the 31st, 32nd and 33rd largest magnitude are
 *     levelled to the 32nd (a magnitude of 0.5 or less becomes 0.5), each
 *     keeping its sign; coefficients 33 and 12 (raster order) are set to
 *     (1 + 0.1 * mark) times the levelled magnitude, keeping their sign,
 *     where mark is the block's pair of a fixed 32-value pattern;
 *   - the inverse DCT goes back to pixels, rounded by adding 0.5 and
 *     truncating to 8 bits without clamping (a result of 256 is stored as 0,
 *     like the vendor).
 *
 * The routine is the same in all those libraries apart from how the gcc
 * build rounds its multiply-adds (WATERMARK_FUSED_MADD below):
 * tools/watermark_oracle.py (unicorn, which rounds madd.s twice) gives the
 * same output for all of them, and the unfused builds give that output on the
 * camera too; the fused builds are checked on the camera
 * (tools/watermark_devcheck.c).  Which pictures and which plane geometry the encoders hand to it
 * is per family, see watermark_embed() users and docs/WATERMARK.md.
 *
 * Pure C: tests/watermark checks it byte for byte against the vendor routine
 * run under unicorn (tools/watermark_oracle.py).
 */
#ifndef OPENIMP_WATERMARK_H
#define OPENIMP_WATERMARK_H

#include <stdint.h>

#define WATERMARK_WIDTH  64     /* pixels rewritten per row */
#define WATERMARK_HEIGHT 16     /* rows rewritten */

/* The vendor build this libimp follows (the one Thingino ships per SoC):
 * 1 = fused multiply-adds, madd.s (the gcc 4.7.2 builds of T10/T20 3.12.0,
 * T21 1.0.33 and T30 1.0.5, and the gcc 7.2 T40 1.3.1 build), 0 = separate
 * multiply and add (the gcc 5.4 builds: T23 1.3.0, T31 1.1.6).  See
 * watermark.c. */
#ifndef WATERMARK_FUSED_MADD
#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30) || defined(PLATFORM_T40)
#define WATERMARK_FUSED_MADD 1
#else
#define WATERMARK_FUSED_MADD 0
#endif
#endif

/* Rewrite the top-left 64x16 pixels of the luma plane `luma` (`stride` bytes
 * per row, `height` rows).  Returns 0, or -1 without touching the plane when
 * stride < 64 or height < 16 (the vendor's checks). */
int watermark_embed(uint8_t *luma, int stride, int height);
/* the same with the multiply-add form chosen (tests) */
int watermark_embed_mode(uint8_t *luma, int stride, int height, int fused);

#endif
