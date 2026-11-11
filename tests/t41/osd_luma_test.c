/* T41 IMP_OSD_GetRegionLuma (src/t23/openimp_t23_osd.c) against a literal C
 * transcription of the vendor 1.1.1 code (0xc680c, float average, slot by
 * group 0 / other).  Host test: synthetic NV12 luma planes, no IPU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_system.h>
#include "t23/openimp_t23_osd_abi.h"
#include "t23/openimp_t23_osd.h"

int IMP_OSD_GetRegionLuma(IMPRgnHandle handle, IMPOSDRgnAttr *attr);
int IMP_OSD_CreateGroup(int group);

#define W 640
#define H 360
static uint8_t plane[2][W * H];
static uint8_t fi[2][0x40];             /* IMPFrameInfo: width 0x08, height 0x0c, virt 0x1c */

static void frame(int s, uint32_t w, uint32_t h)
{
    uint32_t v = (uint32_t)(uintptr_t)plane[s];

    memset(fi[s], 0, sizeof(fi[s]));
    memcpy(fi[s] + 0x08, &w, 4);
    memcpy(fi[s] + 0x0c, &h, 4);
    memcpy(fi[s] + 0x1c, &v, 4);
}

/* vendor 1.1.1 loop, transcribed */
static void ref(const uint8_t *base, uint32_t stride, const IMPOSDRgnAttr *a, unsigned *out)
{
    const IMPOSDFontAttrData *fd = &a->fontData;
    unsigned lum = fd->luminance < 256 ? fd->luminance : 200;
    const uint8_t *p = base + stride * a->rect.p0.y + a->rect.p0.x +
                       ((int)fd->data.fontHeight / 2) * stride + (int)fd->data.fontWidth / 2;
    for (unsigned i = 0; i <= fd->length; i++, p += fd->data.fontWidth) {
        unsigned s = p[-2] + p[0] + p[2] + p[-4 * (int)stride] + p[4 * (int)stride];
        out[i] = (unsigned)(int)((double)((float)(int)s / 5.0f) + 0.5) > lum;
    }
}

static void attr(IMPOSDRgnAttr *a, int type, int x, int y, unsigned len, unsigned lum)
{
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->rect.p0.x = x;
    a->rect.p0.y = y;
    a->fontData.invertColorSwitch = 1;
    a->fontData.istimestamp = 1;
    a->fontData.luminance = lum;
    a->fontData.length = len;
    a->fontData.data.fontWidth = 16;
    a->fontData.data.fontHeight = 32;
}

int main(void)
{
    IMPOSDRgnAttr a;
    unsigned want[64];
    int r, i, k;

    srand(7);
    for (i = 0; i < W * H; i++) {
        plane[0][i] = (uint8_t)rand();
        plane[1][i] = (uint8_t)(i % W < W / 2 ? 30 : 230);   /* dark left, bright right */
    }
    attr(&a, OSD_REG_PIC, 8, 8, 19, 190);
    assert(IMP_OSD_GetRegionLuma(0, NULL) == (int)0x80020040);
    assert(IMP_OSD_GetRegionLuma(0, &a) == (int)0x80020008);   /* no frame yet */
    assert(IMP_OSD_CreateGroup(0) == 0 && IMP_OSD_CreateGroup(3) == 0);
    frame(0, W, H);
    frame(1, W, H);
    openimp_t23_osd_apply(0, fi[0]);
    assert(IMP_OSD_GetRegionLuma(1, &a) == (int)0x80020008);   /* slot 1 still empty */
    openimp_t23_osd_apply(3, fi[1]);                            /* any group != 0 -> slot 1 */

    /* random picture, many positions/thresholds: equal to the vendor loop */
    for (k = 0; k < 2000; k++) {
        unsigned len = (unsigned)(rand() % 20), lum = (unsigned)(rand() % 300);
        int x = 2 + rand() % (W - 16 * 21 - 4), y = rand() % (H - 40);

        attr(&a, k % 3 == 0 ? OSD_REG_BITMAP : k % 3 == 1 ? OSD_REG_PIC : OSD_REG_PIC_RMEM, x, y, len, lum);
        ref(plane[0], W, &a, want);
        r = IMP_OSD_GetRegionLuma(0, &a);
        assert(r == 0);
        for (i = 0; i <= (int)len; i++)
            assert(a.fontData.colType[i] == want[i]);
        assert(a.fontData.colType[len + 1] == 0);
    }
    /* slot 1: dark/bright halves */
    attr(&a, OSD_REG_PIC, W / 2 - 4 * 16, 100, 7, 190);
    assert(IMP_OSD_GetRegionLuma(1, &a) == 0);
    for (i = 0; i < 8; i++)
        assert(a.fontData.colType[i] == (i >= 4));
    /* early returns as the vendor */
    attr(&a, OSD_REG_PIC, 8, 8, 3, 190);
    a.fontData.istimestamp = 0;
    a.type = OSD_REG_RECT;
    a.fontData.colType[0] = 7;
    assert(IMP_OSD_GetRegionLuma(0, &a) == 0 && a.fontData.colType[0] == 7);
    a.fontData.istimestamp = 1;
    assert(IMP_OSD_GetRegionLuma(0, &a) == (int)0x80020040);
    a.type = OSD_REG_PIC;
    a.fontData.invertColorSwitch = 0;
    assert(IMP_OSD_GetRegionLuma(0, &a) == 0 && a.fontData.colType[0] == 7);
    /* better than the vendor: slot 2 is refused instead of reading address 0;
     * out-of-frame samples give 0; at most 64 entries */
    a.fontData.invertColorSwitch = 1;
    assert(IMP_OSD_GetRegionLuma(2, &a) == (int)0x80020008);
    a.fontData.istimestamp = 0;
    assert(IMP_OSD_GetRegionLuma(2, &a) == 0);
    attr(&a, OSD_REG_PIC, W - 40, 8, 5, 0);
    memset(plane[1], 255, sizeof(plane[1]));
    assert(IMP_OSD_GetRegionLuma(1, &a) == 0);
    assert(a.fontData.colType[0] == 1 && a.fontData.colType[1] == 1 && a.fontData.colType[2] == 0 &&
           a.fontData.colType[5] == 0);
    attr(&a, OSD_REG_PIC, 2, 8, 200, 0);
    a.fontData.data.fontWidth = 2;
    assert(IMP_OSD_GetRegionLuma(1, &a) == 0);
    assert(a.fontData.colType[63] == 1 && a.mosaicAttr.x == 0);
    puts("T41 OSD GetRegionLuma: all checks passed");
    return 0;
}
