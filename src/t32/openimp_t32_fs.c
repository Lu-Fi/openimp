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
 *   EnableChn     GET/SET_FRAME_FORMAT (struct frame_image_format, 116 B:
 *                 the vendor ko takes the channel geometry from it, the open
 *                 driver only reads type 1), REQBUFS {nrVBs, 1, 2 = user pointer},
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
 * IVDC direct mode (#301, System_Init's GET_DIRECT = 1): channel 0 goes to
 * the encoder over the IVDC line buffer.  EnableChn then asks MEM_LINE,
 * allocates the DDR overflow ring (width rounded up to 64 x lines x 3/2,
 * lines = half the picture when the driver says 0), hands it over with
 * SET_PADDR and queues the nrVBs buffers with the IVDC source address
 * 0x80000000 instead of frame memory, once: as the vendor library, the frame
 * thread does not queue them back (the driver hands them out again).  Its
 * frames carry no picture (phyAddr 0x80000000, virAddr 0): they reach bound
 * encoder groups only (also through OSD groups, which cannot draw on them),
 * and SetFrameDepth / GetFrame refuse the channel.  Direct modes 2..4
 * (several sensors) are refused.
 * Delay FIFO, GetTimedFrame, SnapFrame, I2D attribute and pools (#445): see
 * the block before OpenIMP_FrameSource_RegisterSink.
 * Not here (later T32 issues): crop / mirror (no request seen on this ABI
 * yet), rotation (the I2D attribute is kept, not applied), joint/sub-stream
 * channels.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "t32/openimp_t32_ivdc.h"
#include "t40/p1_i2d.h"

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
    IMPDMABufferInfo i2d;       /* T32: the I2D output of this buffer (#445) */
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
    int direct;                 /* frames over IVDC (#301), set at EnableChn */
    IMPDMABufferInfo ivdc;      /* the IVDC DDR overflow ring */
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
    /* delay FIFO (#445): Set/GetMaxDelay, Set/GetDelay, Set/GetChnFifoAttr,
     * GetTimedFrame; the newest maxdelay frames, packed NV12 heap copies */
    int maxdelay, delay, fifo_type;
    struct t32_fs_cached *cache;
    int cache_cap, cache_next;
    /* SnapFrame (#445): one request at a time, filled by the frame thread */
    int snap;                   /* 0 idle, 1 wanted, 2 filling, 3 done */
    void *snap_buf;
    IMPFrameInfo *snap_info;
    /* GetFrameEx (F1): waiters, the buffer offered to them, the info block */
    int ex_want;
    struct t32_fs_buf *ex_buf;
    IMPFrameInfo ex_frame;
    int suspended;              /* PM_Suspend (F1): frame thread stopped */
    /* T32 I2D (#445): EnableChn arms it from the channel's i2dattr */
    int i2d_on;
    IMPFSI2DAttr i2d_attr;
    struct p1_i2d_geometry i2d_geom;
    uint32_t i2d_runs, i2d_errors;
};

struct t32_fs_cached {
    void *data;                 /* width x height x 3/2, packed NV12 */
    IMPFrameInfo info;
    int valid;
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

/* FrameSource pools (#445): vendor table of 33 channel -> pool ids, -1 none */
#define T32_FS_POOL_CHNS 33
static int fs_pool[T32_FS_POOL_CHNS];
static int fs_pool_init;

int IMP_MemPool_GetById(int pool_id, void *info_out);

/* a capture buffer: from the channel's pool when SetPool bound one */
static int fs_alloc_chn(int chn, IMPDMABufferInfo *d, uint32_t size)
{
    int pool = -1;

    pthread_once(&fs_once, fs_init_once);
    if (fs_pool_init && chn >= 0 && chn < T32_FS_POOL_CHNS)
        pool = fs_pool[chn];
    if (pool < 0)
        return fs_alloc(d, size, "fs-chn");
    memset(d, 0, sizeof(*d));
    if (DMA_PoolAllocDescriptor(pool, d, (int)size, "fs-chn") < 0 || !d->phys_addr ||
        !d->virt_addr) {
        IMP_LOG_ERR(TAG, "chn %d: pool %d cannot give %u bytes", chn, pool, size);
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
    f->phyAddr = c->direct ? OI_T32_IVDC_SRC_Y : d->phys_addr;
    f->virAddr = c->direct ? 0 : d->virt_addr;
}

static void fs_fill_qbuf(const struct t32_fs_chn *c, struct t32_fs_buf *b, int index)
{
    T32IocBuffer *q = &b->q;

    memset(q, 0, sizeof(*q));
    q->index = (uint32_t)index;
    q->type = T32_BUF_TYPE_CAPTURE;
    q->memory = T32_BUF_MEMORY_USERPTR;
    q->m = c->direct ? OI_T32_IVDC_SRC_Y : b->dma.phys_addr;
    q->length = c->length;
    q->uv_addr = c->direct ? 0 : b->dma.phys_addr + c->stride * ALIGN_UP(c->height, 16);
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
/* bytes of one GetFrame depth copy: the I2D output when the channel turns its frames */
static uint32_t fs_slot_size(const struct t32_fs_chn *c)
{
    return c->i2d_on && c->i2d_geom.dst_size > c->alloc ? c->i2d_geom.dst_size : c->alloc;
}

static void fs_slots_sync_locked(struct t32_fs_chn *c)
{
    int i;

    for (i = 0; i < T32_FS_MAX_DEPTH; i++) {
        struct t32_fs_slot *s = &c->slot[i];
        int want = c->enabled && i < c->depth;

        if (want && !s->dma.phys_addr && c->alloc) {
            if (fs_alloc(&s->dma, fs_slot_size(c), "fs-depth") == 0)
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

/* the capture buffer (Y stride x ALIGN16(h), then UV) as packed NV12 */
static void fs_pack_nv12(const struct t32_fs_chn *c, const struct t32_fs_buf *b, uint8_t *dst)
{
    const uint8_t *y = (const uint8_t *)(uintptr_t)b->dma.virt_addr;
    const uint8_t *uv = y + c->stride * ALIGN_UP(c->height, 16);
    uint32_t r;

    for (r = 0; r < c->height; r++)
        memcpy(dst + r * c->width, y + r * c->stride, c->width);
    dst += c->width * c->height;
    for (r = 0; r < c->height / 2; r++)
        memcpy(dst + r * c->width, uv + r * c->stride, c->width);
}

static void fs_packed_info(const struct t32_fs_chn *c, int chn, const struct t32_fs_buf *b,
                           void *data, IMPFrameInfo *f)
{
    *f = b->frame;
    f->pool_idx = chn;
    f->width = c->width;
    f->height = c->height;
    f->pixfmt = PIX_FMT_NV12;
    f->size = c->width * c->height * 3 / 2;     /* vendor: w x h x 3/2 for NV12 */
    f->phyAddr = 0;
    f->virAddr = (uint32_t)(uintptr_t)data;
}

/* delay FIFO and SnapFrame: called by the frame thread for every passed frame */
static void fs_cache_feed(int chn, struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    struct t32_fs_cached *e = NULL;
    void *snap_buf = NULL;
    IMPFrameInfo *snap_info = NULL;

    pthread_mutex_lock(&fs_lock);
    if (c->cache && c->cache_cap > 0) {
        e = &c->cache[c->cache_next];
        e->valid = 0;           /* being refilled: GetTimedFrame skips it */
        c->cache_next = (c->cache_next + 1) % c->cache_cap;
        if (!e->data)
            e->data = malloc(c->width * c->height * 3 / 2);
        if (!e->data)
            e = NULL;
    }
    if (c->snap == 1) {
        c->snap = 2;
        snap_buf = c->snap_buf;
        snap_info = c->snap_info;
    }
    pthread_mutex_unlock(&fs_lock);
    if (!e && !snap_buf)
        return;
    if (e)
        fs_pack_nv12(c, b, e->data);
    if (snap_buf) {
        fs_pack_nv12(c, b, snap_buf);
        if (snap_info)
            fs_packed_info(c, chn, b, snap_buf, snap_info);
    }
    pthread_mutex_lock(&fs_lock);
    if (e && c->cache) {
        fs_packed_info(c, chn, b, e->data, &e->info);
        e->valid = 1;
    }
    if (snap_buf)
        c->snap = 3;
    pthread_cond_broadcast(&c->ready);
    pthread_mutex_unlock(&fs_lock);
}

static void fs_cache_free_locked(struct t32_fs_chn *c)
{
    int i;

    if (c->cache) {
        for (i = 0; i < c->cache_cap; i++)
            free(c->cache[i].data);
        free(c->cache);
    }
    c->cache = NULL;
    c->cache_cap = c->cache_next = 0;
}

static int fs_collect_sinks_locked(int chn, int direct, OpenIMP_T32_FrameSink *fn, void **ctx,
                                   int max);
static void fs_ex_offer_locked(struct t32_fs_chn *c, struct t32_fs_buf *b);

/* ---- T32 I2D (#445) ------------------------------------------------------
 * Vendor libimp T32 2.2.0 (i2d_enable_ch%d, VBMCreateI2dPool): a channel
 * whose i2dattr enables a rotation / flip / mirror runs every captured NV12
 * frame through /dev/i2d (drivers/video/ingenic_i2d, the T40/T41 ABI:
 * IOCTL_I2D_START with struct i2d_param, the call returns when the block is
 * done) into an I2D buffer of its own and hands that one to GetFrame and
 * the bound modules; 90 / 270 give picHeight x picWidth frames.  Shared
 * helpers: src/t40/p1_i2d.h.  The T33(L) vendor 2.0.2.1 does not apply
 * the attribute (QEMU, #445), so the T33 build keeps it only.  Output line
 * pitch ALIGN16(width) as the T32 capture buffers (the T40 driver needs
 * ALIGN32 for 90 / 270 on silicon: not known for the T32, no camera).
 * The delay FIFO and SnapFrame copy the unturned capture (not modelled). */
static int fs_i2d_fd = -1;

static void fs_i2d_frame(const struct t32_fs_chn *c, const IMPDMABufferInfo *d, IMPFrameInfo *f)
{
    f->width = c->i2d_geom.out_w;
    f->height = c->i2d_geom.out_h;
    f->size = c->i2d_geom.out_w * c->i2d_geom.out_h * 3 / 2;
    f->phyAddr = d->phys_addr;
    f->virAddr = d->virt_addr;
}

/* EnableChn: lock held, capture buffers allocated */
static void fs_i2d_arm_locked(struct t32_fs_chn *c, int chn)
{
#if defined(PLATFORM_T33)
    (void)c;
    (void)chn;
#else
    const IMPFSI2DAttr *a = &c->attr.i2dattr;
    int i;

    c->i2d_on = 0;
    c->i2d_runs = c->i2d_errors = 0;
    if (c->direct || !p1_i2d_active(a->i2d_enable, a->flip_enable, a->mirr_enable,
                                    a->rotate_enable, a->rotate_angle))
        return;
    if (p1_i2d_geometry(c->width, c->height, a->rotate_enable, a->rotate_angle, &c->i2d_geom) < 0) {
        IMP_LOG_ERR(TAG, "chn %d: I2D %ux%u angle %d not possible (8-aligned size, 16-aligned width for 90/270)",
                    chn, c->width, c->height, a->rotate_angle);
        return;
    }
    c->i2d_geom.src_pitch = c->stride;
    c->i2d_geom.dst_pitch = ALIGN_UP(c->i2d_geom.out_w, 16);
    c->i2d_geom.dst_size = c->i2d_geom.dst_pitch * ALIGN_UP(c->i2d_geom.out_h, 16) * 3 / 2;
    if (fs_i2d_fd < 0) {
        fs_i2d_fd = open("/dev/i2d", O_RDWR | O_CLOEXEC);
        if (fs_i2d_fd < 0) {
            IMP_LOG_ERR(TAG, "chn %d: /dev/i2d: %s", chn, strerror(errno));
            return;
        }
    }
    for (i = 0; i < c->nbufs; i++) {
        struct t32_fs_buf *b = &c->buf[i];

        if (fs_alloc(&b->i2d, c->i2d_geom.dst_size, "fs-i2d") < 0) {
            IMP_LOG_ERR(TAG, "chn %d: no memory for the I2D buffers", chn);
            for (i = 0; i < c->nbufs; i++)
                fs_free_dma(&c->buf[i].i2d);
            return;
        }
    }
    c->i2d_attr = *a;
    c->i2d_on = 1;
    for (i = 0; i < c->nbufs; i++)
        fs_i2d_frame(c, &c->buf[i].i2d, &c->buf[i].frame);
    IMP_LOG_INFO(TAG, "chn %d: I2D %ux%u -> %ux%u (rotate %d/%d flip %d mirror %d)", chn, c->width, c->height,
                 c->i2d_geom.out_w, c->i2d_geom.out_h, a->rotate_enable, a->rotate_angle, a->flip_enable,
                 a->mirr_enable);
#endif
}

/* the frame thread, no lock: one buffer through the block */
static void fs_i2d_run(struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    struct p1_i2d_param p;
    const IMPFSI2DAttr *a = &c->i2d_attr;

    p1_i2d_fill(&p, c->width, c->height, a->flip_enable, a->mirr_enable, a->rotate_enable, a->rotate_angle,
                &c->i2d_geom, b->dma.phys_addr, b->i2d.phys_addr);
    /* the T32 driver takes dst_addr_uv as given (the T40 one derives it): 0 would
     * send the chroma to physical address 0; source UV as the driver derives it */
    p.src_addr_uv = b->dma.phys_addr + c->stride * ALIGN_UP(c->height, 16);
    p.dst_addr_uv = b->i2d.phys_addr + c->i2d_geom.dst_pitch * ALIGN_UP(c->i2d_geom.out_h, 16);
    if (ioctl(fs_i2d_fd, P1_I2D_IOC_START, &p) < 0) {
        if (!c->i2d_errors++)
            IMP_LOG_ERR(TAG, "I2D start: %s", strerror(errno));
        return;
    }
    c->i2d_runs++;
    DMA_RmemFlushCache((void *)(uintptr_t)b->i2d.virt_addr, c->i2d_geom.dst_size, 2);
}

static void fs_deliver(int chn, struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    OpenIMP_T32_FrameSink fn[T32_FS_MAX_BINDS];
    void *ctx[T32_FS_MAX_BINDS];
    struct t32_fs_slot *slot;
    int i, nsinks, done = 0;

    /* every cell this channel reaches, breadth first: the FrameSource
     * channel's own receivers, then those behind an OSD group (which draws
     * into the buffer before it passes it on, as the vendor OSD module) */
    pthread_mutex_lock(&fs_lock);
    nsinks = fs_collect_sinks_locked(chn, c->direct, fn, ctx, T32_FS_MAX_BINDS);
    fs_ex_offer_locked(c, b);
    slot = c->direct ? NULL : fs_pick_slot_locked(c);
    if (slot)
        slot->state = SLOT_FILLING;
    pthread_mutex_unlock(&fs_lock);

    if (slot) {
        /* NV12 as captured: Y, then UV after ALIGN16(height) rows */
        memcpy((void *)(uintptr_t)slot->dma.virt_addr,
               (void *)(uintptr_t)(c->i2d_on ? b->i2d.virt_addr : b->dma.virt_addr),
               c->i2d_on ? c->i2d_geom.dst_size : c->alloc);
        pthread_mutex_lock(&fs_lock);
        fs_fill_frame(c, chn, (int)(slot - c->slot), &slot->dma, &slot->frame);
        if (c->i2d_on)
            fs_i2d_frame(c, &slot->dma, &slot->frame);
        slot->frame.timeStamp = b->frame.timeStamp;
        slot->seq = c->seq;
        slot->state = SLOT_READY;
        c->copied++;
        if (!c->enabled || (int)(slot - c->slot) >= c->depth)
            fs_slots_sync_locked(c);
        pthread_cond_broadcast(&c->ready);
        pthread_mutex_unlock(&fs_lock);
    }
    if (!c->direct)
        fs_cache_feed(chn, c, b);
    if (!nsinks)
        return;
    /* every sink holds the buffer while it runs; one that keeps it gives it
     * back with ReleaseSinkFrame (possibly from another thread at once) */
    pthread_mutex_lock(&fs_lock);
    b->holds += nsinks;
    c->sink_frames++;
    pthread_mutex_unlock(&fs_lock);
    for (i = 0; i < nsinks; i++)
        if (fn[i](ctx[i], chn, &b->frame) != 1)
            done++;
    pthread_mutex_lock(&fs_lock);
    b->holds -= done;
    pthread_mutex_unlock(&fs_lock);
}

/* a blocking framechan request the channel's stop may cancel: the vendor
 * tx-isp-t32.ko does not end LISTEN_BUF / DQBUF at the stream-off, and the
 * vendor library cancels its frame thread there too (#301); no lock is held
 * across it */
static int fs_wait_ioctl(int fd, uint32_t req, void *arg)
{
    int st, ty, rc;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &st);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &ty);
    rc = ioctl(fd, req, arg);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &ty);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &st);
    return rc;
}

static void *fs_thread(void *arg)
{
    int chn = (int)(intptr_t)arg;
    struct t32_fs_chn *c = &fs_chn[chn];
    /* LISTEN_BUF's word goes in as -1 first and then as the last answer
     * (the driver only writes it), as the vendor library passes it */
    int32_t count = -1;
    int cst;

    openimp_thread_setnamef("oimp-fs-%d", chn);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cst);
    while (c->run) {
        T32IocBuffer q;
        struct t32_fs_buf *b;
        int requeue;

        if (fs_wait_ioctl(c->fd, OI_T32_IOC_LISTEN_BUF, &count) < 0) {
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
        if (fs_wait_ioctl(c->fd, OI_T32_IOC_DQBUF, &q) < 0)
            continue;               /* -EAGAIN: taken by the stream-off */
        if (q.index >= (uint32_t)c->nbufs ||
            q.m != (c->direct ? OI_T32_IVDC_SRC_Y : c->buf[q.index].dma.phys_addr)) {
            IMP_LOG_ERR(TAG, "framechan%d DQBUF: unknown buffer %u pa 0x%08x", chn, q.index, q.m);
            continue;
        }
        b = &c->buf[q.index];
        b->out = 1;
        b->frame.timeStamp = (int64_t)fs_now_us();
        if (!c->direct)
            DMA_RmemFlushCache((void *)(uintptr_t)b->dma.virt_addr, c->alloc, 2);
        pthread_mutex_lock(&fs_lock);
        c->frames++;
        c->seq = q.sequence;
        requeue = !fs_rate_pass(c, q.fps);
        if (requeue)
            c->dropped++;
        pthread_mutex_unlock(&fs_lock);
        if (!requeue && c->i2d_on)
            fs_i2d_run(c, b);
        if (!requeue)
            fs_deliver(chn, c, b);
        pthread_mutex_lock(&fs_lock);
        if (c->direct)
            b->out = 0;             /* the driver hands it out again by itself */
        else if (c->run && b->out && !b->holds)
            fs_qbuf(c, b);
        pthread_mutex_unlock(&fs_lock);
    }
    return NULL;
}

/* ---- IVDC direct mode (#301) ---------------------------------------------- */
/* MEM_LINE, the DDR overflow ring, SET_PADDR: 0 or -1 */
static int fs_ivdc_ring(struct t32_fs_chn *c)
{
    int32_t lines = 0, pa;
    uint32_t w64 = ALIGN_UP(c->width, 64), size;

    if (OpenIMP_T32_IvdcIoctl(OI_T32_IOC_MEM_LINE, &lines, "MEM_LINE") != 0)
        return -1;
    size = lines > 0 ? w64 * (uint32_t)lines * 3u / 2u : w64 * c->height * 3u / 4u;
    if (fs_alloc(&c->ivdc, size, "ivdc") < 0)
        return -1;
    pa = (int32_t)c->ivdc.phys_addr;
    if (OpenIMP_T32_IvdcIoctl(OI_T32_IOC_SET_PADDR, &pa, "SET_PADDR") != 0) {
        fs_free_dma(&c->ivdc);
        return -1;
    }
    IMP_LOG_INFO(TAG, "chn 0: IVDC direct, %u-byte DDR ring at 0x%08x (mem line %d)", size,
                 c->ivdc.phys_addr, (int)lines);
    return 0;
}

int OpenIMP_T32_FsChnDirect(int chn)
{
    struct t32_fs_chn *c = fs_get(chn);
    int d;

    if (!c)
        return 0;
    pthread_mutex_lock(&fs_lock);
    d = c->enabled && c->direct;
    pthread_mutex_unlock(&fs_lock);
    return d;
}

/* for openimp_t32_fs_enc_ext.c / openimp_t32_ivdc.c (#54 #57): 0 none, 1 created, 2 enabled;
 * the frame depth of the channel (0 = none set) */
int OpenIMP_T32_FsChnState(int chn, int *depth)
{
    struct t32_fs_chn *c = fs_get(chn);
    int st = 0;

    if (depth)
        *depth = 0;
    if (!c)
        return 0;
    pthread_mutex_lock(&fs_lock);
    st = c->enabled ? 2 : c->created ? 1 : 0;
    if (depth)
        *depth = c->depth;
    pthread_mutex_unlock(&fs_lock);
    return st;
}

/* picture size of a created channel (openimp_t32_fs_enc_ext.c joints); -1 when not created */
int OpenIMP_T32_FsChnSize(int chn, int *w, int *h)
{
    struct t32_fs_chn *c = fs_get(chn);
    int rc = -1;

    if (!c)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->created) {
        *w = c->attr.picWidth;
        *h = c->attr.picHeight;
        rc = 0;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

static int inj_state_locked(int chn);
static int inj_get_attr_locked(int chn, IMPFSChnAttr *a);

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
    if (c->created || inj_state_locked(chnNum)) {
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
    c->maxdelay = c->delay = c->fifo_type = 0;
    memset(&c->attr, 0, sizeof(c->attr));
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
    } else if (c->created && !memcmp(&c->attr, chn_attr, sizeof(c->attr))) {
        rc = 0;     /* vendor 2.0.2.1 stores the attribute without a state check; here an unchanged one is accepted on a running channel */
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

    if (!chn_attr)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (inj_get_attr_locked(chnNum, chn_attr) == 0) {     /* ExternInject channel (F1) */
        rc = 0;
    } else if (c && c->created) {
        *chn_attr = c->attr;
        rc = 0;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

static void fs_release_bufs_locked(struct t32_fs_chn *c)
{
    int i;

    c->i2d_on = 0;
    for (i = 0; i < T32_FS_MAX_BUFS; i++) {
        fs_free_dma(&c->buf[i].dma);
        fs_free_dma(&c->buf[i].i2d);
        c->buf[i].holds = 0;
        c->buf[i].out = 0;
    }
    c->nbufs = 0;
}

int IMP_FrameSource_EnableChn(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    T32IocReqBufs rq;
    T32IocFrameFormat fmt;
    int32_t on = T32_BUF_TYPE_CAPTURE;
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
    c->direct = chnNum == 0 && OpenIMP_T32_IvdcDirectMode() != 0;
    if (c->direct && OpenIMP_T32_IvdcDirectMode() != 1) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "EnableChn chn %d: IVDC direct mode %d (several sensors) not supported",
                    chnNum, OpenIMP_T32_IvdcDirectMode());
        c->direct = 0;
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
    if (c->direct && fs_ivdc_ring(c) < 0)
        goto fail;
    if (c->direct)
        c->alloc = 0;           /* no frame memory: frames come over IVDC */

    /* the driver's format, then the channel's geometry in it (as the vendor
     * library: type capture, field 4, colorspace 8, scaler as the attribute,
     * every frame passed: rate bits 0 mask 1; the rate drop is ours) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = T32_FORMAT_NV12;
    if (FS_IOCTL(c->fd, GET_FRAME_FORMAT, &fmt) < 0)
        goto fail;
    fmt.type = T32_FORMAT_NV12;
    fmt.width = c->width;
    fmt.height = c->height;
    if (!fmt.field)
        fmt.field = 4;
    if (!fmt.colorspace)
        fmt.colorspace = 8;
    fmt.crop_enable = 0;
    fmt.scaler_enable = c->attr.scaler.enable != 0;
    fmt.scaler_out_width = c->attr.scaler.enable ? (uint32_t)c->attr.scaler.outwidth : 0;
    fmt.scaler_out_height = c->attr.scaler.enable ? (uint32_t)c->attr.scaler.outheight : 0;
    fmt.rate_bits = 0;
    fmt.rate_mask = 1;
    fmt.fcrop_enable = 0;
    fmt.mirr_enable = 0;
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

        if (!c->direct && fs_alloc_chn(chnNum, &b->dma, c->alloc) < 0)
            goto fail_bufs;
        fs_fill_frame(c, chnNum, i, &b->dma, &b->frame);
        fs_fill_qbuf(c, b, i);
        b->holds = 0;
        if (fs_qbuf(c, b) < 0)
            goto fail_bufs;
    }
    fs_i2d_arm_locked(c, chnNum);
    if (FS_IOCTL(c->fd, FRAME_STREAMON, &on) < 0)
        goto fail_bufs;
    c->enabled = 1;
    c->fps_word = 0;
    if (!c->direct && c->maxdelay > 0) {
        c->cache = calloc((size_t)c->maxdelay, sizeof(*c->cache));
        c->cache_cap = c->cache ? c->maxdelay : 0;
        c->cache_next = 0;
    }
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
    fs_free_dma(&c->ivdc);
    c->direct = 0;
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
    /* stream-off, then REQBUFS 0 as the vendor library: the open driver
     * ends LISTEN_BUF at the stream-off, the vendor tx-isp-t32.ko only when
     * the buffers go (#301); the buffers are freed after the thread is gone */
    rc = FS_IOCTL(c->fd, FRAME_STREAMOFF, &off);
    memset(&rq, 0, sizeof(rq));
    rq.type = T32_BUF_TYPE_CAPTURE;
    rq.memory = T32_BUF_MEMORY_USERPTR;
    if (FS_IOCTL(c->fd, REQBUFS, &rq) < 0)
        rc = -1;
    pthread_cond_broadcast(&c->ready);
    pthread_mutex_unlock(&fs_lock);
    pthread_cancel(c->thread);          /* only takes effect in a blocking request */
    pthread_join(c->thread, NULL);

    pthread_mutex_lock(&fs_lock);
    fs_release_bufs_locked(c);
    fs_free_dma(&c->ivdc);
    fs_cache_free_locked(c);
    c->enabled = 0;
    c->direct = 0;
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
    if (c->direct && depth > 0) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "SetFrameDepth chn %d: IVDC direct channel, its frames carry no picture",
                    chnNum);
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
    if (c->enabled && c->depth <= 0) {
        /* vendor libimp (T41 1.2.5 and T33 2.0.2.1, disassembled): depth 0 returns 0 and leaves *frame alone */
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "GetFrame chn %d: frame depth 0", chnNum);
        return 0;
    }
    if (!c->enabled) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "GetFrame chn %d: not enabled", chnNum);
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
            return c->enabled ? (int)0x80012000 : -1;   /* IMP_ERR_FS_OVERTIME as the vendor */
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
/* ---- delay FIFO, I2D attribute, pools, SnapFrame (#445) --------------------
 * Return codes and checks of vendor libimp T32 2.2.0 / T33 2.0.2.1
 * (disassembly; no vendor code copied):
 *   channels 0..11 (0x80010001 above; this build has /dev/framechan0-2,
 *   the others are never created), state "created" / "enabled";
 *   SetMaxDelay 0..100 on a created, not enabled channel (0x80010040
 *   otherwise, 0x80010002 out of range); SetDelay 0..100 and <= max delay
 *   on a created channel; Get* on a created channel;
 *   SetChnFifoAttr = SetMaxDelay(maxdepth) (its return value) plus
 *   delay = maxdepth and the type; GetChnFifoAttr {max delay, type};
 *   SetI2dAttr / GetI2dAttr store / read the five words of the channel
 *   attribute (no state check; flip with mirror, or flip/mirror with
 *   rotate: 0x80010040; NULL 0x80010010);
 *   SetPool: channel 0..32, the pool must exist (IMP_System_MemPoolRequest,
 *   else 0x80010040), once per channel (0x80010020); GetPool: the id or
 *   0x80010020; the channel's capture buffers come from that pool;
 *   GetTimedFrame: needs a max delay; the newest max-delay frames are kept;
 *   a time stamp at or before the oldest kept frame: 0x80012000; newer than
 *   the newest: 0x80010040 without block, else wait up to 5 s (0x80012000);
 *   otherwise the kept frame nearest to it, copied packed NV12 into
 *   framedata (when given), info with size w x h x 3/2;
 *   SnapFrame: framedata non-NULL (0x80010010), enabled channel, width or
 *   height equal to the channel's (vendor test), the next frame within 3 s
 *   (0x80012000), packed NV12.
 * I2D: SetI2dAttr's rotation / flip is kept, not applied.  T33: as the
 * vendor 2.0.2.1 on the T33L (QEMU: the frame stays unrotated, the I2D
 * block gets no interrupt).  T32: as the vendor 2.2.0, EnableChn arms the
 * /dev/i2d pass (see "T32 I2D" above, #445); SnapFrame takes PIX_FMT_NV12 only (0x80010002 for others, the
 * vendor's other formats are not reverse engineered); the FIFO keeps heap
 * copies instead of holding capture buffers, so bound modules are not
 * delayed by SetDelay (the vendor's FIFO_CACHE_PRIORITY / DATA_PRIORITY
 * output order is not modelled). */
#define FS_ERR_CHN      ((int)0x80010001)
#define FS_ERR_PARAM    ((int)0x80010002)
#define FS_ERR_NULL     ((int)0x80010010)
#define FS_ERR_BUSY     ((int)0x80010020)
#define FS_ERR_STATE    ((int)0x80010040)
#define FS_ERR_TIMEOUT  ((int)0x80012000)
#define FS_VENDOR_CHNS  12
#define FS_DELAY_MAX    100

/* 0 not created, 1 created, 2 enabled; channels 3..11 exist only on the vendor */
static int fs_state_locked(int chn)
{
    if (chn < 0 || chn >= T32_FS_CHANNELS || !fs_chn[chn].created)
        return 0;
    return fs_chn[chn].enabled ? 2 : 1;
}

int IMP_FrameSource_SetMaxDelay(int chnNum, int maxcnt)
{
    int rc = 0;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if ((unsigned)maxcnt > FS_DELAY_MAX)
        return FS_ERR_PARAM;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (fs_state_locked(chnNum) != 1)
        rc = FS_ERR_STATE;
    else
        fs_chn[chnNum].maxdelay = maxcnt;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_GetMaxDelay(int chnNum, int *maxcnt)
{
    int rc = 0;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!maxcnt)
        return FS_ERR_PARAM;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (!fs_state_locked(chnNum))
        rc = FS_ERR_STATE;
    else
        *maxcnt = fs_chn[chnNum].maxdelay;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_SetDelay(int chnNum, int cnt)
{
    int rc = 0;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if ((unsigned)cnt > FS_DELAY_MAX)
        return FS_ERR_PARAM;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (!fs_state_locked(chnNum))
        rc = FS_ERR_STATE;
    else if (cnt > fs_chn[chnNum].maxdelay)
        rc = FS_ERR_PARAM;
    else
        fs_chn[chnNum].delay = cnt;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_GetDelay(int chnNum, int *cnt)
{
    int rc = 0;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!cnt)
        return FS_ERR_PARAM;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (!fs_state_locked(chnNum))
        rc = FS_ERR_STATE;
    else
        *cnt = fs_chn[chnNum].delay;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_SetChnFifoAttr(int chnNum, IMPFSChnFifoAttr *attr)
{
    int rc;

    if (!attr)
        return FS_ERR_PARAM;
    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    rc = fs_state_locked(chnNum) == 1 ? 0 : FS_ERR_STATE;
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        return rc;
    rc = IMP_FrameSource_SetMaxDelay(chnNum, attr->maxdepth);
    pthread_mutex_lock(&fs_lock);
    if (fs_state_locked(chnNum)) {      /* stored as the vendor, whatever SetMaxDelay said */
        fs_chn[chnNum].fifo_type = (int)attr->type;
        fs_chn[chnNum].delay = attr->maxdepth;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_GetChnFifoAttr(int chnNum, IMPFSChnFifoAttr *attr)
{
    int rc = 0;

    if (!attr)
        return FS_ERR_PARAM;
    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (!fs_state_locked(chnNum)) {
        rc = FS_ERR_STATE;
    } else {
        attr->maxdepth = fs_chn[chnNum].maxdelay;
        attr->type = (IMPFSChnFifoType)fs_chn[chnNum].fifo_type;
    }
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

int IMP_FrameSource_SetI2dAttr(int chnNum, IMPFSI2DAttr *attr)
{
    if (!attr)
        return FS_ERR_NULL;
    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if ((attr->flip_enable & attr->mirr_enable) ||
        ((attr->flip_enable | attr->mirr_enable) & attr->rotate_enable))
        return FS_ERR_STATE;
    if (chnNum >= T32_FS_CHANNELS)
        return 0;               /* vendor stores it into its channel table */
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    fs_chn[chnNum].attr.i2dattr = *attr;
    pthread_mutex_unlock(&fs_lock);
#if defined(PLATFORM_T33)
    if (attr->i2d_enable)
        IMP_LOG_WARN(TAG, "chn %d: I2D rotate/flip kept, not applied (as the T33 vendor library)", chnNum);
#endif
    return 0;
}

int IMP_FrameSource_GetI2dAttr(int chnNum, IMPFSI2DAttr *attr)
{
    if (!attr)
        return FS_ERR_NULL;
    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (chnNum >= T32_FS_CHANNELS) {
        memset(attr, 0, sizeof(*attr));
        return 0;
    }
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    *attr = fs_chn[chnNum].attr.i2dattr;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_SetPool(int chnNum, int poolID)
{
    uint8_t info[256];
    int rc = 0;

    if ((unsigned)chnNum >= T32_FS_POOL_CHNS)
        return FS_ERR_CHN;
    if (IMP_MemPool_GetById(poolID, info) != 0)
        return FS_ERR_STATE;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (!fs_pool_init) {
        memset(fs_pool, 0xff, sizeof(fs_pool));
        fs_pool_init = 1;
    }
    if (fs_pool[chnNum] >= 0)
        rc = FS_ERR_BUSY;
    else
        fs_pool[chnNum] = poolID;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

/* vendor: the bound pool id, 0x80010020 while none is bound */
int IMP_FrameSource_GetPool(int chnNum)
{
    int rc;

    if ((unsigned)chnNum >= T32_FS_POOL_CHNS)
        return FS_ERR_CHN;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    rc = fs_pool_init && fs_pool[chnNum] >= 0 ? fs_pool[chnNum] : FS_ERR_BUSY;
    pthread_mutex_unlock(&fs_lock);
    return rc;
}

/* deadline on the channel condition's clock (CLOCK_MONOTONIC) */
static void fs_deadline(struct timespec *t, int sec)
{
    clock_gettime(CLOCK_MONOTONIC, t);
    t->tv_sec += sec;
}

int IMP_FrameSource_GetTimedFrame(int chnNum, void *framets, int block, void *framedata,
                                  void *frame)
{
    const struct { uint64_t ts, minus, plus; } *ts = framets;   /* vendor IMPFrameTimestamp; only ts is used */
    IMPFrameInfo *out = frame;
    struct t32_fs_chn *c;
    struct timespec until;
    int rc;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!ts)
        return FS_ERR_PARAM;
    if (chnNum >= T32_FS_CHANNELS)
        return FS_ERR_TIMEOUT;  /* vendor: no max delay on a channel never created */
    pthread_once(&fs_once, fs_init_once);
    c = &fs_chn[chnNum];
    fs_deadline(&until, 5);
    pthread_mutex_lock(&fs_lock);
    for (;;) {
        const struct t32_fs_cached *best = NULL, *oldest = NULL, *newest = NULL;
        uint64_t bd = 0;
        int i;

        if (!c->maxdelay || !c->cache) {
            pthread_mutex_unlock(&fs_lock);
            return FS_ERR_TIMEOUT;
        }
        for (i = 0; i < c->cache_cap; i++) {
            const struct t32_fs_cached *e = &c->cache[i];
            uint64_t t, d;

            if (!e->valid)
                continue;
            t = (uint64_t)e->info.timeStamp;
            if (!oldest || t < (uint64_t)oldest->info.timeStamp)
                oldest = e;
            if (!newest || t > (uint64_t)newest->info.timeStamp)
                newest = e;
            d = t > ts->ts ? t - ts->ts : ts->ts - t;
            if (!best || d < bd || (d == bd && t > (uint64_t)best->info.timeStamp)) {
                best = e;
                bd = d;
            }
        }
        if (oldest && (uint64_t)oldest->info.timeStamp >= ts->ts) {
            /* vendor: a time at or before the oldest kept frame is an error */
            pthread_mutex_unlock(&fs_lock);
            return FS_ERR_TIMEOUT;
        }
        if (newest && (uint64_t)newest->info.timeStamp >= ts->ts) {
            if (framedata)
                memcpy(framedata, best->data, best->info.size);
            if (out) {
                *out = best->info;
                if (framedata)
                    out->virAddr = (uint32_t)(uintptr_t)framedata;
            }
            pthread_mutex_unlock(&fs_lock);
            return 0;
        }
        if (!block) {
            pthread_mutex_unlock(&fs_lock);
            return FS_ERR_STATE;
        }
        rc = pthread_cond_timedwait(&c->ready, &fs_lock, &until);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&fs_lock);
            return FS_ERR_TIMEOUT;
        }
    }
}

static pthread_mutex_t fs_snap_lock = PTHREAD_MUTEX_INITIALIZER;

int IMP_FrameSource_SnapFrame(int chnNum, IMPPixelFormat fmt, int width, int height,
                              void *framedata, IMPFrameInfo *frame)
{
    struct t32_fs_chn *c;
    struct timespec until;
    int rc = 0;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!framedata)
        return FS_ERR_NULL;
    if (chnNum >= T32_FS_CHANNELS)
        return FS_ERR_STATE;
    pthread_once(&fs_once, fs_init_once);
    c = &fs_chn[chnNum];
    pthread_mutex_lock(&fs_lock);
    if (fs_state_locked(chnNum) != 2 || c->direct ||
        ((uint32_t)width != c->width && (uint32_t)height != c->height)) {
        IMP_LOG_ERR(TAG, "SnapFrame chn %d: %s", chnNum,
                    fs_state_locked(chnNum) != 2 ? "not enabled" :
                    c->direct ? "IVDC direct channel" : "size differs from the channel");
        pthread_mutex_unlock(&fs_lock);
        return FS_ERR_STATE;
    }
    pthread_mutex_unlock(&fs_lock);
    if (fmt != PIX_FMT_NV12) {
        IMP_LOG_ERR(TAG, "SnapFrame chn %d: format %d not supported (NV12 only)", chnNum, (int)fmt);
        return FS_ERR_PARAM;
    }
    pthread_mutex_lock(&fs_snap_lock);
    fs_deadline(&until, 3);
    pthread_mutex_lock(&fs_lock);
    c->snap_buf = framedata;
    c->snap_info = frame;
    c->snap = 1;
    while (c->snap != 3 && c->enabled) {
        if (pthread_cond_timedwait(&c->ready, &fs_lock, &until) == ETIMEDOUT && c->snap != 2) {
            rc = FS_ERR_TIMEOUT;
            break;
        }
    }
    if (c->snap != 3 && !rc)
        rc = FS_ERR_TIMEOUT;
    c->snap = 0;
    c->snap_buf = NULL;
    c->snap_info = NULL;
    pthread_mutex_unlock(&fs_lock);
    pthread_mutex_unlock(&fs_snap_lock);
    return rc;
}

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

static int inj_release_locked(int chn, IMPFrameInfo *frame);

int OpenIMP_FrameSource_ReleaseSinkFrame(int fs_chn_num, IMPFrameInfo *frame)
{
    struct t32_fs_chn *c;
    int rc = -1;

    if (!frame)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (inj_release_locked(fs_chn_num, frame)) {        /* ExternInject channel (F1) */
        pthread_mutex_unlock(&fs_lock);
        return 0;
    }
    pthread_mutex_unlock(&fs_lock);
    c = fs_get(fs_chn_num);
    if (!c)
        return -1;
    pthread_mutex_lock(&fs_lock);
    if (c->enabled && frame->index >= 0 && frame->index < c->nbufs &&
        frame == &c->buf[frame->index].frame) {
        struct t32_fs_buf *b = &c->buf[frame->index];

        if (b->holds > 0 && --b->holds == 0 && b->out && c->run && !c->direct)
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

int OpenIMP_T32_FsSourceOf(int devID, int grpID)
{
    IMPCell at;
    int i, hops, found;

    at.deviceID = devID;
    at.groupID = grpID;
    at.outputID = 0;
    pthread_mutex_lock(&fs_lock);
    for (hops = 0; hops <= T32_FS_MAX_BINDS; hops++) {
        if (at.deviceID == DEV_ID_FS) {
            pthread_mutex_unlock(&fs_lock);
            return at.groupID;
        }
        for (i = 0, found = 0; i < T32_FS_MAX_BINDS && !found; i++)
            if (fs_bind[i].used && fs_bind[i].dst.deviceID == at.deviceID &&
                fs_bind[i].dst.groupID == at.groupID) {
                at = fs_bind[i].src;
                found = 1;
            }
        if (!found)
            break;
    }
    pthread_mutex_unlock(&fs_lock);
    return -1;
}

int IMP_System_Bind(IMPCell *srcCell, IMPCell *dstCell)
{
    int i, freei = -1, sink = 0;

    if (!srcCell || !dstCell)
        return -1;
    if (srcCell->outputID != 0 || srcCell->groupID < 0 ||
        !((srcCell->deviceID == DEV_ID_FS && srcCell->groupID < 12 /* + ExternInject */) ||
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

extern int32_t IMP_ISP_OneShotTick(void);
#include "t32/openimp_t32_fs_f1.inc"
