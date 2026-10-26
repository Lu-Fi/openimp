/* T40/T41 IMP_FrameSource_ExternInject_* / DequeueBuffer / QueueBuffer and
 * the consumer hooks openimp_p1.c calls (OpenIMP_FSInject_*), plus the
 * EmuFrameSource ENOTSUP stubs, on a fake rmem allocator.  Build with
 * -DPLATFORM_T40 or -DPLATFORM_T41. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include "dma_alloc.h"

int OpenIMP_FSInject_Active(int channel);
int OpenIMP_FSInject_GetFrame(int channel, IMPFrameInfo **frame,
                              uint32_t wait_ms);
int OpenIMP_FSInject_ReleaseFrame(int channel, IMPFrameInfo *frame);

static int64_t now_ts = 1000;
int64_t IMP_System_GetTimeStamp(void) { return now_ts; }
static int sensor_chn = -1;
int OpenIMP_P1_ChannelCreated(int channel) { return channel == sensor_chn; }

static int allocs, frees, flushes;
/* static arena: the frame records hold 32-bit addresses (-no-pie) */
static uint8_t arena[8][640 * 368 * 3 / 2 + 4096] __attribute__((aligned(4096)));
static int arena_used[8];
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    int i;

    (void)tag;
    if ((size_t)size > sizeof(arena[0]))
        return -1;
    for (i = 0; i < 8 && arena_used[i]; i++)
        ;
    if (i == 8)
        return -1;
    arena_used[i] = 1;
    memset(info, 0, sizeof(*info));
    info->virt_addr = (uint32_t)(uintptr_t)arena[i];
    info->phys_addr = info->virt_addr;
    allocs++;
    return 0;
}
int DMA_FreePhys(uint32_t phys)
{
    int i;

    for (i = 0; i < 8; i++)
        if ((uint32_t)(uintptr_t)arena[i] == phys)
            arena_used[i] = 0;
    frees++;
    return 0;
}
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    (void)virt; (void)size;
    if (dir == 1)
        flushes++;
    return 0;
}

int main(void)
{
    IMPFSChnAttr attr;
    IMPFrameInfo *f[4], *g;
    int i;

    memset(&attr, 0, sizeof(attr));
    attr.picWidth = 640;
    attr.picHeight = 360;
    attr.pixFmt = PIX_FMT_NV12;
    attr.outFrmRateNum = 25;
    attr.outFrmRateDen = 1;
    attr.nrVBs = 3;
    /* vendor argument checks */
    assert(IMP_FrameSource_ExternInject_CreateChn(7, &attr) == -1);
    assert(IMP_FrameSource_ExternInject_CreateChn(-1, &attr) == -1);
    assert(IMP_FrameSource_ExternInject_CreateChn(2, NULL) == -1);
    attr.picWidth = 632;
    assert(IMP_FrameSource_ExternInject_CreateChn(2, &attr) == -1);
    attr.picWidth = 640;
    sensor_chn = 2;
    assert(IMP_FrameSource_ExternInject_CreateChn(2, &attr) == -1);
    sensor_chn = -1;
    assert(!OpenIMP_FSInject_Active(2));
    assert(IMP_FrameSource_ExternInject_CreateChn(2, &attr) == 0);
    assert(IMP_FrameSource_ExternInject_CreateChn(2, &attr) == -1);
    assert(OpenIMP_FSInject_Active(2));
    /* nothing to dequeue before EnableChn */
    assert(IMP_FrameSource_DequeueBuffer(2, &f[0]) == -1);
    assert(IMP_FrameSource_ExternInject_EnableChn(2) == 0 && allocs == 3);
    assert(IMP_FrameSource_ExternInject_EnableChn(2) == 0 && allocs == 3);
    /* three frames, then none */
    for (i = 0; i < 3; i++) {
        assert(IMP_FrameSource_DequeueBuffer(2, &f[i]) == 0 && f[i]);
        assert(f[i]->width == 640 && f[i]->height == 360 &&
               f[i]->pixfmt == PIX_FMT_NV12 && f[i]->phyAddr &&
               f[i]->size == 640 * 368 * 3 / 2 && f[i]->index == i);
    }
    assert(IMP_FrameSource_DequeueBuffer(2, &f[3]) == -1);
    /* consumers see nothing until a frame is queued */
    assert(OpenIMP_FSInject_GetFrame(2, &g, 0) == -1 && !g);
    assert(OpenIMP_FSInject_GetFrame(2, &g, 30) == -1);
    /* queue order is delivery order; a missing time stamp is filled */
    f[2]->timeStamp = 777;
    assert(IMP_FrameSource_QueueBuffer(2, f[2]) == 0);
    now_ts = 2000;
    assert(IMP_FrameSource_QueueBuffer(2, f[0]) == 0 && flushes == 2);
    assert(IMP_FrameSource_QueueBuffer(2, f[0]) == -1);      /* twice */
    assert(OpenIMP_FSInject_GetFrame(2, &g, 0) == 0 && g == f[2] &&
           g->timeStamp == 777);
    assert(OpenIMP_FSInject_GetFrame(2, &g, 0) == 0 && g == f[0] &&
           g->timeStamp == 2000);
    assert(OpenIMP_FSInject_GetFrame(2, &g, 0) == -1);
    /* a consumer still holds f[2]: no disable */
    assert(OpenIMP_FSInject_ReleaseFrame(2, f[0]) == 1);
    assert(OpenIMP_FSInject_ReleaseFrame(2, f[0]) == -1);    /* twice */
    assert(IMP_FrameSource_ExternInject_DisableChn(2) == -1);
    assert(OpenIMP_FSInject_ReleaseFrame(2, f[2]) == 1);
    /* a frame of another channel is not ours */
    assert(OpenIMP_FSInject_ReleaseFrame(3, f[2]) == 0);
    /* freed frames come back */
    assert(IMP_FrameSource_DequeueBuffer(2, &f[3]) == 0);
    assert(IMP_FrameSource_ExternInject_DestroyChn(2) == -1); /* enabled */
    assert(IMP_FrameSource_ExternInject_DisableChn(2) == 0 && frees == 3);
    assert(IMP_FrameSource_QueueBuffer(2, f[1]) == -1);
    assert(IMP_FrameSource_ExternInject_DestroyChn(2) == 0);
    assert(!OpenIMP_FSInject_Active(2));
    assert(IMP_FrameSource_ExternInject_DestroyChn(2) == -1);
    /* EmuFrameSource: not supported */
    errno = 0;
    assert(IMP_EmuFrameSource_CreateChn(0, &attr) == -1 && errno == ENOTSUP);
    assert(IMP_EmuFrameSource_EnableChn(0) == -1);
    assert(IMP_EmuFrameSource_DisableChn(0) == -1);
    assert(IMP_EmuFrameSource_DestroyChn(0) == -1);
    assert(IMP_EmuFrameSource_GetChnAttr(0, &attr) == -1);
    puts("fs_inject_test: ok");
    return 0;
}
