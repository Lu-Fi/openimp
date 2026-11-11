/*
 * OpenIMP T32 FrameSource on the 2.x framechan ABI (#290, planned; not a
 * release gate), plus the System bind table of the T32 build.
 *
 * Kernel side: open-tx-isp driver/t32 /dev/framechan0-2 (requests in
 * openimp_t32_ioctl.h, FS base 130).  The flow follows what vendor libimp
 * 2.2.0 issues in QEMU -M ingenic-t32 (reference
 * tests/t32/vendor-2.2.0-ioctl-seq-fs.txt, payloads recorded with
 * tools/t32/qemu-ioctl-trace.sh); no vendor code copied.
 *
 *   CreateChn     check and keep the attributes, open /dev/framechanN
 *   EnableChn     GET/SET_FRAME_FORMAT (an int, 1: there is no format
 *                 struct on T32), REQBUFS {nrVBs, 1, 2 = user pointer},
 *                 nrVBs NV12 buffers from the reserved arena, QBUF each
 *                 (96 B: Y pa, length, UV pa, stride, h << 16 | w: the
 *                 driver takes the channel geometry from these words only),
 *                 FRAME_STREAMON, start the frame thread
 *   frame thread  LISTEN_BUF (blocks, answers the ready count), DQBUF,
 *                 frame-rate drop, deliver (bound sinks, depth copy for
 *                 GetFrame), QBUF the buffer back
 *   DisableChn    FRAME_STREAMOFF, join the thread, REQBUFS 0, free buffers
 *   DestroyChn    close the node
 *
 * IMP_FrameSource_GetFrame works on copies, as the vendor library does on
 * T32 (its frame thread requeues every buffer right after DQBUF): with
 * SetFrameDepth(n) each delivered frame is copied into one of n arena
 * buffers (the oldest waiting copy is replaced when none is free) and
 * GetFrame hands out the oldest waiting copy (2 s wait).  SetFrameDepth
 * needs an enabled channel (vendor: error before EnableChn); DisableChn
 * frees the copies that are not held, a later EnableChn allocates the
 * depth again.  Bound modules get
 * the capture buffer itself through a sink (openimp_t32_fs.h); a sink that
 * keeps a frame delays its requeue until ReleaseSinkFrame.
 *
 * IVDC direct mode (System_Init's GET_DIRECT != 0) is not supported: the
 * PRJ007 driver answers 0, and EnableChn refuses a direct-mode ISP.
 * Not here (later T32 issues): crop / mirror (no request seen on this ABI
 * yet), rotation, joint/sub-stream channels, SnapFrame.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "core/thread_name.h"
#include "t32/openimp_t32_ioctl.h"
#include "t32/openimp_t32_isp.h"
#include "t32/openimp_t32_fs.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_fs.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-FS"

#define T32_FS_CHANNELS   3     /* /dev/framechan0-2 (MSCA channels) */
#define T32_FS_MAX_BUFS   8     /* driver limit per channel */
#define T32_FS_MAX_DEPTH  8
#define T32_FS_MAX_BINDS  16
#define T32_FS_MAX_SINKS  32            /* encoder, IVS and 12 OSD groups */
#define T32_FS_GET_WAIT_MS 2000

#define T32_BUF_TYPE_CAPTURE 1
#define T32_BUF_MEMORY_USERPTR 2
#define T32_FORMAT_NV12 1       /* GET/SET_FRAME_FORMAT word */

#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((uint32_t)(a) - 1))

struct t32_fs_buf {
    IMPDMABufferInfo dma;
    T32IocBuffer q;             /* the words this buffer is queued with */
    IMPFrameInfo frame;         /* what a sink sees */
    int holds;                  /* sinks that kept it */
    int out;                    /* dequeued, not queued back yet */
};

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_USER };

struct t32_fs_slot {
    IMPDMABufferInfo dma;
    IMPFrameInfo frame;
    int state;
    uint32_t seq;
};

struct t32_fs_chn {
    int created, enabled;
    IMPFSChnAttr attr;
    int fd;
    /* geometry of the enabled channel */
    uint32_t width, height, stride, length, alloc;
    int nbufs;
    struct t32_fs_buf buf[T32_FS_MAX_BUFS];
    pthread_t thread;
    volatile int run;
    /* frame-rate drop */
    uint32_t fps_word;
    uint64_t fps_acc;
    uint32_t seq;
    /* GetFrame depth copies */
    int depth;
    struct t32_fs_slot slot[T32_FS_MAX_DEPTH];
    pthread_cond_t ready;
    uint32_t frames, dropped, copied, sink_frames;
};

/* src: a FrameSource channel, or an OSD group that passes the frames on
 * (FS -> OSD -> encoder, #297) */
struct t32_fs_bind {
    int used;
    IMPCell src;
    IMPCell dst;
};

struct t32_fs_sink {
    int used;
    int dev, grp;
    OpenIMP_T32_FrameSink fn;
    void *ctx;
};

static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t32_fs_chn fs_chn[T32_FS_CHANNELS];
static struct t32_fs_bind fs_bind[T32_FS_MAX_BINDS];
static struct t32_fs_sink fs_sink[T32_FS_MAX_SINKS];
static pthread_once_t fs_once = PTHREAD_ONCE_INIT;

static void fs_init_once(void)
{
    pthread_condattr_t ca;
    int i;

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    for (i = 0; i < T32_FS_CHANNELS; i++) {
        fs_chn[i].fd = -1;
        pthread_cond_init(&fs_chn[i].ready, &ca);
    }
    pthread_condattr_destroy(&ca);
}

static struct t32_fs_chn *fs_get(int chn)
{
    pthread_once(&fs_once, fs_init_once);
    if (chn < 0 || chn >= T32_FS_CHANNELS)
        return NULL;
    return &fs_chn[chn];
}

static int fs_ioctl(int fd, uint32_t req, void *arg, const char *name)
{
    int rc = ioctl(fd, req, arg);

    if (rc < 0)
        IMP_LOG_ERR(TAG, "%s (0x%08x) failed: %s", name, req, strerror(errno));
    return rc;
}

#define FS_IOCTL(fd, req, arg) fs_ioctl((fd), OI_T32_IOC_##req, (arg), #req)

static uint64_t fs_now_us(void)
{
    return IMP_System_GetTimeStamp();
}

/* ---- buffers ------------------------------------------------------------ */
static void fs_free_dma(IMPDMABufferInfo *d)
{
    if (d->phys_addr)
        DMA_FreePhys(d->phys_addr);
    memset(d, 0, sizeof(*d));
}

static int fs_alloc(IMPDMABufferInfo *d, uint32_t size, const char *tag)
{
    memset(d, 0, sizeof(*d));
    if (DMA_AllocDescriptor(d, (int)size, tag) < 0 || !d->phys_addr || !d->virt_addr) {
        IMP_LOG_ERR(TAG, "%s: cannot allocate %u bytes", tag, size);
        memset(d, 0, sizeof(*d));
        return -1;
    }
    return 0;
}

static void fs_fill_frame(const struct t32_fs_chn *c, int chn, int index,
                          const IMPDMABufferInfo *d, IMPFrameInfo *f)
{
    memset(f, 0, sizeof(*f));
    f->index = index;
    f->pool_idx = chn;
    f->width = c->width;
    f->height = c->height;
    f->pixfmt = PIX_FMT_NV12;
    f->size = c->length;
    f->phyAddr = d->phys_addr;
    f->virAddr = d->virt_addr;
}

static void fs_fill_qbuf(const struct t32_fs_chn *c, struct t32_fs_buf *b, int index)
{
    T32IocBuffer *q = &b->q;

    memset(q, 0, sizeof(*q));
    q->index = (uint32_t)index;
    q->type = T32_BUF_TYPE_CAPTURE;
    q->memory = T32_BUF_MEMORY_USERPTR;
    q->m = b->dma.phys_addr;
    q->length = c->length;
    q->uv_addr = b->dma.phys_addr + c->stride * ALIGN_UP(c->height, 16);
    q->stride = c->stride;
    q->state = 1;
    q->geom = c->height << 16 | c->width;
    q->reserved = (uint32_t)(uintptr_t)b;
}

static int fs_qbuf(struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    T32IocBuffer q = b->q;

    if (FS_IOCTL(c->fd, QBUF, &q) < 0)
        return -1;
    b->out = 0;
    return 0;
}

/* depth copies: allocate up to c->depth (geometry known), free the rest */
static void fs_slots_sync_locked(struct t32_fs_chn *c)
{
    int i;

    for (i = 0; i < T32_FS_MAX_DEPTH; i++) {
        struct t32_fs_slot *s = &c->slot[i];
        int want = c->enabled && i < c->depth;

        if (want && !s->dma.phys_addr && c->alloc) {
            if (fs_alloc(&s->dma, c->alloc, "fs-depth") == 0)
                s->state = SLOT_FREE;
        } else if (!want && s->dma.phys_addr && s->state != SLOT_USER &&
                   s->state != SLOT_FILLING) {
            fs_free_dma(&s->dma);
            s->state = SLOT_FREE;
        }
    }
}

/* ---- frame thread ------------------------------------------------------- */
/* frame-rate drop: 1 = deliver (channel rate below the sensor rate) */
static int fs_rate_pass(struct t32_fs_chn *c, uint32_t fps_word)
{
    uint64_t in_num = fps_word >> 16, in_den = fps_word & 0xffffu;
    uint64_t out_num = (uint32_t)c->attr.outFrmRateNum, out_den = (uint32_t)c->attr.outFrmRateDen;
    uint64_t step, full;

    if (!in_num || !in_den || !out_num || !out_den || out_num * in_den >= in_num * out_den)
        return 1;
    step = out_num * in_den;    /* out / in, scaled by in_num * out_den */
    full = in_num * out_den;
    if (fps_word != c->fps_word) {
        c->fps_word = fps_word;
        c->fps_acc = full;      /* the first frame passes */
    } else {
        c->fps_acc += step;
    }
    if (c->fps_acc >= full) {
        c->fps_acc -= full;
        return 1;
    }
    return 0;
}

static struct t32_fs_slot *fs_pick_slot_locked(struct t32_fs_chn *c)
{
    struct t32_fs_slot *best = NULL;
    int i;

    for (i = 0; i < c->depth && i < T32_FS_MAX_DEPTH; i++)
        if (c->slot[i].dma.phys_addr && c->slot[i].state == SLOT_FREE)
            return &c->slot[i];
    /* none free: replace the oldest waiting copy */
    for (i = 0; i < c->depth && i < T32_FS_MAX_DEPTH; i++) {
        struct t32_fs_slot *s = &c->slot[i];

        if (s->dma.phys_addr && s->state == SLOT_READY &&
            (!best || (int32_t)(s->seq - best->seq) < 0))
            best = s;
    }
    return best;
}

static void fs_deliver(int chn, struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    struct { OpenIMP_T32_FrameSink fn; void *ctx; } sinks[T32_FS_MAX_BINDS];
    IMPCell from[T32_FS_MAX_BINDS + 1];
    struct t32_fs_slot *slot;
    int i, j, k, nsinks = 0, nfrom = 1, done = 0;

    /* every cell this channel reaches, breadth first: the FrameSource
     * channel's own receivers, then those behind an OSD group (which draws
     * into the buffer before it passes it on, as the vendor OSD module) */
    from[0].deviceID = DEV_ID_FS;
    from[0].groupID = chn;
    pthread_mutex_lock(&fs_lock);
    for (k = 0; k < nfrom; k++)
        for (i = 0; i < T32_FS_MAX_BINDS; i++) {
            if (!fs_bind[i].used || fs_bind[i].src.deviceID != from[k].deviceID ||
                fs_bind[i].src.groupID != from[k].groupID)
                continue;
            for (j = 0; j < T32_FS_MAX_SINKS && nsinks < T32_FS_MAX_BINDS; j++)
                if (fs_sink[j].used && fs_sink[j].dev == (int)fs_bind[i].dst.deviceID &&
                    fs_sink[j].grp == (int)fs_bind[i].dst.groupID) {
                    sinks[nsinks].fn = fs_sink[j].fn;
                    sinks[nsinks].ctx = fs_sink[j].ctx;
                    nsinks++;
                    break;
                }
            if (fs_bind[i].dst.deviceID == DEV_ID_OSD && nfrom <= T32_FS_MAX_BINDS)
                from[nfrom++] = fs_bind[i].dst;
        }
    slot = fs_pick_slot_locked(c);
    if (slot)
        slot->state = SLOT_FILLING;
    pthread_mutex_unlock(&fs_lock);

    if (slot) {
        /* NV12 as captured: Y, then UV after ALIGN16(height) rows */
        memcpy((void *)(uintptr_t)slot->dma.virt_addr, (void *)(uintptr_t)b->dma.virt_addr,
               c->alloc);
        pthread_mutex_lock(&fs_lock);
        fs_fill_frame(c, chn, (int)(slot - c->slot), &slot->dma, &slot->frame);
        slot->frame.timeStamp = b->frame.timeStamp;
        slot->seq = c->seq;
        slot->state = SLOT_READY;
        c->copied++;
        if (!c->enabled || (int)(slot - c->slot) >= c->depth)
            fs_slots_sync_locked(c);
        pthread_cond_broadcast(&c->ready);
        pthread_mutex_unlock(&fs_lock);
    }
    if (!nsinks)
        return;
    /* every sink holds the buffer while it runs; one that keeps it gives it
     * back with ReleaseSinkFrame (possibly from another thread at once) */
    pthread_mutex_lock(&fs_lock);
    b->holds += nsinks;
    c->sink_frames++;
    pthread_mutex_unlock(&fs_lock);
    for (i = 0; i < nsinks; i++)
        if (sinks[i].fn(sinks[i].ctx, chn, &b->frame) != 1)
            done++;
    pthread_mutex_lock(&fs_lock);
    b->holds -= done;
    pthread_mutex_unlock(&fs_lock);
}

static void *fs_thread(void *arg)
{
    int chn = (int)(intptr_t)arg;
    struct t32_fs_chn *c = &fs_chn[chn];
    /* LISTEN_BUF's word goes in as -1 first and then as the last answer
     * (the driver only writes it), as the vendor library passes it */
    int32_t count = -1;

    openimp_thread_setnamef("oimp-fs-%d", chn);
    while (c->run) {
        T32IocBuffer q;
        struct t32_fs_buf *b;
        int requeue;

        if (ioctl(c->fd, OI_T32_IOC_LISTEN_BUF, &count) < 0) {
            if (errno != EINTR && c->run) {
                IMP_LOG_ERR(TAG, "framechan%d LISTEN_BUF: %s", chn, strerror(errno));
                usleep(10000);
            }
            continue;
        }
        if (!c->run)
            break;
        if (count <= 0)
            continue;               /* listen timeout without a frame */
        memset(&q, 0, sizeof(q));
        q.type = T32_BUF_TYPE_CAPTURE;
        q.memory = T32_BUF_MEMORY_USERPTR;
        if (ioctl(c->fd, OI_T32_IOC_DQBUF, &q) < 0)
            continue;               /* -EAGAIN: taken by the stream-off */
        if (q.index >= (uint32_t)c->nbufs || q.m != c->buf[q.index].dma.phys_addr) {
            IMP_LOG_ERR(TAG, "framechan%d DQBUF: unknown buffer %u pa 0x%08x", chn, q.index, q.m);
            continue;
        }
        b = &c->buf[q.index];
        b->out = 1;
        b->frame.timeStamp = (int64_t)fs_now_us();
        DMA_RmemFlushCache((void *)(uintptr_t)b->dma.virt_addr, c->alloc, 2);
        pthread_mutex_lock(&fs_lock);
        c->frames++;
        c->seq = q.sequence;
        requeue = !fs_rate_pass(c, q.fps);
        if (requeue)
            c->dropped++;
        pthread_mutex_unlock(&fs_lock);
        if (!requeue)
            fs_deliver(chn, c, b);
        pthread_mutex_lock(&fs_lock);
        if (c->run && b->out && !b->holds)
            fs_qbuf(c, b);
        pthread_mutex_unlock(&fs_lock);
    }
    return NULL;
}

/* ---- channel API ---------------------------------------------------------- */
static int fs_attr_ok(int chn, const IMPFSChnAttr *a)
{
    if (!a)
        return 0;
    if (a->pixFmt != PIX_FMT_NV12) {
        IMP_LOG_ERR(TAG, "chn %d: pixel format %d not supported (NV12 only)", chn, (int)a->pixFmt);
        return 0;
    }
    if (a->picWidth <= 0 || a->picHeight <= 0 || (a->picWidth & 1) || (a->picHeight & 1) ||
        a->picWidth > 0xffff || a->picHeight > 0xffff) {
        IMP_LOG_ERR(TAG, "chn %d: picture %dx%d invalid", chn, a->picWidth, a->picHeight);
        return 0;
    }
    if (a->scaler.enable &&
        (a->scaler.outwidth != a->picWidth || a->scaler.outheight != a->picHeight)) {
        IMP_LOG_ERR(TAG, "chn %d: scaler %dx%d differs from the picture %dx%d", chn,
                    a->scaler.outwidth, a->scaler.outheight, a->picWidth, a->picHeight);
        return 0;
    }
    if (a->nrVBs < 1 || a->nrVBs > T32_FS_MAX_BUFS) {
        IMP_LOG_ERR(TAG, "chn %d: nrVBs %d (1..%d)", chn, a->nrVBs, T32_FS_MAX_BUFS);
        return 0;
    }
    if (a->crop.enable || a->fcrop.enable || a->mirr_enable)
        IMP_LOG_WARN(TAG, "chn %d: crop/mirror not supported on T32 yet, ignored", chn);
    return 1;
}

int IMP_FrameSource_CreateChn(int chnNum, IMPFSChnAttr *chn_attr)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    char path[24];
    int fd;

    if (!c || !fs_attr_ok(chnNum, chn_attr))
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->created) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "chn %d already created", chnNum);
        return -1;
    }
    snprintf(path, sizeof(path), "/dev/framechan%d", chnNum);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "open %s: %s", path, strerror(errno));
        return -1;
    }
    c->fd = fd;
    c->attr = *chn_attr;
    c->created = 1;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_DestroyChn(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int i;

    if (!c)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (!c->created || c->enabled) {
        pthread_mutex_unlock(&fs_lock);
        if (c->enabled)
            IMP_LOG_ERR(TAG, "chn %d still enabled", chnNum);
        return c->enabled ? -1 : 0;
    }
    for (i = 0; i < T32_FS_MAX_DEPTH; i++) {
        fs_free_dma(&c->slot[i].dma);
        c->slot[i].state = SLOT_FREE;
    }
    close(c->fd);
    c->fd = -1;
    c->created = 0;
    c->depth = 0;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_SetChnAttr(int chnNum, IMPFSChnAttr *chn_attr)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int rc = -1;

    if (!c || !fs_attr_ok(chnNum, chn_attr))
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->created && !c->enabled) {
        c->attr = *chn_attr;
        rc = 0;
    }
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "SetChnAttr chn %d: %s", chnNum, c->enabled ? "enabled" : "not created");
    return rc;
}

int IMP_FrameSource_GetChnAttr(int chnNum, IMPFSChnAttr *chn_attr)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int rc = -1;

    if (!c || !chn_attr)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->created) {
        *chn_attr = c->attr;
        rc = 0;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

static void fs_release_bufs_locked(struct t32_fs_chn *c)
{
    int i;

    for (i = 0; i < T32_FS_MAX_BUFS; i++) {
        fs_free_dma(&c->buf[i].dma);
        c->buf[i].holds = 0;
        c->buf[i].out = 0;
    }
    c->nbufs = 0;
}

int IMP_FrameSource_EnableChn(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    T32IocReqBufs rq;
    int32_t fmt, on = T32_BUF_TYPE_CAPTURE;
    uint32_t sw, sh;
    int i;

    if (!c)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (!c->created) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "EnableChn chn %d: not created", chnNum);
        return -1;
    }
    if (c->enabled) {
        pthread_mutex_unlock(&fs_lock);
        return 0;
    }
    if (OpenIMP_T32_IvdcDirectMode()) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "EnableChn chn %d: IVDC direct mode not supported", chnNum);
        return -1;
    }
    if (OpenIMP_T32_SensorSize(&sw, &sh) == 0 &&
        ((uint32_t)c->attr.picWidth > sw || (uint32_t)c->attr.picHeight > sh)) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "EnableChn chn %d: %dx%d above the sensor %ux%u (no upscaling)",
                    chnNum, c->attr.picWidth, c->attr.picHeight, sw, sh);
        return -1;
    }
    c->width = (uint32_t)c->attr.picWidth;
    c->height = (uint32_t)c->attr.picHeight;
    c->stride = ALIGN_UP(c->width, 16);
    c->length = c->width * c->height * 3 / 2;
    c->alloc = c->stride * ALIGN_UP(c->height, 16) * 3 / 2;

    fmt = T32_FORMAT_NV12;
    if (FS_IOCTL(c->fd, GET_FRAME_FORMAT, &fmt) < 0)
        goto fail;
    fmt = T32_FORMAT_NV12;
    if (FS_IOCTL(c->fd, SET_FRAME_FORMAT, &fmt) < 0)
        goto fail;
    memset(&rq, 0, sizeof(rq));
    rq.count = (uint32_t)c->attr.nrVBs;
    rq.type = T32_BUF_TYPE_CAPTURE;
    rq.memory = T32_BUF_MEMORY_USERPTR;
    if (FS_IOCTL(c->fd, REQBUFS, &rq) < 0)
        goto fail;
    if (rq.count < 1 || rq.count > T32_FS_MAX_BUFS) {
        IMP_LOG_ERR(TAG, "chn %d: driver granted %u buffers", chnNum, rq.count);
        goto fail_reqbufs;
    }
    c->nbufs = (int)rq.count;
    for (i = 0; i < c->nbufs; i++) {
        struct t32_fs_buf *b = &c->buf[i];

        if (fs_alloc(&b->dma, c->alloc, "fs-chn") < 0)
            goto fail_bufs;
        fs_fill_frame(c, chnNum, i, &b->dma, &b->frame);
        fs_fill_qbuf(c, b, i);
        b->holds = 0;
        if (fs_qbuf(c, b) < 0)
            goto fail_bufs;
    }
    if (FS_IOCTL(c->fd, FRAME_STREAMON, &on) < 0)
        goto fail_bufs;
    c->enabled = 1;
    c->fps_word = 0;
    c->frames = c->dropped = c->copied = c->sink_frames = 0;
    fs_slots_sync_locked(c);
    c->run = 1;
    if (pthread_create(&c->thread, NULL, fs_thread, (void *)(intptr_t)chnNum) != 0) {
        IMP_LOG_ERR(TAG, "chn %d: frame thread: %s", chnNum, strerror(errno));
        c->run = 0;
        c->enabled = 0;
        on = T32_BUF_TYPE_CAPTURE;
        FS_IOCTL(c->fd, FRAME_STREAMOFF, &on);
        fs_slots_sync_locked(c);
        goto fail_bufs;
    }
    pthread_mutex_unlock(&fs_lock);
    DMA_RmemStreamStarted();
    return 0;

fail_bufs:
    fs_release_bufs_locked(c);
fail_reqbufs:
    memset(&rq, 0, sizeof(rq));
    rq.type = T32_BUF_TYPE_CAPTURE;
    rq.memory = T32_BUF_MEMORY_USERPTR;
    FS_IOCTL(c->fd, REQBUFS, &rq);
fail:
    pthread_mutex_unlock(&fs_lock);
    return -1;
}

int IMP_FrameSource_DisableChn(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    T32IocReqBufs rq;
    int32_t off = T32_BUF_TYPE_CAPTURE;
    int rc;

    if (!c)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (!c->enabled) {
        pthread_mutex_unlock(&fs_lock);
        return 0;
    }
    c->run = 0;
    /* stream-off wakes the listener (the driver ends LISTEN_BUF when the
     * channel stops) */
    rc = FS_IOCTL(c->fd, FRAME_STREAMOFF, &off);
    pthread_cond_broadcast(&c->ready);
    pthread_mutex_unlock(&fs_lock);
    pthread_join(c->thread, NULL);

    pthread_mutex_lock(&fs_lock);
    memset(&rq, 0, sizeof(rq));
    rq.type = T32_BUF_TYPE_CAPTURE;
    rq.memory = T32_BUF_MEMORY_USERPTR;
    if (FS_IOCTL(c->fd, REQBUFS, &rq) < 0)
        rc = -1;
    fs_release_bufs_locked(c);
    c->enabled = 0;
    fs_slots_sync_locked(c);
    IMP_LOG_INFO(TAG, "chn %d off: %u frames, %u dropped (rate), %u copies, %u to sinks",
                 chnNum, c->frames, c->dropped, c->copied, c->sink_frames);
    pthread_mutex_unlock(&fs_lock);
    return rc < 0 ? -1 : 0;
}

int IMP_FrameSource_SetFrameDepth(int chnNum, int depth)
{
    struct t32_fs_chn *c = fs_get(chnNum);

    if (!c || depth < 0 || depth > T32_FS_MAX_DEPTH)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (!c->enabled) {
        /* as the vendor library (0x80010040 before EnableChn, QEMU fs3 run) */
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "SetFrameDepth chn %d: channel not enabled", chnNum);
        return -1;
    }
    c->depth = depth;
    fs_slots_sync_locked(c);
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_GetFrameDepth(int chnNum, int *depth)
{
    struct t32_fs_chn *c = fs_get(chnNum);

    if (!c || !depth)
        return -1;
    pthread_mutex_lock(&fs_lock);
    *depth = c->depth;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_GetFrame(int chnNum, void **frame)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    struct timespec until;
    int rc = 0;

    if (!c || !frame)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += T32_FS_GET_WAIT_MS / 1000;
    until.tv_nsec += (long)(T32_FS_GET_WAIT_MS % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&fs_lock);
    if (!c->enabled || c->depth <= 0) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "GetFrame chn %d: %s", chnNum, c->enabled ? "frame depth 0" : "not enabled");
        return -1;
    }
    for (;;) {
        struct t32_fs_slot *best = NULL;
        int i;

        for (i = 0; i < T32_FS_MAX_DEPTH; i++) {
            struct t32_fs_slot *s = &c->slot[i];

            if (s->state == SLOT_READY && (!best || (int32_t)(s->seq - best->seq) < 0))
                best = s;
        }
        if (best) {
            best->state = SLOT_USER;
            *frame = &best->frame;
            break;
        }
        if (!c->enabled || rc == ETIMEDOUT) {
            pthread_mutex_unlock(&fs_lock);
            IMP_LOG_ERR(TAG, "GetFrame chn %d: %s", chnNum, c->enabled ? "timeout" : "disabled");
            return -1;
        }
        rc = pthread_cond_timedwait(&c->ready, &fs_lock, &until);
    }
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_ReleaseFrame(int chnNum, void *frame)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int i, rc = -1;

    if (!c || !frame)
        return -1;
    pthread_mutex_lock(&fs_lock);
    for (i = 0; i < T32_FS_MAX_DEPTH; i++) {
        struct t32_fs_slot *s = &c->slot[i];

        if (frame == &s->frame && s->state == SLOT_USER) {
            s->state = SLOT_FREE;
            rc = 0;
            break;
        }
    }
    if (rc == 0)
        fs_slots_sync_locked(c);    /* depth lowered meanwhile: drop it */
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "ReleaseFrame chn %d: %p is not a frame of GetFrame", chnNum, frame);
    return rc;
}

/* ---- sinks (bound modules) ------------------------------------------------ */
int OpenIMP_FrameSource_RegisterSink(int devID, int grpID, OpenIMP_T32_FrameSink sink, void *ctx)
{
    int i, freei = -1, rc = -1;

    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    for (i = 0; i < T32_FS_MAX_SINKS; i++) {
        if (fs_sink[i].used && fs_sink[i].dev == devID && fs_sink[i].grp == grpID) {
            if (sink) {
                fs_sink[i].fn = sink;
                fs_sink[i].ctx = ctx;
            } else {
                memset(&fs_sink[i], 0, sizeof(fs_sink[i]));
            }
            rc = 0;
            break;
        }
        if (!fs_sink[i].used && freei < 0)
            freei = i;
    }
    if (rc && sink && freei >= 0) {
        fs_sink[freei].used = 1;
        fs_sink[freei].dev = devID;
        fs_sink[freei].grp = grpID;
        fs_sink[freei].fn = sink;
        fs_sink[freei].ctx = ctx;
        rc = 0;
    } else if (rc && !sink) {
        rc = 0;                 /* nothing to remove */
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int OpenIMP_FrameSource_ReleaseSinkFrame(int fs_chn_num, IMPFrameInfo *frame)
{
    struct t32_fs_chn *c = fs_get(fs_chn_num);
    int rc = -1;

    if (!c || !frame)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->enabled && frame->index >= 0 && frame->index < c->nbufs &&
        frame == &c->buf[frame->index].frame) {
        struct t32_fs_buf *b = &c->buf[frame->index];

        if (b->holds > 0 && --b->holds == 0 && b->out && c->run)
            fs_qbuf(c, b);
        rc = 0;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

/* ---- System bind ---------------------------------------------------------- */
#define T32_OSD_GROUPS 12               /* vendor 2.2.0 osd.c: group < 12 */

static int same_cell(const IMPCell *a, const IMPCell *b)
{
    return a->deviceID == b->deviceID && a->groupID == b->groupID;
}

int IMP_System_Bind(IMPCell *srcCell, IMPCell *dstCell)
{
    int i, freei = -1, sink = 0;

    if (!srcCell || !dstCell)
        return -1;
    if (srcCell->outputID != 0 || srcCell->groupID < 0 ||
        !((srcCell->deviceID == DEV_ID_FS && srcCell->groupID < T32_FS_CHANNELS) ||
          (srcCell->deviceID == DEV_ID_OSD && srcCell->groupID < T32_OSD_GROUPS)) ||
        same_cell(srcCell, dstCell)) {
        IMP_LOG_ERR(TAG, "Bind: source %d-%d-%d not supported (FrameSource channels, OSD groups)",
                    srcCell->deviceID, srcCell->groupID, srcCell->outputID);
        return -1;
    }
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    for (i = 0; i < T32_FS_MAX_SINKS; i++)
        if (fs_sink[i].used && fs_sink[i].dev == (int)dstCell->deviceID &&
            fs_sink[i].grp == (int)dstCell->groupID)
            sink = 1;
    for (i = 0; i < T32_FS_MAX_BINDS; i++) {
        if (fs_bind[i].used && fs_bind[i].dst.deviceID == dstCell->deviceID &&
            fs_bind[i].dst.groupID == dstCell->groupID) {
            pthread_mutex_unlock(&fs_lock);
            IMP_LOG_ERR(TAG, "Bind: %d-%d already bound", dstCell->deviceID, dstCell->groupID);
            return -1;
        }
        if (!fs_bind[i].used && freei < 0)
            freei = i;
    }
    if (!sink || freei < 0) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "Bind: %s %d-%d", sink ? "too many bindings for" : "no receiving group",
                    dstCell->deviceID, dstCell->groupID);
        return -1;
    }
    fs_bind[freei].used = 1;
    fs_bind[freei].src = *srcCell;
    fs_bind[freei].dst = *dstCell;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_System_UnBind(IMPCell *srcCell, IMPCell *dstCell)
{
    int i, rc = -1;

    if (!srcCell || !dstCell)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    for (i = 0; i < T32_FS_MAX_BINDS; i++)
        if (fs_bind[i].used && same_cell(&fs_bind[i].src, srcCell) &&
            same_cell(&fs_bind[i].dst, dstCell)) {
            memset(&fs_bind[i], 0, sizeof(fs_bind[i]));
            rc = 0;
        }
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "UnBind: %d-%d -> %d-%d not bound", srcCell->deviceID,
                    srcCell->groupID, dstCell->deviceID, dstCell->groupID);
    return rc;
}

/* Vendor 2.x sys_interface.c: the cell bound to dstCell (any output of the
 * source); 0, or -1 when nothing is bound to it.  The IVS framework follows
 * this to the FrameSource channel that feeds a group. */
int IMP_System_GetBindbyDest(IMPCell *dstCell, IMPCell *srcCell)
{
    int i, rc = -1;

    if (!dstCell || !srcCell)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    for (i = 0; i < T32_FS_MAX_BINDS; i++)
        if (fs_bind[i].used && fs_bind[i].dst.deviceID == dstCell->deviceID &&
            fs_bind[i].dst.groupID == dstCell->groupID) {
            *srcCell = fs_bind[i].src;
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

/* Y stride of a frame this FrameSource handed out (capture buffer or a
 * GetFrame copy), found by its physical address; 0 when unknown.  Used by
 * the IVS framework (src/t31/openimp_t31_ivs.c) for widths that are not a
 * multiple of 16. */
uint32_t OpenIMP_FS_FramePitch(uint32_t physical)
{
    uint32_t pitch = 0;
    int chn, i;

    if (!physical)
        return 0;
    pthread_mutex_lock(&fs_lock);
    for (chn = 0; chn < T32_FS_CHANNELS && !pitch; chn++) {
        struct t32_fs_chn *c = &fs_chn[chn];

        if (!c->enabled)
            continue;
        for (i = 0; i < c->nbufs && !pitch; i++)
            if (c->buf[i].dma.phys_addr == physical)
                pitch = c->stride;
        for (i = 0; i < T32_FS_MAX_DEPTH && !pitch; i++)
            if (c->slot[i].dma.phys_addr == physical)
                pitch = c->stride;
    }
    pthread_mutex_unlock(&fs_lock);
    return pitch;
}
