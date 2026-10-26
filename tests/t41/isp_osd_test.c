/* T41 ISP-drawn OSD (src/t40/openimp_t40_isp_osd.c built with PLATFORM_T41):
 * region table, picture copy, show/overlap and the two driver controls
 * 0x080000a1 (IMPISPOSDAttr) / 0x080000a2 (IMPISPOSDBlockAttr) per region,
 * as the vendor T41 libimp 1.2.6 OsdnotifyIsp sends them; the call sequence
 * is the one of raptor-hal src/hal_osd.c.  Host test with fake rmem, frame
 * source and tuning ioctl; needs the vendor T41 headers. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>

#include "dma_alloc.h"

int IMP_OSD_SetPoolSize_ISP(int size);
void IMP_OSD_Exit_ISP(void);

/* ---- fakes ---- */
static struct {
    int32_t vinum, direction, control;
    unsigned char payload[sizeof(IMPISPOSDBlockAttr) > sizeof(IMPISPOSDAttr) ?
                          sizeof(IMPISPOSDBlockAttr) : sizeof(IMPISPOSDAttr)];
} sent[64];
static int nsent, tuning_on = 1, ioctl_fail;
static int nalloc, nfree, nflush;
static unsigned char (*rmem)[4096];   /* below 4 GiB: virt_addr is 32-bit */

int OpenIMP_P1_TuningEnabled(void);
int OpenIMP_P1_TuningEnabled(void)
{
    return tuning_on;
}

int OpenIMP_P1_TuningIOCtl(uint32_t command, void *argument)
{
    const struct {
        int32_t vinum, direction, control;
        uintptr_t payload;
    } *request = argument;

    assert(command == 0xc0105435U);
    assert(nsent < 64);
    if (ioctl_fail)
        return -1;
    sent[nsent].vinum = request->vinum;
    sent[nsent].direction = request->direction;
    sent[nsent].control = request->control;
    memcpy(sent[nsent].payload, (void *)request->payload,
           request->control == 0x080000a1 ? sizeof(IMPISPOSDAttr) :
                                             sizeof(IMPISPOSDBlockAttr));
    nsent++;
    return 0;
}

int DMA_AllocDescriptor(IMPDMABufferInfo *out, int size, const char *tag)
{
    (void)tag;
    assert(size <= 4096 && nalloc < 8);
    memset(out, 0, sizeof(*out));
    out->virt_addr = (uint32_t)(uintptr_t)rmem[nalloc];
    out->phys_addr = 0x06000000U + 0x1000U * (uint32_t)nalloc;
    out->size = (uint32_t)size;
    nalloc++;
    return 0;
}

int DMA_FreePhys(uint32_t phys)
{
    assert(phys >= 0x06000000U);
    nfree++;
    return 0;
}

int DMA_RmemFlushCache(void *address, uint32_t length, int direction)
{
    (void)address;
    (void)length;
    assert(direction == 1);
    nflush++;
    return 0;
}

int IMP_FrameSource_GetChnAttr(int chn, IMPFSChnAttr *attr)
{
    if (chn > 1)
        return -1;
    memset(attr, 0, sizeof(*attr));
    attr->picWidth = chn ? 640 : 1920;
    attr->picHeight = chn ? 360 : 1080;
    return 0;
}

/* ---- helpers ---- */
static uint8_t pic[64 * 16 * 4];


/* raptor-hal hal_isp_osd_set_region_attr (T41 branch) */
static void fill(IMPIspOsdAttrAsm *a, int handle, int x, int y)
{
    memset(a, 0, sizeof(*a));
    a->type = ISP_OSD_REG_PIC;
    a->stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888;
    a->stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_BGRA;
    a->stsinglepicAttr.chnOSDAttr.osd_pixel_alpha_disable =
        IMPISP_TUNING_OPS_MODE_DISABLE;
    a->stsinglepicAttr.pic.pinum = (uint8_t)handle;
    a->stsinglepicAttr.pic.osd_enable = 1;
    a->stsinglepicAttr.pic.osd_left = (uint16_t)(x & ~1);
    a->stsinglepicAttr.pic.osd_top = (uint16_t)(y & ~1);
    a->stsinglepicAttr.pic.osd_width = 64;
    a->stsinglepicAttr.pic.osd_height = 16;
    a->stsinglepicAttr.pic.osd_image = (char *)pic;
    a->stsinglepicAttr.pic.osd_stride = 64 * 4;
}

/* one region = OSD attr then OSD block, sensor 0, set */
static void expect_region(int i, int pinum, int en, int x, int y,
                          uint32_t phys)
{
    IMPISPOSDAttr a;
    IMPISPOSDBlockAttr b;

    memcpy(&a, sent[i].payload, sizeof(a));
    memcpy(&b, sent[i + 1].payload, sizeof(b));

    assert(sent[i].vinum == 0 && sent[i].direction == 0 &&
           sent[i].control == 0x080000a1);
    assert(a.osd_type == IMP_ISP_PIC_ARGB_8888 &&
           a.osd_argb_type == IMP_ISP_ARGB_TYPE_BGRA &&
           a.osd_pixel_alpha_disable == IMPISP_TUNING_OPS_MODE_DISABLE);
    assert(sent[i + 1].vinum == 0 && sent[i + 1].direction == 0 &&
           sent[i + 1].control == 0x080000a2);
    assert(b.pinum == pinum && b.osd_enable == en);
    assert(b.osd_left == x && b.osd_top == y);
    assert(b.osd_width == 64 && b.osd_height == 16);
    assert((uintptr_t)b.osd_image == phys && b.osd_stride == 256);
}

int main(void)
{
    IMPIspOsdAttrAsm a, g;
    IMPISPOSDAttr oattr;
    IMPISPOSDBlockAttr oblock;
    int i, h0, h1;

    rmem = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(rmem != MAP_FAILED);
    for (i = 0; i < (int)sizeof(pic); i++)
        pic[i] = (uint8_t)(i * 7);

    /* raptor: SetOsdPoolSize, CreateOsdRgn(0, NULL) */
    assert(IMP_ISP_Tuning_SetOsdPoolSize(0) == -1);
    assert(IMP_ISP_Tuning_SetOsdPoolSize(64 * 1024) == 0);
    assert(IMP_ISP_Tuning_CreateOsdRgn(2, NULL) == -1);      /* chn < 2 */
    h0 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    assert(h0 == 0);

    /* SetOsdRgnAttr: frame bounds of the channel, copy into rmem, nothing
     * goes to the driver, the stored enable is cleared until Show */
    fill(&a, h0, 1900, 0);
    assert(IMP_ISP_Tuning_SetOsdRgnAttr(0, h0, &a) == -1);
    fill(&a, h0, 16, 32);
    assert(IMP_ISP_Tuning_SetOsdRgnAttr(0, h0, &a) == 0);
    assert(nsent == 0 && nalloc == 1 && nflush == 1);
    assert(memcmp(rmem[0], pic, sizeof(pic)) == 0);
    assert(IMP_ISP_Tuning_GetOsdRgnAttr(0, h0, &g) == 0);
    assert(g.type == ISP_OSD_REG_PIC && g.stsinglepicAttr.pic.osd_enable == 0);
    assert(g.stsinglepicAttr.pic.pinum == h0);
    assert(g.stsinglepicAttr.pic.osd_image == (char *)rmem[0]);

    /* ShowOsdRgn(1): SetOSDAttr + SetOSDBlock with the physical address */
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, 5, 1) == -1);        /* not created */
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1) == 0);
    assert(nsent == 2);
    expect_region(0, h0, 1, 16, 32, 0x06000000U);
    assert(IMP_ISP_Tuning_GetOsdRgnAttr(0, h0, &g) == 0);
    assert(g.stsinglepicAttr.pic.osd_enable == 1);
    assert(g.stsinglepicAttr.pic.osd_image == (char *)rmem[0]); /* virtual */

    /* update the picture while shown (raptor timestamp): copy only, the
     * next Show sends it again (the vendor behaviour) */
    pic[0] ^= 0xff;
    nsent = 0;
    assert(IMP_ISP_Tuning_SetOsdRgnAttr(0, h0, &a) == 0 && nsent == 0);
    assert(rmem[0][0] == pic[0] && nalloc == 1);
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1) == 0 && nsent == 2);

    /* second region; one overlapping an earlier shown one is switched off */
    h1 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    assert(h1 == 1);
    fill(&a, h1, 40, 40);                                    /* crosses h0 */
    assert(IMP_ISP_Tuning_SetOsdRgnAttr(0, h1, &a) == 0);
    nsent = 0;
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h1, 1) == 0);
    assert(nsent == 4);
    expect_region(0, h0, 1, 16, 32, 0x06000000U);
    expect_region(2, h1, 0, 40, 40, 0x06001000U);
    fill(&a, h1, 1000, 500);
    assert(IMP_ISP_Tuning_SetOsdRgnAttr(0, h1, &a) == 0);
    nsent = 0;
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h1, 1) == 0);
    expect_region(2, h1, 1, 1000, 500, 0x06001000U);

    /* hide: the block goes out with enable 0 */
    nsent = 0;
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h0, 0) == 0);
    expect_region(0, h0, 0, 16, 32, 0x06000000U);
    expect_region(2, h1, 1, 1000, 500, 0x06001000U);

    /* driver refuses (e.g. outside the main stream): logged, Show still 0 */
    ioctl_fail = 1;
    assert(IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1) == 0);
    ioctl_fail = 0;

    /* destroy shown region: switched off at the driver, picture freed */
    nsent = 0;
    nfree = 0;
    assert(IMP_ISP_Tuning_DestroyOsdRgn(0, h1) == 0);
    assert(nfree == 1);
    for (i = 0; i + 1 < nsent; i += 2)
        if (sent[i + 1].payload[0] == h1)
            break;
    assert(i + 1 < nsent && sent[i + 1].payload[1] == 0);
    assert(IMP_ISP_Tuning_GetOsdRgnAttr(0, h1, &g) == 0 && g.type == 0);
    IMP_OSD_Exit_ISP();
    assert(nfree == 2);

    /* Tuning_Set/GetOSDAttr/Block: vendor return codes */
    memset(&oattr, 0, sizeof(oattr));
    memset(&oblock, 0, sizeof(oblock));
    assert(IMP_ISP_Tuning_SetOSDAttr(IMPVI_MAIN, NULL) == -4087);
    assert(IMP_ISP_Tuning_SetOSDBlock((IMPVI_NUM)2, &oblock) == -4084);
    tuning_on = 0;
    assert(IMP_ISP_Tuning_GetOSDAttr(IMPVI_MAIN, &oattr) == -4091);
    tuning_on = 1;
    ioctl_fail = 1;
    assert(IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, &oblock) == -4090);
    ioctl_fail = 0;
    nsent = 0;
    oblock.pinum = 3;
    assert(IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, &oblock) == 0);
    assert(nsent == 1 && sent[0].direction == 1 &&
           sent[0].control == 0x080000a2 && sent[0].payload[0] == 3);
    assert(IMP_ISP_Tuning_GetOSDAttr(IMPVI_MAIN, &oattr) == 0);
    assert(sent[1].direction == 1 && sent[1].control == 0x080000a1);

    printf("t41 isp osd: ok\n");
    return 0;
}
