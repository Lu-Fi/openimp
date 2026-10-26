/*
 * T40/T41 externally injected FrameSource channels (vendor T40 1.3.1 /
 * T41 1.2.6 IMP_FrameSource_ExternInject_*, DequeueBuffer, QueueBuffer) and
 * the EmuFrameSource stubs.
 *
 * Vendor (T41 1.2.5 libimp.a framesource_tseries.c.o 0xb3dc..0xc000):
 *  - ExternInject_CreateChn(chn, attr): chn < 7, attr != NULL, picWidth a
 *    multiple of 16 ("should be align to 16"), channel 0 without scaler,
 *    channel not created yet; creates the FrameSource group so the channel
 *    binds like a sensor channel (IMP_System_Bind {DEV_ID_FS, chn, 0}).
 *  - EnableChn: a VBM pool of nrVBs NV12 frames and two FIFOs (free and
 *    filled); DisableChn flushes the module tree and frees them; DestroyChn
 *    removes the group (channel must be disabled).
 *  - DequeueBuffer(chn, &frame): a free frame of the pool (fifo_get; none
 *    free: error), the caller writes NV12 into frame->virAddr;
 *    QueueBuffer(chn, frame): the filled frame goes to the bound modules.
 *
 * OpenIMP: the channel is a record here; IMP_FrameSource_GetFrame /
 * ReleaseFrame of openimp_p1.c (the encoder's frame pull, IVS, OSD) hand
 * out the queued frames in order and give them back to the free list, so a
 * bound encoder reads them exactly like sensor frames.  Frames are rmem,
 * NV12, pitch = width, chroma after ALIGN16(height) luma lines (the layout
 * of the sensor frames and of the AVPU source); frame->size is that size.
 * A queued frame without a time stamp gets IMP_System_GetTimeStamp().
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>

#include "dma_alloc.h"
#include "imp_log_int.h"

#define FSI_CHANNELS 7                  /* vendor: chnNum < 7 */
#define FSI_MAX_BUFFERS 8
#define FSI_DEFAULT_BUFFERS 3

extern int64_t IMP_System_GetTimeStamp(void);
/* openimp_p1.c: a sensor channel with this number exists */
extern int OpenIMP_P1_ChannelCreated(int channel) __attribute__((weak));

enum { FSI_FREE, FSI_DEQUEUED, FSI_READY, FSI_OUT };

struct fsi_buffer {
    IMPFrameInfo frame;
    uint32_t physical;
    void *virt;
    int state;
    uint64_t order;                     /* QueueBuffer sequence */
};

struct fsi_channel {
    int created;
    int enabled;
    IMPFSChnAttr attr;
    uint32_t count;
    uint32_t frame_size;
    uint64_t queued;                    /* QueueBuffer sequence counter */
    struct fsi_buffer buffers[FSI_MAX_BUFFERS];
};

static struct fsi_channel fsi[FSI_CHANNELS];
static pthread_mutex_t fsi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fsi_ready = PTHREAD_COND_INITIALIZER;

static int fsi_valid(int chn, const char *function)
{
    if (chn >= 0 && chn < FSI_CHANNELS)
        return 1;
    IMP_LOG_LIMITED(LOG_ERR, "Framesource", "%s: chnNum %d out of 0..%d",
                    function, chn, FSI_CHANNELS - 1);
    return 0;
}

int IMP_FrameSource_ExternInject_CreateChn(int chnNum, IMPFSChnAttr *chnAttr)
{
    struct fsi_channel *c;

    if (!fsi_valid(chnNum, __func__) || !chnAttr)
        return -1;
    if (chnAttr->picWidth <= 0 || chnAttr->picHeight <= 0 ||
        (chnAttr->picWidth & 0xf)) {
        IMP_LOG_ERR("Framesource", "chnNum=%d, chnAttr->picWidth should be "
                    "align to 16\n", chnNum);
        return -1;
    }
    if (chnAttr->pixFmt != PIX_FMT_NV12) {
        IMP_LOG_ERR("Framesource", "%s: chn %d: only NV12 can be injected\n",
                    __func__, chnNum);
        return -1;
    }
    if (chnNum == 0 && chnAttr->scaler.enable) {
        IMP_LOG_ERR("Framesource", "%s: chn 0 has no scaler\n", __func__);
        return -1;
    }
    if (OpenIMP_P1_ChannelCreated && OpenIMP_P1_ChannelCreated(chnNum)) {
        IMP_LOG_ERR("Framesource", "%s: chn %d is a sensor channel\n",
                    __func__, chnNum);
        return -1;
    }
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (c->created) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->attr = *chnAttr;
    c->created = 1;
    pthread_mutex_unlock(&fsi_lock);
    return 0;
}

static void fsi_free_buffers(struct fsi_channel *c)
{
    uint32_t i;

    for (i = 0; i < c->count; i++)
        if (c->buffers[i].physical)
            DMA_FreePhys(c->buffers[i].physical);
    memset(c->buffers, 0, sizeof(c->buffers));
    c->count = 0;
}

int IMP_FrameSource_ExternInject_EnableChn(int chnNum)
{
    struct fsi_channel *c;
    uint32_t width, height, lines, count, i;

    if (!fsi_valid(chnNum, __func__))
        return -1;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (!c->created) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    if (c->enabled) {
        pthread_mutex_unlock(&fsi_lock);
        return 0;
    }
    width = (uint32_t)c->attr.picWidth;
    height = (uint32_t)c->attr.picHeight;
    lines = (height + 15u) & ~15u;
    count = c->attr.nrVBs > 0 ? (uint32_t)c->attr.nrVBs : FSI_DEFAULT_BUFFERS;
    if (count > FSI_MAX_BUFFERS)
        count = FSI_MAX_BUFFERS;
    c->frame_size = width * lines * 3u / 2u;
    for (i = 0; i < count; i++) {
        IMPDMABufferInfo info;
        struct fsi_buffer *b = &c->buffers[i];

        memset(&info, 0, sizeof(info));
        if (DMA_AllocDescriptor(&info, (int)c->frame_size, "inject") != 0 ||
            !info.phys_addr) {
            IMP_LOG_ERR("Framesource", "%s: chn %d: no rmem for frame %u "
                        "(%u bytes)\n", __func__, chnNum, i, c->frame_size);
            c->count = i;
            fsi_free_buffers(c);
            pthread_mutex_unlock(&fsi_lock);
            return -1;
        }
        b->physical = info.phys_addr;
        b->virt = (void *)(uintptr_t)info.virt_addr;
        b->state = FSI_FREE;
        memset(&b->frame, 0, sizeof(b->frame));
        b->frame.index = (int)i;
        b->frame.pool_idx = -1;
        b->frame.width = width;
        b->frame.height = height;
        b->frame.pixfmt = PIX_FMT_NV12;
        b->frame.size = c->frame_size;
        b->frame.phyAddr = info.phys_addr;
        b->frame.virAddr = info.virt_addr;
#if defined(PLATFORM_T41)
        b->frame.direct_phyAddr = info.phys_addr;
#endif
        c->count = i + 1;
    }
    c->queued = 0;
    c->enabled = 1;
    pthread_mutex_unlock(&fsi_lock);
    return 0;
}

int IMP_FrameSource_ExternInject_DisableChn(int chnNum)
{
    struct fsi_channel *c;
    uint32_t i;

    if (!fsi_valid(chnNum, __func__))
        return -1;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (!c->created) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    if (c->enabled) {
        /* consumers (the encoder) give their frames back first */
        for (i = 0; i < c->count; i++)
            if (c->buffers[i].state == FSI_OUT) {
                pthread_mutex_unlock(&fsi_lock);
                IMP_LOG_ERR("Framesource", "%s: chn %d: frame %u still held "
                            "by a consumer\n", __func__, chnNum, i);
                return -1;
            }
        c->enabled = 0;
        fsi_free_buffers(c);
        pthread_cond_broadcast(&fsi_ready);
    }
    pthread_mutex_unlock(&fsi_lock);
    return 0;
}

int IMP_FrameSource_ExternInject_DestroyChn(int chnNum)
{
    struct fsi_channel *c;

    if (!fsi_valid(chnNum, __func__))
        return -1;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (!c->created || c->enabled) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    memset(c, 0, sizeof(*c));
    pthread_mutex_unlock(&fsi_lock);
    return 0;
}

int IMP_FrameSource_DequeueBuffer(int chnNum, IMPFrameInfo **frame)
{
    struct fsi_channel *c;
    uint32_t i;

    if (!fsi_valid(chnNum, __func__) || !frame)
        return -1;
    *frame = NULL;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (!c->enabled) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    for (i = 0; i < c->count; i++)
        if (c->buffers[i].state == FSI_FREE) {
            c->buffers[i].state = FSI_DEQUEUED;
            c->buffers[i].frame.timeStamp = 0;
            *frame = &c->buffers[i].frame;
            pthread_mutex_unlock(&fsi_lock);
            return 0;
        }
    pthread_mutex_unlock(&fsi_lock);
    IMP_LOG_LIMITED(LOG_ERR, "Framesource", "%s: chn %d: no free frame",
                    __func__, chnNum);
    return -1;
}

int IMP_FrameSource_QueueBuffer(int chnNum, const IMPFrameInfo *frame)
{
    struct fsi_channel *c;
    struct fsi_buffer *b;

    if (!fsi_valid(chnNum, __func__) || !frame)
        return -1;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[chnNum];
    if (!c->enabled || frame->index < 0 ||
        (uint32_t)frame->index >= c->count ||
        frame != &c->buffers[frame->index].frame ||
        c->buffers[frame->index].state != FSI_DEQUEUED) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    b = &c->buffers[frame->index];
    pthread_mutex_unlock(&fsi_lock);
    /* the CPU wrote the picture, the encoder reads it by DMA: write it back
     * while the frame is still the caller's (DEQUEUED, no consumer sees it) */
    (void)DMA_RmemFlushCache(b->virt, c->frame_size, 1);
    pthread_mutex_lock(&fsi_lock);
    if (!c->enabled || b->state != FSI_DEQUEUED) {
        pthread_mutex_unlock(&fsi_lock);
        return -1;
    }
    if (!b->frame.timeStamp)
        b->frame.timeStamp = IMP_System_GetTimeStamp();
    b->state = FSI_READY;
    b->order = ++c->queued;
    pthread_cond_broadcast(&fsi_ready);
    pthread_mutex_unlock(&fsi_lock);
    return 0;
}

/* ---- openimp_p1.c hooks: consumers of an injected channel -------------- */

int OpenIMP_FSInject_Active(int channel);
int OpenIMP_FSInject_Active(int channel)
{
    int on;

    if (channel < 0 || channel >= FSI_CHANNELS)
        return 0;
    pthread_mutex_lock(&fsi_lock);
    on = fsi[channel].created;
    pthread_mutex_unlock(&fsi_lock);
    return on;
}

/* the oldest queued frame; wait_ms 0: one probe */
int OpenIMP_FSInject_GetFrame(int channel, IMPFrameInfo **frame,
                              uint32_t wait_ms);
int OpenIMP_FSInject_GetFrame(int channel, IMPFrameInfo **frame,
                              uint32_t wait_ms)
{
    struct fsi_channel *c;
    struct timespec until;
    int result = -1;

    if (channel < 0 || channel >= FSI_CHANNELS || !frame)
        return -1;
    *frame = NULL;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += wait_ms / 1000u;
    until.tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[channel];
    for (;;) {
        struct fsi_buffer *oldest = NULL;
        uint32_t i;

        if (!c->enabled)
            break;
        for (i = 0; i < c->count; i++)
            if (c->buffers[i].state == FSI_READY &&
                (!oldest || c->buffers[i].order < oldest->order))
                oldest = &c->buffers[i];
        if (oldest) {
            oldest->state = FSI_OUT;
            *frame = &oldest->frame;
            result = 0;
            break;
        }
        if (!wait_ms ||
            pthread_cond_timedwait(&fsi_ready, &fsi_lock, &until) != 0)
            break;
    }
    pthread_mutex_unlock(&fsi_lock);
    return result;
}

/* 1: the frame was an injected one and is free again, 0: not ours */
int OpenIMP_FSInject_ReleaseFrame(int channel, IMPFrameInfo *frame);
int OpenIMP_FSInject_ReleaseFrame(int channel, IMPFrameInfo *frame)
{
    struct fsi_channel *c;
    int ours = 0;

    if (channel < 0 || channel >= FSI_CHANNELS || !frame)
        return 0;
    pthread_mutex_lock(&fsi_lock);
    c = &fsi[channel];
    if (c->enabled && frame->index >= 0 &&
        (uint32_t)frame->index < c->count &&
        frame == &c->buffers[frame->index].frame &&
        c->buffers[frame->index].state == FSI_OUT) {
        c->buffers[frame->index].state = FSI_FREE;
        ours = 1;
    } else if (c->created) {
        ours = -1;                      /* an injected channel, a stray frame */
    }
    pthread_mutex_unlock(&fsi_lock);
    return ours;
}

/* ---- EmuFrameSource ----------------------------------------------------- */
/*
 * IMP_EmuFrameSource_* (imp_emu_framesource.h, "encoder emulator", in the
 * export union of the vendor T40/T41 builds, not in the T41 1.2.6
 * libimp.so): a channel fed from a file by a vendor test harness.  OpenIMP
 * has no emulator; IMP_FrameSource_ExternInject_* is the supported way to
 * feed own frames.  Every call fails with -1 and errno ENOTSUP.
 */
static int fsi_emu_unsupported(const char *function)
{
    IMP_LOG_LIMITED(LOG_ERR, "Framesource", "%s: not supported by OpenIMP "
                    "(use IMP_FrameSource_ExternInject_*)", function);
    errno = ENOTSUP;
    return -1;
}

int IMP_EmuFrameSource_GetChnAttr(uint32_t chn_num, IMPFSChnAttr *chn_attr)
{
    (void)chn_num;
    (void)chn_attr;
    return fsi_emu_unsupported(__func__);
}

int IMP_EmuFrameSource_EnableChn(uint32_t chn_num)
{
    (void)chn_num;
    return fsi_emu_unsupported(__func__);
}

int IMP_EmuFrameSource_DisableChn(uint32_t chn_num)
{
    (void)chn_num;
    return fsi_emu_unsupported(__func__);
}

int IMP_EmuFrameSource_CreateChn(uint32_t chn_num, IMPFSChnAttr *chn_attr)
{
    (void)chn_num;
    (void)chn_attr;
    return fsi_emu_unsupported(__func__);
}

int IMP_EmuFrameSource_DestroyChn(uint32_t chn_num)
{
    (void)chn_num;
    return fsi_emu_unsupported(__func__);
}
