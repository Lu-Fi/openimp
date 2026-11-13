/*
 * T32/T33 FrameSource data paths of the 2.x libimp (batch F1 of #57 / #42;
 * included at the end of openimp_t32_fs.c: it shares the channel table,
 * the bind table and the sinks).  Return codes and checks as vendor libimp
 * T33 2.0.2.1 / T32 2.2.0 (disassembly, QEMU runs; no vendor code copied):
 *
 * ExternInject (a channel fed by the application instead of the ISP):
 *   CreateChn(chn, attr)  chn < 12, attr != NULL, picWidth a multiple of
 *                         16, channel 0 only of type FS_PHY_CHANNEL: else -1;
 *                         a channel that already exists (sensor or inject):
 *                         0 without a change
 *   EnableChn(chn)        chn < 12, created: nrVBs NV12 frames (width x
 *                         ALIGN16(height) x 3/2, UV after the luma rows),
 *                         a free and a filled FIFO; enabled again: 0
 *   DisableChn(chn)       enabled -> created (bound modules give their frames
 *                         back first), else -1
 *   DestroyChn(chn)       created -> gone; not created: 0; enabled: -1
 *   DequeueBuffer(chn, &f) a free frame (-1 none / not enabled); -1 always in
 *                         IVDC direct mode (System_Init's direct flag)
 *   QueueBuffer(chn, f)   the frame goes to the modules bound to the channel
 *                         ({DEV_ID_FS, chn}); -1 when it is not a dequeued
 *                         frame of the channel
 * GetFrameEx / ReleaseFrameEx (main process): chn < 12 (0x80010001), NULL
 *   (0x80010010), channel not created (0x80010040); GetFrameEx waits for the
 *   next frame of the channel and keeps that capture buffer until
 *   ReleaseFrameEx (which wants the same channel in frame->pool_idx,
 *   0x80010040, and a frame of the channel, 0x80010008).  The vendor's
 *   secondary-process path (/dev/mpsys_data) does not exist here: OpenIMP
 *   is always the main process.
 * PM_Suspend / PM_Resume: Suspend needs an enabled channel (0x80010040):
 *   the frame thread stops (the bound modules finish their frames), the
 *   channel is "suspended", "FS_PM" goes to /sys/power/wake_unlock (-1 when
 *   the file cannot be written).  Resume needs a suspended channel
 *   (0x80011000): the frame thread runs again, "FS_PM" goes to
 *   /sys/power/wake_lock (0x80014000 when the file cannot be written) and,
 *   when no channel is suspended any more, IMP_ISP_OneShotTick (T32 only,
 *   the T33 is a "t32v" chip for the vendor library; failure: 0x80012000).
 */

#define FS_ERR_NOTREADY ((int)0x80011000)
#define FS_ERR_RESOURCE ((int)0x80014000)
#define FS_ERR_NOFS     ((int)0x80010008)
#define T32_INJ_CHANNELS 12
#define T32_INJ_MAX_BUFS 16

struct t32_inj_buf {
    IMPDMABufferInfo dma;
    IMPFrameInfo frame;
    int state;                  /* INJ_FREE .. INJ_OUT */
    int holds;
};

enum { INJ_FREE, INJ_DEQUEUED, INJ_OUT };

struct t32_inj_chn {
    int state;                  /* 0 none, 1 created, 2 enabled */
    IMPFSChnAttr attr;
    uint32_t stride, alloc;
    int nbufs;
    struct t32_inj_buf buf[T32_INJ_MAX_BUFS];
    uint32_t queued, delivered;
};

static struct t32_inj_chn fs_inj[T32_INJ_CHANNELS];

static int inj_state_locked(int chn)
{
    return chn >= 0 && chn < T32_INJ_CHANNELS ? fs_inj[chn].state : 0;
}

/* the sinks reached from FrameSource channel chn (also behind OSD groups) */
static int fs_collect_sinks_locked(int chn, int direct, OpenIMP_T32_FrameSink *fn, void **ctx,
                                   int max)
{
    IMPCell from[T32_FS_MAX_BINDS + 1];
    int i, j, k, n = 0, nfrom = 1;

    from[0].deviceID = DEV_ID_FS;
    from[0].groupID = chn;
    for (k = 0; k < nfrom; k++)
        for (i = 0; i < T32_FS_MAX_BINDS; i++) {
            if (!fs_bind[i].used || fs_bind[i].src.deviceID != from[k].deviceID ||
                fs_bind[i].src.groupID != from[k].groupID)
                continue;
            for (j = 0; j < T32_FS_MAX_SINKS && n < max; j++)
                if (fs_sink[j].used && fs_sink[j].dev == (int)fs_bind[i].dst.deviceID &&
                    fs_sink[j].grp == (int)fs_bind[i].dst.groupID &&
                    (!direct || fs_sink[j].dev == DEV_ID_ENC)) {
                    fn[n] = fs_sink[j].fn;
                    ctx[n] = fs_sink[j].ctx;
                    n++;
                    break;
                }
            if (fs_bind[i].dst.deviceID == DEV_ID_OSD && nfrom <= T32_FS_MAX_BINDS)
                from[nfrom++] = fs_bind[i].dst;
        }
    return n;
}

static int inj_get_attr_locked(int chn, IMPFSChnAttr *a)
{
    if (!inj_state_locked(chn))
        return -1;
    *a = fs_inj[chn].attr;
    return 0;
}

static void inj_free_bufs_locked(struct t32_inj_chn *c)
{
    int i;

    for (i = 0; i < T32_INJ_MAX_BUFS; i++) {
        fs_free_dma(&c->buf[i].dma);
        c->buf[i].state = INJ_FREE;
        c->buf[i].holds = 0;
    }
    c->nbufs = 0;
}

int IMP_FrameSource_ExternInject_CreateChn(int chnNum, IMPFSChnAttr *chnAttr)
{
    pthread_once(&fs_once, fs_init_once);
    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS) {
        IMP_LOG_ERR(TAG, "ExternInject_CreateChn: chn %d out of 0..%d", chnNum,
                    T32_INJ_CHANNELS - 1);
        return -1;
    }
    if (!chnAttr) {
        IMP_LOG_ERR(TAG, "ExternInject_CreateChn chn %d: attr NULL", chnNum);
        return -1;
    }
    if (chnAttr->picWidth & 0xf) {
        IMP_LOG_ERR(TAG, "chnNum=%d, chnAttr->picWidth should be align to 16", chnNum);
        return -1;
    }
    if (chnNum == 0 && chnAttr->type != FS_PHY_CHANNEL) {
        IMP_LOG_ERR(TAG, "ExternInject_CreateChn chn 0: type %d (physical channel only)",
                    (int)chnAttr->type);
        return -1;
    }
    pthread_mutex_lock(&fs_lock);
    if (fs_inj[chnNum].state == 0 && fs_state_locked(chnNum) == 0) {
        memset(&fs_inj[chnNum], 0, sizeof(fs_inj[chnNum]));
        fs_inj[chnNum].attr = *chnAttr;
        fs_inj[chnNum].state = 1;
    }
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_ExternInject_EnableChn(int chnNum)
{
    struct t32_inj_chn *c;
    uint32_t w, h, i;

    pthread_once(&fs_once, fs_init_once);
    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS)
        return -1;
    pthread_mutex_lock(&fs_lock);
    c = &fs_inj[chnNum];
    if (c->state == 2) {
        pthread_mutex_unlock(&fs_lock);
        return 0;
    }
    if (c->state != 1 || c->attr.picWidth <= 0 || c->attr.picHeight <= 0 ||
        c->attr.pixFmt != PIX_FMT_NV12 || c->attr.nrVBs < 0 ||
        c->attr.nrVBs > T32_INJ_MAX_BUFS) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "ExternInject_EnableChn chn %d: %s", chnNum,
                    c->state != 1 ? "not created" : "attribute (NV12, nrVBs 0..16)");
        return -1;
    }
    w = (uint32_t)c->attr.picWidth;
    h = (uint32_t)c->attr.picHeight;
    c->stride = w;
    c->alloc = w * ALIGN_UP(h, 16) * 3 / 2;
    for (i = 0; i < (uint32_t)c->attr.nrVBs; i++) {
        struct t32_inj_buf *b = &c->buf[i];

        if (fs_alloc(&b->dma, c->alloc, "fs-inject") < 0) {
            inj_free_bufs_locked(c);
            pthread_mutex_unlock(&fs_lock);
            return -1;
        }
        memset(&b->frame, 0, sizeof(b->frame));
        b->frame.index = (int)i;
        b->frame.pool_idx = chnNum;
        b->frame.width = w;
        b->frame.height = h;
        b->frame.pixfmt = PIX_FMT_NV12;
        b->frame.size = c->alloc;
        b->frame.phyAddr = b->dma.phys_addr;
        b->frame.virAddr = b->dma.virt_addr;
        b->state = INJ_FREE;
        b->holds = 0;
        c->nbufs = (int)i + 1;
    }
    c->queued = c->delivered = 0;
    c->state = 2;
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_ExternInject_DisableChn(int chnNum)
{
    struct t32_inj_chn *c;
    int i, tries;

    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    c = &fs_inj[chnNum];
    if (c->state != 2) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "ExternInject_DisableChn chn %d: not enabled", chnNum);
        return -1;
    }
    c->state = 1;               /* no more Dequeue / Queue */
    /* the bound modules finish their frames (vendor: flush_module_tree_sync,
     * 10 tries) */
    for (tries = 0; tries < 100; tries++) {
        int held = 0;

        for (i = 0; i < c->nbufs; i++)
            held += c->buf[i].holds;
        if (!held)
            break;
        pthread_mutex_unlock(&fs_lock);
        usleep(10000);
        pthread_mutex_lock(&fs_lock);
    }
    if (tries == 100)
        IMP_LOG_ERR(TAG, "ExternInject_DisableChn chn %d: frames still held by a module", chnNum);
    inj_free_bufs_locked(c);
    IMP_LOG_INFO(TAG, "inject chn %d off: %u queued, %u to modules", chnNum, c->queued,
                 c->delivered);
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

int IMP_FrameSource_ExternInject_DestroyChn(int chnNum)
{
    int rc = 0;

    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    if (fs_inj[chnNum].state == 2)
        rc = -1;
    else
        memset(&fs_inj[chnNum], 0, sizeof(fs_inj[chnNum]));
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "ExternInject_DestroyChn chn %d: still enabled", chnNum);
    return rc;
}

int IMP_FrameSource_DequeueBuffer(int chnNum, IMPFrameInfo **frame)
{
    struct t32_inj_chn *c;
    int i;

    if (OpenIMP_T32_IvdcDirectMode() >= 1) {
        IMP_LOG_ERR(TAG, "DequeueBuffer: not in IVDC direct mode");
        return -1;
    }
    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS || !frame)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    c = &fs_inj[chnNum];
    if (c->state == 2)
        for (i = 0; i < c->nbufs; i++)
            if (c->buf[i].state == INJ_FREE) {
                c->buf[i].state = INJ_DEQUEUED;
                c->buf[i].frame.timeStamp = 0;
                *frame = &c->buf[i].frame;
                pthread_mutex_unlock(&fs_lock);
                return 0;
            }
    pthread_mutex_unlock(&fs_lock);
    IMP_LOG_LIMITED(LOG_ERR, TAG, "DequeueBuffer chn %d: no free frame", chnNum);
    return -1;
}

int IMP_FrameSource_QueueBuffer(int chnNum, IMPFrameInfo *frame)
{
    OpenIMP_T32_FrameSink fn[T32_FS_MAX_BINDS];
    void *ctx[T32_FS_MAX_BINDS];
    struct t32_inj_chn *c;
    struct t32_inj_buf *b;
    int i, n, done = 0;

    if (chnNum < 0 || chnNum >= T32_INJ_CHANNELS || !frame)
        return -1;
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    c = &fs_inj[chnNum];
    if (c->state != 2 || frame->index < 0 || frame->index >= c->nbufs ||
        frame != &c->buf[frame->index].frame || c->buf[frame->index].state != INJ_DEQUEUED) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "QueueBuffer chn %d: %p is not a dequeued frame", chnNum, (void *)frame);
        return -1;
    }
    b = &c->buf[frame->index];
    b->state = INJ_OUT;
    if (!b->frame.timeStamp)
        b->frame.timeStamp = (int64_t)fs_now_us();
    n = fs_collect_sinks_locked(chnNum, 0, fn, ctx, T32_FS_MAX_BINDS);
    b->holds = n + 1;           /* +1: this call until the sinks ran */
    c->queued++;
    if (n)
        c->delivered++;
    pthread_mutex_unlock(&fs_lock);
    /* the application wrote the picture with the CPU, the modules read it
     * by DMA */
    DMA_RmemFlushCache((void *)(uintptr_t)b->dma.virt_addr, c->alloc, 1);
    for (i = 0; i < n; i++)
        if (fn[i](ctx[i], chnNum, &b->frame) != 1)
            done++;
    pthread_mutex_lock(&fs_lock);
    b->holds -= done + 1;
    if (b->holds <= 0) {
        b->holds = 0;
        b->state = INJ_FREE;
    }
    pthread_mutex_unlock(&fs_lock);
    return 0;
}

/* ReleaseSinkFrame of an injected channel: 1 handled, 0 not an inject frame */
static int inj_release_locked(int chn, IMPFrameInfo *frame)
{
    struct t32_inj_chn *c;

    if (inj_state_locked(chn) == 0)
        return 0;
    c = &fs_inj[chn];
    if (frame->index >= 0 && frame->index < c->nbufs && frame == &c->buf[frame->index].frame &&
        c->buf[frame->index].holds > 0 && --c->buf[frame->index].holds == 0)
        c->buf[frame->index].state = INJ_FREE;
    return 1;
}

/* ---- GetFrameEx / ReleaseFrameEx ----------------------------------------- */
/* fs_deliver: hand the buffer to a GetFrameEx waiter (holds it) */
static void fs_ex_offer_locked(struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    if (c->ex_want > 0 && !c->ex_buf && !c->direct) {
        c->ex_buf = b;
        b->holds++;
        pthread_cond_broadcast(&c->ready);
    }
}

int IMP_FrameSource_GetFrameEx(int chnNum, IMPFrameInfo **frame)
{
    struct t32_fs_chn *c;
    struct t32_fs_buf *b = NULL;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!frame)
        return FS_ERR_NULL;
    pthread_once(&fs_once, fs_init_once);
    if (chnNum >= T32_FS_CHANNELS)
        return FS_ERR_STATE;
    c = &fs_chn[chnNum];
    pthread_mutex_lock(&fs_lock);
    if (!c->created) {
        pthread_mutex_unlock(&fs_lock);
        return FS_ERR_STATE;
    }
    /* vendor: sem_wait for the next frame, no time limit; here also ended by
     * a channel that goes away */
    c->ex_want++;
    while (c->created && !c->ex_buf) {
        struct timespec until;

        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += 1;
        pthread_cond_timedwait(&c->ready, &fs_lock, &until);
    }
    c->ex_want--;
    if (c->ex_buf) {
        b = c->ex_buf;
        c->ex_buf = NULL;
        c->ex_frame = b->frame;     /* one info block per channel, as the vendor */
        c->ex_frame.pool_idx = chnNum;
        *frame = &c->ex_frame;
    }
    pthread_mutex_unlock(&fs_lock);
    return b ? 0 : FS_ERR_NOFS;
}

int IMP_FrameSource_ReleaseFrameEx(int chnNum, IMPFrameInfo *frame)
{
    struct t32_fs_chn *c;
    int i, rc = FS_ERR_NOFS;

    if (chnNum < 0 || chnNum >= FS_VENDOR_CHNS)
        return FS_ERR_CHN;
    if (!frame)
        return FS_ERR_NULL;
    pthread_once(&fs_once, fs_init_once);
    if (chnNum >= T32_FS_CHANNELS)
        return FS_ERR_STATE;
    c = &fs_chn[chnNum];
    pthread_mutex_lock(&fs_lock);
    if (!c->created || frame->pool_idx != chnNum) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "ReleaseFrameEx chn %d: %s", chnNum,
                    c->created ? "frame of another channel" : "channel not created");
        return FS_ERR_STATE;
    }
    for (i = 0; c->enabled && i < c->nbufs; i++) {
        struct t32_fs_buf *b = &c->buf[i];

        if (b->frame.phyAddr != frame->phyAddr || b->holds <= 0)
            continue;
        if (--b->holds == 0 && b->out && c->run && !c->direct)
            fs_qbuf(c, b);
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&fs_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "ReleaseFrameEx chn %d: 0x%08x is not a held frame", chnNum,
                    frame->phyAddr);
    return rc;
}

/* ---- PM_Suspend / PM_Resume ---------------------------------------------- */
static int fs_pm_suspended;

static int fs_pm_write(const char *path)
{
    FILE *f = fopen(path, "w");
    int rc;

    if (!f) {
        IMP_LOG_ERR(TAG, "Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    rc = fprintf(f, "%s\n", "FS_PM");
    if (fclose(f) != 0 || rc < 0) {
        IMP_LOG_ERR(TAG, "write %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int IMP_FrameSource_PM_Suspend(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int i;

    if (!c)
        return FS_ERR_STATE;
    pthread_mutex_lock(&fs_lock);
    if (!c->created || !c->enabled || c->suspended) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "PM_Suspend chn %d: channel not running", chnNum);
        return FS_ERR_STATE;
    }
    c->run = 0;
    pthread_mutex_unlock(&fs_lock);
    pthread_cancel(c->thread);          /* takes effect in the blocking request */
    pthread_join(c->thread, NULL);
    /* the bound modules finish their frames (vendor: flush_module_tree_sync) */
    for (i = 0; i < 100; i++) {
        int j, held = 0;

        pthread_mutex_lock(&fs_lock);
        for (j = 0; j < c->nbufs; j++)
            held += c->buf[j].holds;
        pthread_mutex_unlock(&fs_lock);
        if (!held)
            break;
        usleep(10000);
    }
    pthread_mutex_lock(&fs_lock);
    c->suspended = 1;
    fs_pm_suspended++;
    pthread_cond_broadcast(&c->ready);
    pthread_mutex_unlock(&fs_lock);
    return fs_pm_write("/sys/power/wake_unlock");
}

int IMP_FrameSource_PM_Resume(int chnNum)
{
    struct t32_fs_chn *c = fs_get(chnNum);
    int i, left;

    if (!c)
        return FS_ERR_NOTREADY;
    pthread_mutex_lock(&fs_lock);
    if (!c->suspended) {
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "PM_Resume chn %d: channel not suspended", chnNum);
        return FS_ERR_NOTREADY;
    }
    /* buffers a module held over the suspend go back to the driver */
    for (i = 0; i < c->nbufs; i++)
        if (c->buf[i].out && !c->buf[i].holds && !c->direct)
            fs_qbuf(c, &c->buf[i]);
    c->run = 1;
    if (pthread_create(&c->thread, NULL, fs_thread, (void *)(intptr_t)chnNum) != 0) {
        c->run = 0;
        pthread_mutex_unlock(&fs_lock);
        IMP_LOG_ERR(TAG, "PM_Resume chn %d: frame thread: %s", chnNum, strerror(errno));
        return FS_ERR_RESOURCE;
    }
    c->suspended = 0;
    left = --fs_pm_suspended;
    pthread_mutex_unlock(&fs_lock);
    if (fs_pm_write("/sys/power/wake_lock") < 0)
        return FS_ERR_RESOURCE;
#if !defined(PLATFORM_T33)
    /* T32: one-shot tick after the last resume; the T33 is a "t32v" chip
     * for the vendor library (no tick, as its PM_SetMode path) */
    if (left == 0 && IMP_ISP_OneShotTick() != 0)
        return FS_ERR_TIMEOUT;     /* vendor: 0x80012000 */
#else
    (void)left;
#endif
    return 0;
}
