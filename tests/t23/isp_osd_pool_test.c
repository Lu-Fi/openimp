/* T23 ISP OSD picture pool (src/t23/openimp_t23_isp_osd.c): the OEM reserves
 * IMP_OSD_SetPoolSize_ISP bytes of rmem at IMP_OSD_Init_ISP and cuts the
 * pictures out of it in 256-byte units.  The module runs on stubs for the
 * ISP layer and for the rmem allocator (a private arena). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/t23/openimp_t23_isp_osd.c"

/* ---- stubs --------------------------------------------------------- */
static unsigned char rmem[64 * 1024] __attribute__((aligned(4096)));
static size_t rmem_top;
static int rmem_live;
static int notified;

int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    (void)tag;
    size = (size + 4095) & ~4095;
    if (rmem_top + (size_t)size > sizeof(rmem))
        return -1;
    memset(info, 0, sizeof(*info));
    info->virt_addr = (uint32_t)(uintptr_t)(rmem + rmem_top);
    info->phys_addr = 0x1000000u + (uint32_t)rmem_top;
    info->size = (uint32_t)size;
    rmem_top += (size_t)size;
    rmem_live++;
    return 0;
}

int DMA_FreePhys(uint32_t phys)
{
    assert(phys >= 0x1000000u);
    rmem_live--;
    return 0;
}

int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    (void)virt; (void)size; (void)dir;
    return 0;
}

int IMP_FrameSource_GetChnAttr(int chn, IMPFSChnAttr *attr)
{
    (void)chn; (void)attr;
    return -1;                      /* channel not created: no size check */
}

#define STUB1(name, type) int name(type *a) { (void)a; notified++; return 0; }
#define STUB2(name, type) int name(int v, type *a) { (void)v; (void)a; notified++; return 0; }
STUB1(IMP_ISP_Tuning_SetOSDAttr, T23ISPOSDAttr)
STUB1(IMP_ISP_Tuning_SetOSDBlock, T23ISPOSDBlockAttr)
STUB1(IMP_ISP_Tuning_SetOSDAttr_Sec, T23ISPOSDAttr)
STUB1(IMP_ISP_Tuning_SetOSDBlock_Sec, T23ISPOSDBlockAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_SetOSDAttr, T23ISPOSDAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_SetOSDBlock, T23ISPOSDBlockAttr)
STUB1(IMP_ISP_Tuning_SetDrawBlock, T23ISPDrawBlockAttr)
STUB1(IMP_ISP_Tuning_SetMaskBlock, T23ISPMaskBlockAttr)
STUB1(IMP_ISP_Tuning_GetOSDAttr, T23ISPOSDAttr)
STUB1(IMP_ISP_Tuning_GetMaskBlock, T23ISPMaskBlockAttr)
STUB1(IMP_ISP_Tuning_SetDrawBlock_Sec, T23ISPDrawBlockAttr)
STUB1(IMP_ISP_Tuning_SetMaskBlock_Sec, T23ISPMaskBlockAttr)
STUB1(IMP_ISP_Tuning_GetOSDAttr_Sec, T23ISPOSDAttr)
STUB1(IMP_ISP_Tuning_GetMaskBlock_Sec, T23ISPMaskBlockAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_GetOSDAttr, T23ISPOSDAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_GetMaskBlock, T23ISPMaskBlockAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_SetDrawBlock, T23ISPDrawBlockAttr)
STUB2(IMP_ISP_MultiCamera_Tuning_SetMaskBlock, T23ISPMaskBlockAttr)

static void pic(IMPIspOsdAttrAsm *a, int stride, int h)
{
    memset(a, 0, sizeof(*a));
    a->type = ISP_OSD_REG_PIC;
    a->stsinglepicAttr.pic.osd_stride = (uint16_t)stride;
    a->stsinglepicAttr.pic.osd_height = (uint16_t)h;
    a->stsinglepicAttr.pic.osd_width = (uint16_t)stride;
}

int main(void)
{
    IMPIspOsdAttrAsm a;
    int h0, h1, h2;

    /* size <= 0 is refused; the value is only stored */
    assert(IMP_OSD_SetPoolSize_ISP(0) == -1);
    assert(IMP_OSD_SetPoolSize_ISP(-4) == -1);
    assert(isp_osd_pool_size == 0);

    /* a pool below 0x100 bytes: Init fails and gives the rmem back */
    assert(IMP_OSD_SetPoolSize_ISP(100) == 0);
    assert(IMP_OSD_Init_ISP() == -1);
    assert(rmem_live == 0 && !isp_osd_ready && !isp_osd_pool);

    /* 2 KiB pool: 8 blocks of 256 bytes */
    assert(IMP_OSD_SetPoolSize_ISP(2048) == 0);
    assert(IMP_OSD_Init_ISP() == 0 && rmem_live == 1);
    assert(isp_osd_pool && isp_osd_pool->size == 2048);
    h0 = IMP_OSD_CreateRgn_ISP(0, NULL);
    h1 = IMP_OSD_CreateRgn_ISP(0, NULL);
    h2 = IMP_OSD_CreateRgn_ISP(0, NULL);
    assert(h0 == 0 && h1 == 1 && h2 == 2);

    pic(&a, 16, 16);                    /* 256 bytes: block 0 */
    assert(IMP_OSD_SetRgnAttr_PicISP(0, h0, &a) == 0);
    pic(&a, 16, 17);                    /* 272 bytes -> 512: blocks 1-2 */
    assert(IMP_OSD_SetRgnAttr_PicISP(0, h1, &a) == 0);
    assert(isp_osd[0][h0].phys == isp_osd_pool_phys);
    assert(isp_osd[0][h1].phys == isp_osd_pool_phys + 256);
    assert(isp_osd[0][h0].in_pool && isp_osd[0][h1].in_pool);
    assert(rmem_live == 1);             /* pictures take nothing more */

    pic(&a, 32, 40);                    /* 1280 bytes: the last 5 blocks */
    assert(IMP_OSD_SetRgnAttr_PicISP(0, h2, &a) == 0);
    assert(isp_osd[0][h2].phys == isp_osd_pool_phys + 768);
    pic(&a, 16, 1);                     /* pool is full */
    assert(IMP_OSD_CreateRgn_ISP(0, NULL) == 3);
    assert(IMP_OSD_SetRgnAttr_PicISP(0, 3, &a) == -1);
    assert(!isp_osd[0][3].size);

    /* destroying a region returns its block, merged with its neighbours */
    assert(IMP_OSD_DestroyRgn_ISP(0, h1) == 0);
    pic(&a, 16, 16);
    assert(IMP_OSD_SetRgnAttr_PicISP(0, 3, &a) == 0);
    assert(isp_osd[0][3].phys == isp_osd_pool_phys + 256);

    IMP_OSD_Exit_ISP();
    assert(rmem_live == 0 && !isp_osd_pool);

    /* never set: the pictures keep their own rmem (no pool) */
    isp_osd_pool_size = 0;
    assert(IMP_OSD_Init_ISP() == 0 && !isp_osd_pool && rmem_live == 0);
    assert(IMP_OSD_CreateRgn_ISP(0, NULL) == 0);
    pic(&a, 16, 16);
    assert(IMP_OSD_SetRgnAttr_PicISP(0, 0, &a) == 0 && rmem_live == 1);
    assert(!isp_osd[0][0].in_pool);
    IMP_OSD_Exit_ISP();
    assert(rmem_live == 0);

    puts("T23 ISP OSD pool test passed");
    return 0;
}
