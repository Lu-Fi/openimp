/*
 * OpenIMP T33: IMP_OSD regions drawn by the CPU into the NV12 frame (#675).
 *
 * The T33 has no IPU (datasheet T33L v0.6 "Image Engine" = LDC only, the
 * 0x13080000 block does not take register writes on the T33L).  Vendor libimp
 * 2.0.2.1 knows this: its OSD group update (static function at 0x170794,
 * registered by IMP_OSD_CreateGroup) asks system_get_cpu_type() (T33 ids 13..21
 * -> type 1) and then never calls the IPU drawer; pictures go through
 * osd_draw_pic (0x170108) / osd_draw_pic_rgb (0x16f894), covers and bitmaps
 * are drawn inline (0x170d7c / 0x17105c), lines / rectangles / mosaics by the
 * same CPU helpers as on the T32.  The region-type jump table (0x1dc004):
 * 1 2 9 lines, 3 rectangle, 5 bitmap, 6 cover (type 1 only), 7 8 picture
 * (type 1 only), 13 mosaic; 0 4 10 11 12 are not drawn (also the four-corner
 * rectangle).
 *
 * These are reimplementations of those vendor flows, checked pixel for pixel
 * against the vendor code under an emulator (tools/t33_osd_oracle.py,
 * tests/t33/osd_sw_test.c).  Frame: NV12 with the luma pitch ALIGN16(width)
 * and the chroma plane at ALIGN16(width) * ALIGN16(height), as the vendor
 * computes it.  Like the vendor, the functions write back into the attributes
 * they get: a zero scale factor becomes 1.0, a bitmap rectangle is clamped
 * to the frame.
 */
#ifndef OPENIMP_T33_OSD_SW_H
#define OPENIMP_T33_OSD_SW_H

#include <stdint.h>

/* the frame fields the vendor reads (IMPFrameInfo +8 width, +0xc height, +0x1c virt) */
typedef struct {
    int32_t width, height;
    uint8_t *virt;
} T33OsdFrame;

/* view of IMPOSDRgnAttr (vendor 2.0.2.1 layout: type, rect, line, fmt at 0x1c, data at 0x20) */
typedef struct {
    int32_t type;
    int32_t x0, y0, x1, y1;         /* rect.p0, rect.p1 */
    int32_t line[2];
    int32_t fmt;                    /* IMPPixelFormat */
    void *data;                     /* bitmapData / picData.pData; cover: the colour word */
} T33OsdRgn;

/* view of IMPOSDGrpRgnAttr */
typedef struct {
    int32_t show;
    int32_t offx, offy;
    float scalex, scaley;
    int32_t alpha_en, fg_alpha, bg_alpha, layer;
} T33OsdGrp;

/* vendor osd_draw_pic: PIX_FMT_NV12 pictures and the RGB formats ARGB, RGBA,
 * ABGR, BGRA, RGB565LE, RGB555LE, BGR555LE; 0, -1 or 0x80020040 as the vendor */
int t33_osd_draw_pic(const T33OsdFrame *f, T33OsdRgn *r, T33OsdGrp *g);
/* vendor osd_draw_pic_rgb (the RGB formats only) */
int t33_osd_draw_pic_rgb(const T33OsdFrame *f, const T33OsdRgn *r, const T33OsdGrp *g);
/* the vendor cover of the type-1 path: five colours (white, black, red,
 * green, blue; any other colour is black), global alpha; 0 drawn or skipped */
int t33_osd_draw_cover(const T33OsdFrame *f, T33OsdRgn *r, T33OsdGrp *g, uint32_t colour);
/* the vendor bitmap: one luma byte per pixel, 0 = transparent, chroma 0x80 */
int t33_osd_draw_bitmap(const T33OsdFrame *f, T33OsdRgn *r, const T33OsdGrp *g);

#endif
