/* -*- c -*- included by openimp_t32_fs.c */
/*
 * FrameSource joints (IMP_FrameSource_CreateJoint & co, #784): several channels written by the ISP side by side
 * into one shared frame, handed out by the board's first channel.
 *
 * Vendor libimp T33 2.0.2.1 (Ghidra numbering, objdump = Ghidra - 0x10000):
 * - CreateJoint 0x14a460: board of 12 rows x 12 channel numbers (0xff ends a row / the board); a 0x1b8-byte joint
 *   per board, marked at chn+0x428 of every channel in it (one already marked -> 0x80010004), kept in the table
 *   fs+0x317c (12 slots).  Width = widest row (sum of the channel widths), height = sum of the row heights (the
 *   tallest channel of a row).  Offsets: the first channel of a row at Y width x (heights above), UV half of
 *   it; every further one at the previous one + its width (Y and UV).
 * - EnableChn 0x14f3b0: a channel with chn+0x428 gets VBMCreateJointPool (0x3ff94) instead of VBMCreatePool: the
 *   first enabled channel of the joint allocates nrVBs frames of max(NV12 size of the joint, the channel's
 *   buffer size) and fills each with the background (fs+0x31b0, SetJointBackground 0x14ac2c: 0 Y 0x00, 1 Y
 *   0xff, UV 0x80; other values no fill); every channel's frame i = joint frame i + its Y offset, UV at the
 *   joint frame's UV plane + its UV offset, stride ALIGN16(joint width).  The board's first channel's frames
 *   have the joint size, the others their own.
 * - QBUF 0x143488: the size word stays the channel's own picture (the ISP writes it at its offset with the
 *   joint stride); for the first channel, once its frame i was handed out whole (flag joint+0x17c+12i), every
 *   other channel's frame i is given back first (VBMUnlockFrameByPaddr at the joint frame + its Y offset).
 * - frame update 0x144eb4 (every DQBUF): mask joint+0x17e+12i |= 1 << chn; all channels in -> sem_post
 *   (joint+0x4c); a channel other than the first keeps its frame (VBMLockFrameByPaddr) and passes it on as usual;
 *   the first one waits on the semaphore until all channels wrote frame i, clears the mask and sets the flag,
 *   then passes the whole frame on.
 * - DestroyJoint 0x14ab24 / GetJointByHandler 0x14a99c: look the handle up in the table, unknown 0x80010008.
 *
 * Here: the same layout and hand-out.  Differences (none visible in a normal joint): the vertical offsets use
 * the stride (the vendor's the unaligned width: equal when the width is a multiple of 16); every other channel's
 * frame is held, also the last one in (the vendor does not hold that one, the ISP may overwrite it while the
 * joint frame is still in use); the pool has the frames of the joint channel with the most buffers (the vendor:
 * the first enabled channel's nrVBs, a later channel with more indexes past it); the output channel's wait ends
 * when the channel stops; DestroyJoint frees the channels for a new joint and keeps a board in use by enabled
 * channels until the last of them is disabled (the vendor frees it under them).
 */
#define FS_JOINTS      12
#define FS_JOINT_CHNS  12           /* channel numbers a board may name (vendor FrameSource channels) */

struct fs_joint {
    uint8_t pos[12][12];
    int16_t w, h;                   /* joint picture */
    int32_t handle;
    int out;                        /* the board's first channel: hands out the whole frame */
    uint32_t mask;                  /* every channel on the board */
    uint32_t yoff[FS_JOINT_CHNS], uvoff[FS_JOINT_CHNS];
    uint32_t stride, ysize, fsize;  /* per joint frame */
    int nframes, users, dead;
    IMPDMABufferInfo pool;
    uint32_t got[T32_FS_MAX_BUFS];  /* channels that wrote frame i */
    int whole[T32_FS_MAX_BUFS];     /* frame i went out whole: the others' frame i come back with it */
    sem_t sem;
};

static struct fs_joint *fs_joints[FS_JOINTS];
static struct fs_joint *fs_joint_of[FS_JOINT_CHNS];  /* vendor chn+0x428 */
static uint32_t fs_joint_bg;                         /* vendor fs+0x31b0 */

static int fs_joint_find_locked(int32_t h)
{
    int i;

    for (i = 0; i < FS_JOINTS; i++)
        if (fs_joints[i] && fs_joints[i]->handle == h)
            return i;
    return -1;
}

/* layout of a board with the channels' sizes at creation (vendor CreateJoint) */
static void fs_joint_layout_locked(struct fs_joint *j)
{
    uint32_t rowh[12] = { 0 }, w = 0, h = 0, above = 0;
    int r, c;

    for (r = 0; r < 12 && j->pos[r][0] != 0xff; r++) {
        uint32_t rw = 0;

        for (c = 0; c < 12 && j->pos[r][c] != 0xff; c++) {
            unsigned ch = j->pos[r][c];
            uint32_t cw = 0, chh = 0;

            if (ch < T32_FS_CHANNELS && fs_chn[ch].created) {
                cw = (uint32_t)FS_LIVE(&fs_chn[ch])->picWidth;
                chh = (uint32_t)FS_LIVE(&fs_chn[ch])->picHeight;
            }
            rw += cw;
            if (chh > rowh[r])
                rowh[r] = chh;
            j->mask |= 1u << ch;
        }
        if (rw > w)
            w = rw;
        h += rowh[r];
    }
    j->w = (int16_t)w;
    j->h = (int16_t)h;
    j->out = j->pos[0][0] != 0xff ? j->pos[0][0] : -1;
    j->stride = ALIGN_UP(w, 16);
    j->ysize = j->stride * ALIGN_UP(h, 16);
    j->fsize = j->ysize * 3 / 2;
    for (r = 0; r < 12 && j->pos[r][0] != 0xff; r++) {
        uint32_t x = 0;

        for (c = 0; c < 12 && j->pos[r][c] != 0xff; c++) {
            unsigned ch = j->pos[r][c];

            j->yoff[ch] = j->stride * above + x;
            j->uvoff[ch] = j->stride * (above / 2) + x;
            if (ch < T32_FS_CHANNELS && fs_chn[ch].created)
                x += (uint32_t)FS_LIVE(&fs_chn[ch])->picWidth;
        }
        above += rowh[r];
    }
}

int OpenIMP_T32_FsJointCreate(const uint8_t *board, int16_t *w, int16_t *h, int32_t *handle)
{
    struct fs_joint *j;
    int r, c, slot;

    pthread_once(&fs_once, fs_init_once);
    j = calloc(1, sizeof(*j));
    if (!j)
        return (int)0x80010100;
    memcpy(j->pos, board, sizeof(j->pos));
    pthread_mutex_lock(&fs_lock);
    /* vendor: a channel already in a joint -> 0x80010004 (checked first here: a refused board marks nothing) */
    for (r = 0; r < 12 && j->pos[r][0] != 0xff; r++)
        for (c = 0; c < 12 && j->pos[r][c] != 0xff; c++)
            if (j->pos[r][c] >= FS_JOINT_CHNS || fs_joint_of[j->pos[r][c]]) {
                pthread_mutex_unlock(&fs_lock);
                free(j);
                return (int)0x80010004;
            }
    for (slot = 0; slot < FS_JOINTS && fs_joints[slot]; slot++)
        ;
    if (slot == FS_JOINTS) {
        pthread_mutex_unlock(&fs_lock);
        free(j);
        return (int)0x80010100;
    }
    fs_joint_layout_locked(j);
    j->handle = (int32_t)(intptr_t)j;
    sem_init(&j->sem, 0, 0);
    fs_joints[slot] = j;
    for (r = 0; r < 12 && j->pos[r][0] != 0xff; r++)
        for (c = 0; c < 12 && j->pos[r][c] != 0xff; c++)
            fs_joint_of[j->pos[r][c]] = j;
    *w = j->w;
    *h = j->h;
    *handle = j->handle;
    pthread_mutex_unlock(&fs_lock);
    IMP_LOG_INFO(TAG, "joint 0x%08x: %dx%d, output chn %d", (unsigned)j->handle, j->w, j->h, j->out);
    return 0;
}

static void fs_joint_free(struct fs_joint *j)
{
    fs_free_dma(&j->pool);
    sem_destroy(&j->sem);
    free(j);
}

int OpenIMP_T32_FsJointDestroy(int32_t handle)
{
    struct fs_joint *j;
    int i, busy;

    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    i = fs_joint_find_locked(handle);
    if (i < 0) {
        pthread_mutex_unlock(&fs_lock);
        return (int)0x80010008;
    }
    j = fs_joints[i];
    fs_joints[i] = NULL;
    for (i = 0; i < FS_JOINT_CHNS; i++)
        if (fs_joint_of[i] == j)
            fs_joint_of[i] = NULL;
    busy = j->users > 0;
    j->dead = 1;                    /* the last DisableChn of its channels frees it */
    pthread_mutex_unlock(&fs_lock);
    if (!busy)
        fs_joint_free(j);
    return 0;
}

int OpenIMP_T32_FsJointGet(int32_t handle, uint8_t *board, int16_t *w, int16_t *h)
{
    int i;

    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    i = fs_joint_find_locked(handle);
    if (i >= 0) {
        memcpy(board, fs_joints[i]->pos, sizeof(fs_joints[i]->pos));
        *w = fs_joints[i]->w;
        *h = fs_joints[i]->h;
    }
    pthread_mutex_unlock(&fs_lock);
    return i < 0 ? (int)0x80010008 : 0;
}

void OpenIMP_T32_FsJointBackground(uint32_t color)
{
    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    fs_joint_bg = color;
    pthread_mutex_unlock(&fs_lock);
}

/* the most buffers a channel of the board asks for (EnableChn REQBUFS count) */
static int fs_joint_frames_locked(const struct fs_joint *j)
{
    int ch, n = 1;

    for (ch = 0; ch < T32_FS_CHANNELS; ch++) {
        const struct t32_fs_chn *o = &fs_chn[ch];
        int want;

        if (!(j->mask & (1u << ch)) || !o->created)
            continue;
        want = FS_LIVE(o)->nrVBs + (o->maxdelay > 0 ? o->maxdelay : 0);
        if (want > n)
            n = want;
    }
    return n > T32_FS_MAX_BUFS ? T32_FS_MAX_BUFS : n;
}

/* EnableChn: the channel's buffers become views into the joint frames (VBMCreateJointPool).  0, or -1 */
static int fs_joint_attach_locked(struct t32_fs_chn *c, int chn)
{
    struct fs_joint *j = chn < FS_JOINT_CHNS ? fs_joint_of[chn] : NULL;
    int i;

    c->joint = NULL;
    c->pw = c->width;
    c->ph = c->height;
    c->pstride = c->stride;
    c->palloc = c->alloc;
    if (!j)
        return 0;
    if (c->direct) {
        IMP_LOG_ERR(TAG, "EnableChn chn %d: in a joint, but IVDC direct (frames do not reach memory)", chn);
        return -1;
    }
    if (!j->pool.phys_addr) {
        uint32_t f;

        if (c->alloc > j->fsize)
            j->fsize = c->alloc;
        j->nframes = fs_joint_frames_locked(j);
        if (fs_alloc(&j->pool, j->fsize * (uint32_t)j->nframes, "fs-joint") < 0)
            return -1;
        for (f = 0; f < (uint32_t)j->nframes; f++) {
            uint8_t *p = (uint8_t *)(uintptr_t)j->pool.virt_addr + f * j->fsize;

            if (fs_joint_bg == 0 || fs_joint_bg == 1) {
                memset(p, fs_joint_bg ? 0xff : 0x00, j->ysize);
                memset(p + j->ysize, 0x80, j->fsize - j->ysize);
            }
            j->got[f] = 0;
            j->whole[f] = 0;
        }
        DMA_RmemFlushCache((void *)(uintptr_t)j->pool.virt_addr, j->fsize * (uint32_t)j->nframes, 1);
        IMP_LOG_INFO(TAG, "joint 0x%08x: %d frames of %u bytes at 0x%08x, background %u", (unsigned)j->handle,
                     j->nframes, j->fsize, j->pool.phys_addr, fs_joint_bg);
    }
    if (c->nbufs > j->nframes) {
        IMP_LOG_ERR(TAG, "EnableChn chn %d: %d buffers, the joint has %d frames", chn, c->nbufs, j->nframes);
        if (!j->users)
            fs_free_dma(&j->pool);
        return -1;
    }
    for (i = 0; i < c->nbufs; i++) {
        struct t32_fs_buf *b = &c->buf[i];
        uint32_t base = (uint32_t)i * j->fsize;

        memset(&b->dma, 0, sizeof(b->dma));
        b->dma.phys_addr = j->pool.phys_addr + base + j->yoff[chn];
        b->dma.virt_addr = j->pool.virt_addr + base + j->yoff[chn];
        b->dma.size = j->fsize - j->yoff[chn];
        b->uv_off = j->ysize + j->uvoff[chn] - j->yoff[chn];
        b->jview = 1;
        b->jheld = 0;
    }
    c->joint = j;
    c->pstride = j->stride;
    if (chn == j->out) {
        c->pw = (uint32_t)j->w;
        c->ph = (uint32_t)j->h;
        c->palloc = j->fsize;
    }
    j->users++;
    return 0;
}

/* DisableChn / a failed EnableChn: the channel leaves the joint frames; the output channel gives the others'
 * held frames back */
static void fs_joint_detach_locked(struct t32_fs_chn *c, int chn)
{
    struct fs_joint *j = c->joint;
    int i, ch;

    if (!j)
        return;
    c->joint = NULL;
    for (i = 0; i < T32_FS_MAX_BUFS; i++) {
        c->buf[i].jheld = 0;
        if (c->buf[i].jview)
            memset(&c->buf[i].dma, 0, sizeof(c->buf[i].dma));
        c->buf[i].jview = 0;
    }
    if (chn == j->out)
        for (ch = 0; ch < T32_FS_CHANNELS; ch++) {
            struct t32_fs_chn *o = &fs_chn[ch];

            if (o == c || o->joint != j)
                continue;
            for (i = 0; i < o->nbufs; i++) {
                struct t32_fs_buf *ob = &o->buf[i];

                if (!ob->jheld)
                    continue;
                ob->jheld = 0;
                if (ob->holds > 0 && --ob->holds == 0 && ob->out && o->run)
                    fs_qbuf(o, ob);
            }
        }
    if (--j->users <= 0) {
        j->users = 0;
        fs_free_dma(&j->pool);
        if (j->dead) {
            sem_destroy(&j->sem);
            free(j);
        }
    }
}

/* frame thread, after DQBUF of buffer i (vendor frame update 0x144eb4) */
static void fs_joint_frame(struct t32_fs_chn *c, int chn, struct t32_fs_buf *b)
{
    struct fs_joint *j;
    int i = (int)(b - c->buf), full;

    pthread_mutex_lock(&fs_lock);
    j = c->joint;
    if (!j || i >= T32_FS_MAX_BUFS) {
        pthread_mutex_unlock(&fs_lock);
        return;
    }
    j->got[i] |= 1u << chn;
    full = j->got[i] == j->mask;
    if (full)
        sem_post(&j->sem);
    if (chn != j->out) {
        /* kept until the joint frame i is queued again (vendor VBMLockFrameByPaddr) */
        if (!b->jheld) {
            b->jheld = 1;
            b->holds++;
        }
        pthread_mutex_unlock(&fs_lock);
        return;
    }
    pthread_mutex_unlock(&fs_lock);
    /* the output channel waits for the others' frame i; the channel's stop ends the wait */
    for (;;) {
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(&j->sem, &ts) == 0)
            break;
        if (!c->run)
            return;
    }
    pthread_mutex_lock(&fs_lock);
    j->got[i] = 0;
    j->whole[i] = 1;
    pthread_mutex_unlock(&fs_lock);
    DMA_RmemFlushCache((void *)(uintptr_t)(j->pool.virt_addr + (uint32_t)i * j->fsize), j->fsize, 2);
}

/* fs_qbuf of the output channel's buffer i: the other channels' frame i are queued again (vendor QBUF
 * 0x143488 -> VBMUnlockFrameByPaddr) */
static void fs_joint_requeue_locked(struct t32_fs_chn *c, struct t32_fs_buf *b)
{
    struct fs_joint *j = c->joint;
    int i = (int)(b - c->buf), ch;

    if (!j || c != &fs_chn[j->out < 0 ? 0 : j->out] || i >= T32_FS_MAX_BUFS || !j->whole[i])
        return;
    j->whole[i] = 0;
    for (ch = 0; ch < T32_FS_CHANNELS; ch++) {
        struct t32_fs_chn *o = &fs_chn[ch];
        struct t32_fs_buf *ob;

        if (o == c || o->joint != j || i >= o->nbufs)
            continue;
        ob = &o->buf[i];
        if (!ob->jheld)
            continue;
        ob->jheld = 0;
        if (ob->holds > 0 && --ob->holds == 0 && ob->out && o->run && !o->direct)
            fs_qbuf(o, ob);
    }
}

/* the encoder (openimp_t32_encoder.c): stride and UV address of a frame in a joint frame; 0 = not in one */
int OpenIMP_T32_FsFrameLayout(uint32_t phys, uint32_t *stride, uint32_t *uv)
{
    int k, found = 0;

    pthread_once(&fs_once, fs_init_once);
    pthread_mutex_lock(&fs_lock);
    for (k = 0; k < FS_JOINT_CHNS && !found; k++) {
        struct fs_joint *j = k < T32_FS_CHANNELS ? fs_chn[k].joint : NULL;
        uint32_t off, f, rel, row, col;

        if (!j || !j->pool.phys_addr || phys < j->pool.phys_addr ||
            phys >= j->pool.phys_addr + j->fsize * (uint32_t)j->nframes)
            continue;
        off = phys - j->pool.phys_addr;
        f = off / j->fsize;
        rel = off % j->fsize;
        row = rel / j->stride;
        col = rel % j->stride;
        *stride = j->stride;
        *uv = j->pool.phys_addr + f * j->fsize + j->ysize + (row / 2) * j->stride + col;
        found = 1;
    }
    pthread_mutex_unlock(&fs_lock);
    return found;
}

/* after DQBUF: the whole joint frame i (the other channels' pictures and the UV plane lie outside the view) */
static void fs_joint_flush(const struct t32_fs_chn *c, const struct t32_fs_buf *b)
{
    const struct fs_joint *j = c->joint;
    uint32_t i = (uint32_t)(b - c->buf);

    DMA_RmemFlushCache((void *)(uintptr_t)(j->pool.virt_addr + i * j->fsize), j->fsize, 2);
}

#if defined(PLATFORM_T33)
/* T33 GetFrame copy of a channel other than the output one: its picture out of the joint frame, laid out as a
 * frame of its own (stride ALIGN16(width), UV after ALIGN16(height) rows; the vendor copies it as if it were
 * not in a joint) */
static void fs_joint_copy_sub(const struct t32_fs_chn *c, const struct t32_fs_buf *b, uint8_t *dst)
{
    const uint8_t *y = (const uint8_t *)(uintptr_t)b->dma.virt_addr;
    const uint8_t *uv = y + b->uv_off;
    uint8_t *duv = dst + c->stride * ALIGN_UP(c->height, 16);
    uint32_t r;

    for (r = 0; r < c->height; r++)
        memcpy(dst + r * c->stride, y + r * c->pstride, c->width);
    for (r = 0; r < c->height / 2; r++)
        memcpy(duv + r * c->stride, uv + r * c->pstride, c->width);
}
#endif
