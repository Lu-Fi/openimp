/*
 * OpenIMP T32 encoder: JPEG on the JPEG v1.2 engine (#292, planned; not a
 * release gate).  T23-family API with the T32 2.x layouts (imp_encoder.h:
 * IMPEncoderCHNAttr 184 bytes, profile type byte JPEG 0 / AVC 1 / HEVC 2).
 * H.264 / H.265 (HERA) are later T32 issues: CreateChn refuses them.
 *
 * Flow (as vendor libimp 2.2.0 in the QEMU trace, own code):
 *   CreateGroup        registers the group as a FrameSource sink (bind target)
 *   CreateChn          soc_vpu CHANNEL_REQ (codec JPEGENC), job + stream buffers
 *   frame (sink)       a receiving channel of the bound group keeps the frame;
 *                      the channel thread builds the jpeg_reg_info, FLUSH_CACHE
 *                      (write back the job), CHANNEL_RUN on helix0, FLUSH_CACHE
 *                      (invalidate the bitstream), gives the frame back
 *   GetStream          one pack: the engine's file (SOI .. EOI, its own DQT/
 *                      DHT/SOF0/SOS; no APP0), as the vendor hands it over
 *   DestroyChn         CHANNEL_REL
 *
 * Register image (vendor values in tests/t32/vendor-2.2.0-jpeg-job.txt):
 * base tables = ITU-T T.81 Annex K luma / chroma in zigzag order, quality
 * index = 98 - jpegQp (IMP_Encoder_SetJpegQp range 0..98; an initial QP
 * outside it, e.g. SetDefaultParam's -1, gives jpegQp 40 as the vendor
 * does), limit = stream buffer size, frame-end + over-limit interrupts.
 * A picture over the limit is coded again with a lower quality (index + 8)
 * instead of the vendor's assert at the end of its range.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "core/thread_name.h"
#include "t32/openimp_t32_fs.h"
#include "t32/openimp_t32_vpu.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_encoder.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ENC"

#define T32_ENC_GROUPS      6
#define T32_ENC_CHNS        9
#define T32_ENC_MAX_STREAMS 8
#define T32_ENC_DEF_STREAMS 3
#define T32_JPEG_DEF_QP     40          /* vendor: initial QP out of 0..98 */
#define T32_JPEG_MAX_QP     98
#define T32_JPEG_RETRY_STEP 8           /* quality index step after an over-limit picture */
#define T32_JPEG_BS_1080P   512000u     /* vendor default stream buffer / limit at 1920x1080 */
#define T32_JPEG_BS_MIN     65536u
#define T32_JPEG_VPU_ID     (OI_T32_VPU_HELIX_ID | 1u)
#define T32_JPEG_VERSION_REG 0x132f0200u

#define ALIGN16(x) (((x) + 15u) & ~15u)

/* ITU-T T.81 Annex K tables K.1 / K.2, zigzag order */
static const uint8_t jpeg_base_luma[64] = {
    16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
    26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
    56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
};
static const uint8_t jpeg_base_chroma[64] = {
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

enum { SLOT_FREE, SLOT_READY, SLOT_USER };

struct t32_enc_slot {
    T32VpuBuf bs;
    IMPEncoderPack pack;
    uint32_t seq;
    int state;
};

struct t32_enc_chn {
    int created, registered, recv, quit;
    int grp;
    IMPEncoderCHNAttr attr;
    int qp;                             /* jpegQp 0..98 */
    T32VpuChannel vpu;
    T32VpuBuf job;
    struct t32_enc_slot slot[T32_ENC_MAX_STREAMS];
    int nslots, max_streams;
    uint32_t bs_size;
    int ready[T32_ENC_MAX_STREAMS];     /* FIFO of SLOT_READY slots */
    int rhead, rcount;
    uint32_t seq;
    int pending, busy;                  /* a frame of the group is waiting / in work */
    uint64_t rate_acc;                  /* frame-rate drop */
    int src_fs;
    uint32_t src_num, src_den;
    pthread_t thread;
    int thread_up;
    pthread_cond_t work, done;
    int pipefd[2];
    uint32_t frames, encoded, skipped, retries, errors;
    int size_logged;
};

struct t32_enc_grp {
    int created;
    IMPFrameInfo *frame;                /* the kept frame while channels code it */
    int fs_chn, refs;
};

static pthread_mutex_t enc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t32_enc_grp enc_grp[T32_ENC_GROUPS];
static struct t32_enc_chn enc_chn[T32_ENC_CHNS];
static uint32_t enc_jpeg_bs_size;       /* IMP_Encoder_SetJpegBsSize, 0 = default */

static struct t32_enc_chn *chn_get(int n)
{
    return n >= 0 && n < T32_ENC_CHNS && enc_chn[n].created ? &enc_chn[n] : NULL;
}

static int jpeg_qp_of(int qp)
{
    if (qp < 0)
        return T32_JPEG_DEF_QP;
    return qp > T32_JPEG_MAX_QP ? T32_JPEG_MAX_QP : qp;
}

/* OPENIMP_T32_VPU_TEST=1 (#291 test): jobs as START + WAIT_COMPLETE instead
 * of RUN, and the engine's version register read once per channel
 * (WOR_VPU_REG); the default path is the vendor's (RUN only) */
static int vpu_test_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *v = getenv("OPENIMP_T32_VPU_TEST");
        mode = v && v[0] == '1';
    }
    return mode;
}

/* ---- the JPEG job --------------------------------------------------------- */
static void jpeg_fill_job(struct t32_enc_chn *e, const IMPFrameInfo *f, const T32VpuBuf *bs,
                          uint32_t index)
{
    T32IocJpegRegInfo *j = t32_vpu_buf_va(&e->job);
    uint32_t w = f->width, h = f->height, stride = ALIGN16(w), i;

    memset(j, 0, T32_JPEG_JOB_BYTES);
    for (i = 0; i < 16; i++) {
        j->qmem[i] = (uint32_t)jpeg_base_luma[4 * i] | (uint32_t)jpeg_base_luma[4 * i + 1] << 8 |
                     (uint32_t)jpeg_base_luma[4 * i + 2] << 16 | (uint32_t)jpeg_base_luma[4 * i + 3] << 24;
        j->qmem[16 + i] = (uint32_t)jpeg_base_chroma[4 * i] | (uint32_t)jpeg_base_chroma[4 * i + 1] << 8 |
                          (uint32_t)jpeg_base_chroma[4 * i + 2] << 16 | (uint32_t)jpeg_base_chroma[4 * i + 3] << 24;
    }
    j->jpeg_ctrl = 1u << 4 | 1u << 5 | 1u << 6 |       /* U: DC/AC/quant table 1 */
                   1u << 8 | 1u << 9 | 1u << 10 |       /* V: the same */
                   1u << 16 |                           /* limit_bs_len enabled */
                   (index & 0x7fu) << 18 |              /* quality index */
                   1u << 25 | 1u << 26;                 /* frame end, over limit */
    j->nmcu = (ALIGN16(w) / 16u) * (ALIGN16(h) / 16u) - 1u;
    j->frm_info = h << 16 | w;
    j->stride = stride;
    j->y_addr = f->phyAddr;
    j->c_addr = f->phyAddr + stride * ALIGN16(h);       /* NV12 as FrameSource lays it out */
    j->bs_addr = t32_vpu_buf_pa(bs);
    j->pixel_num = ALIGN16(w) * ALIGN16(h) * 3u / 2u;
    j->limit_bs_len = bs->size;
}

/* one picture into a free slot: 0 with the length, -1 dropped */
static int jpeg_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s,
                       uint32_t *len)
{
    uint32_t index = (uint32_t)(T32_JPEG_MAX_QP - e->qp);
    T32VpuResult r;

    for (;;) {
        jpeg_fill_job(e, f, &s->bs, index);
        OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&e->job), T32_JPEG_JOB_BYTES, OI_T32_VPU_FLUSH_WBACK);
        if (vpu_test_mode()) {          /* the two-step path: START, WAIT_COMPLETE */
            if (OpenIMP_T32_VpuStart(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job)) != 0 ||
                OpenIMP_T32_VpuWait(&e->vpu, &r) != 0)
                return -1;
        } else if (OpenIMP_T32_VpuRun(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job), &r) != 0) {
            return -1;
        }
        if (r.rc == 0 && r.output_len > 0 && r.output_len <= s->bs.size) {
            OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&s->bs), r.output_len, OI_T32_VPU_FLUSH_INV);
            *len = r.output_len;
            return 0;
        }
        if (r.rc != OI_T32_VPU_RET_JPEG_BSLESS || index >= T32_JPEG_MAX_QP) {
            IMP_LOG_ERR(TAG, "JPEG job: result 0x%lx status 0x%x length %u", r.rc, r.status,
                        r.output_len);
            return -1;
        }
        index = index + T32_JPEG_RETRY_STEP > T32_JPEG_MAX_QP ? T32_JPEG_MAX_QP :
                index + T32_JPEG_RETRY_STEP;
        e->retries++;
        IMP_LOG_WARN(TAG, "picture over the %u byte buffer, again at quality index %u",
                     s->bs.size, index);
    }
}

/* ---- channel thread ------------------------------------------------------- */
static int slot_free_locked(struct t32_enc_chn *e)
{
    int i;

    for (i = 0; i < e->nslots; i++)
        if (e->slot[i].state == SLOT_FREE)
            return i;
    return -1;
}

static void *enc_thread(void *arg)
{
    struct t32_enc_chn *e = arg;
    openimp_thread_setnamef("oimp-jpeg-%d", (int)(e - enc_chn));
    pthread_mutex_lock(&enc_lock);
    for (;;) {
        struct t32_enc_grp *g;
        IMPFrameInfo *frame;
        int fs, si, ok;
        uint32_t len = 0;

        while (!e->quit && !e->pending)
            pthread_cond_wait(&e->work, &enc_lock);
        if (e->quit)
            break;
        g = &enc_grp[e->grp];
        frame = g->frame;
        fs = g->fs_chn;
        e->pending = 0;
        e->busy = 1;
        si = slot_free_locked(e);       /* reserved by the sink */
        pthread_mutex_unlock(&enc_lock);

        ok = si >= 0 && frame && jpeg_encode(e, frame, &e->slot[si], &len) == 0;

        pthread_mutex_lock(&enc_lock);
        if (ok) {
            struct t32_enc_slot *s = &e->slot[si];

            memset(&s->pack, 0, sizeof(s->pack));
            s->pack.phyAddr = t32_vpu_buf_pa(&s->bs);
            s->pack.virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&s->bs);
            s->pack.length = len;
            s->pack.timestamp = frame->timeStamp;
            s->pack.frameEnd = 1;
            s->seq = e->seq++;
            s->state = SLOT_READY;
            e->ready[(e->rhead + e->rcount) % T32_ENC_MAX_STREAMS] = si;
            e->rcount++;
            e->encoded++;
            if (write(e->pipefd[1], "s", 1) != 1)
                IMP_LOG_WARN(TAG, "stream notify pipe full");
        } else {
            e->errors++;
        }
        e->busy = 0;
        if (--g->refs == 0) {
            g->frame = NULL;
            pthread_mutex_unlock(&enc_lock);
            OpenIMP_FrameSource_ReleaseSinkFrame(fs, frame);
            pthread_mutex_lock(&enc_lock);
        }
        pthread_cond_broadcast(&e->done);
    }
    pthread_mutex_unlock(&enc_lock);
    return NULL;
}

/* frame-rate drop: the channel rate out of the source rate */
static int rate_pass_locked(struct t32_enc_chn *e)
{
    uint64_t num = e->attr.rcAttr.outFrmRate.frmRateNum, den = e->attr.rcAttr.outFrmRate.frmRateDen;
    uint64_t step, full;

    if (!num || !den || !e->src_num || !e->src_den)
        return 1;
    step = num * e->src_den;            /* channel fps x source fps' denominators */
    full = (uint64_t)e->src_num * den;
    if (step >= full)
        return 1;
    e->rate_acc += step;
    if (e->rate_acc < full)
        return 0;
    e->rate_acc -= full;
    return 1;
}

/* FrameSource sink of an encoder group (runs in the channel's frame thread) */
static int enc_sink(void *ctx, int fs_chn, IMPFrameInfo *frame)
{
    int grp = (int)(intptr_t)ctx, i, refs = 0, need_rate = 0;
    struct t32_enc_grp *g = &enc_grp[grp];

    pthread_mutex_lock(&enc_lock);
    for (i = 0; i < T32_ENC_CHNS; i++) {
        struct t32_enc_chn *e = &enc_chn[i];
        if (e->created && e->registered && e->grp == grp && e->recv && e->src_fs != fs_chn)
            need_rate = 1;
    }
    pthread_mutex_unlock(&enc_lock);
    if (need_rate) {                    /* source rate for the drop, outside the lock */
        IMPFSChnAttr fa;
        if (IMP_FrameSource_GetChnAttr(fs_chn, &fa) == 0) {
            pthread_mutex_lock(&enc_lock);
            for (i = 0; i < T32_ENC_CHNS; i++) {
                struct t32_enc_chn *e = &enc_chn[i];
                if (e->created && e->registered && e->grp == grp) {
                    e->src_fs = fs_chn;
                    e->src_num = fa.outFrmRateNum;
                    e->src_den = fa.outFrmRateDen;
                }
            }
            pthread_mutex_unlock(&enc_lock);
        }
    }

    pthread_mutex_lock(&enc_lock);
    if (!g->created || g->frame) {      /* still coding the last one: drop */
        for (i = 0; i < T32_ENC_CHNS; i++)
            if (enc_chn[i].created && enc_chn[i].registered && enc_chn[i].grp == grp &&
                enc_chn[i].recv)
                enc_chn[i].skipped++;
        pthread_mutex_unlock(&enc_lock);
        return 0;
    }
    for (i = 0; i < T32_ENC_CHNS; i++) {
        struct t32_enc_chn *e = &enc_chn[i];

        if (!e->created || !e->registered || e->grp != grp || !e->recv)
            continue;
        e->frames++;
        if (!rate_pass_locked(e))
            continue;
        if (frame->width != e->attr.encAttr.picWidth || frame->height != e->attr.encAttr.picHeight) {
            if (!e->size_logged++)
                IMP_LOG_ERR(TAG, "chn %d: frame %ux%u, channel %ux%u (no scaling in the encoder)",
                            i, frame->width, frame->height, e->attr.encAttr.picWidth,
                            e->attr.encAttr.picHeight);
            e->skipped++;
            continue;
        }
        if (e->pending || e->busy || slot_free_locked(e) < 0) {
            e->skipped++;               /* no free stream buffer: GetStream is behind */
            continue;
        }
        e->pending = 1;
        refs++;
        pthread_cond_signal(&e->work);
    }
    if (refs) {
        g->frame = frame;
        g->fs_chn = fs_chn;
        g->refs = refs;
    }
    pthread_mutex_unlock(&enc_lock);
    return refs ? 1 : 0;
}

/* ---- groups --------------------------------------------------------------- */
int IMP_Encoder_CreateGroup(int encGroup)
{
    int rc;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    if (enc_grp[encGroup].created) {
        pthread_mutex_unlock(&enc_lock);
        IMP_LOG_ERR(TAG, "group %d exists", encGroup);
        return -1;
    }
    memset(&enc_grp[encGroup], 0, sizeof(enc_grp[encGroup]));
    enc_grp[encGroup].created = 1;
    pthread_mutex_unlock(&enc_lock);
    rc = OpenIMP_FrameSource_RegisterSink(DEV_ID_ENC, encGroup, enc_sink, (void *)(intptr_t)encGroup);
    if (rc) {
        pthread_mutex_lock(&enc_lock);
        enc_grp[encGroup].created = 0;
        pthread_mutex_unlock(&enc_lock);
    }
    return rc;
}

int IMP_Encoder_DestroyGroup(int encGroup)
{
    int i;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    if (!enc_grp[encGroup].created) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    for (i = 0; i < T32_ENC_CHNS; i++)
        if (enc_chn[i].created && enc_chn[i].registered && enc_chn[i].grp == encGroup) {
            pthread_mutex_unlock(&enc_lock);
            IMP_LOG_ERR(TAG, "group %d: channel %d still registered", encGroup, i);
            return -1;
        }
    enc_grp[encGroup].created = 0;
    pthread_mutex_unlock(&enc_lock);
    OpenIMP_FrameSource_RegisterSink(DEV_ID_ENC, encGroup, NULL, NULL);
    return 0;
}

/* ---- channel attributes ----------------------------------------------------- */
int IMP_Encoder_SetDefaultParam(IMPEncoderChnAttr *attr, IMPEncoderProfile profile,
                                IMPEncoderRcMode rcMode, int width, int height,
                                int fpsNum, int fpsDen, int gopLen,
                                int bufSize, int iInitialQP, int bitrate)
{
    IMPEncoderAttrRcMode *rc;

    if (!attr || !fpsDen)
        return -1;
    memset(attr, 0, sizeof(*attr));
    attr->encAttr.enType = (IMPPayloadType)(((uint32_t)profile >> 24) & 0xffu);
    attr->encAttr.bufSize = (uint32_t)bufSize;
    attr->encAttr.profile = (uint32_t)profile;
    attr->encAttr.picWidth = (uint16_t)width;
    attr->encAttr.picHeight = (uint16_t)height;
    attr->rcAttr.outFrmRate.frmRateNum = (uint32_t)fpsNum;
    attr->rcAttr.outFrmRate.frmRateDen = (uint32_t)fpsDen;
    attr->rcAttr.maxGop = (uint32_t)gopLen;
    rc = &attr->rcAttr.attrRcMode;
    rc->rcMode = rcMode;
    if (attr->encAttr.enType == PT_JPEG) {
        if (rcMode != IMP_ENC_RC_MODE_FIXQP) {
            IMP_LOG_ERR(TAG, "JPEG takes FIXQP only");
            return -1;
        }
        rc->attrJPEGFixQp.qp = (uint32_t)iInitialQP;  /* kept as given, -1 too */
        return 0;
    }
    /* H.264 / H.265 (HERA, later issue): the basic fields */
    switch (rcMode) {
    case IMP_ENC_RC_MODE_FIXQP:
        rc->attrH264FixQp.IQp = rc->attrH264FixQp.PQp = rc->attrH264FixQp.BQp = (uint32_t)iInitialQP;
        break;
    case IMP_ENC_RC_MODE_CBR:
        rc->attrH264Cbr.outBitRate = (uint32_t)bitrate;
        rc->attrH264Cbr.initialQp = (uint8_t)iInitialQP;
        rc->attrH264Cbr.maxQp = 48;
        rc->attrH264Cbr.minQp = 15;
        break;
    default:
        rc->attrH264Vbr.maxBitRate = (uint32_t)bitrate;
        rc->attrH264Vbr.initialQp = (uint8_t)iInitialQP;
        rc->attrH264Vbr.maxQp = 48;
        rc->attrH264Vbr.minQp = 15;
        break;
    }
    return 0;
}

static uint32_t jpeg_bs_size(const IMPEncoderCHNAttr *a)
{
    uint64_t area = (uint64_t)a->encAttr.picWidth * a->encAttr.picHeight, size;

    if (a->encAttr.bufSize)
        return (a->encAttr.bufSize + 4095u) & ~4095u;
    if (enc_jpeg_bs_size)
        return (enc_jpeg_bs_size + 4095u) & ~4095u;
    size = (T32_JPEG_BS_1080P * area + 1920u * 1080u - 1u) / (1920u * 1080u);
    size = (size + 4095u) & ~(uint64_t)4095u;
    return size < T32_JPEG_BS_MIN ? T32_JPEG_BS_MIN : (uint32_t)size;
}

static void chn_free_bufs(struct t32_enc_chn *e)
{
    int i;

    for (i = 0; i < e->nslots; i++)
        OpenIMP_T32_VpuBufFree(&e->slot[i].bs);
    e->nslots = 0;
    e->rhead = e->rcount = 0;
}

static int chn_alloc_bufs(struct t32_enc_chn *e, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        T32VpuBuf *b = &e->slot[i].bs;

        e->slot[i].state = SLOT_FREE;
        if (OpenIMP_T32_VpuBufAlloc(b, e->bs_size, "t32_jpeg_bs") != 0) {
            e->nslots = i;
            chn_free_bufs(e);
            return -1;
        }
        /* the CPU never writes a stream buffer again: no dirty line may
         * land on the engine's output later */
        DMA_RmemFlushCache(t32_vpu_buf_va(b), b->size, 1);
        e->nslots = i + 1;
    }
    return 0;
}

int IMP_Encoder_CreateChn(int encChn, IMPEncoderCHNAttr *attr)
{
    struct t32_enc_chn *e;
    uint32_t version = 0;

    if (encChn < 0 || encChn >= T32_ENC_CHNS || !attr)
        return -1;
    if (attr->encAttr.enType != PT_JPEG) {
        IMP_LOG_ERR(TAG, "chn %d: payload %d: H.264/H.265 on HERA are not implemented on T32 yet",
                    encChn, (int)attr->encAttr.enType);
        return -1;
    }
    if (!attr->encAttr.picWidth || !attr->encAttr.picHeight ||
        attr->encAttr.picWidth > 8192 || attr->encAttr.picHeight > 8192) {
        IMP_LOG_ERR(TAG, "chn %d: picture %ux%u", encChn, attr->encAttr.picWidth, attr->encAttr.picHeight);
        return -1;
    }
    if (attr->encAttr.crop.enable)
        IMP_LOG_WARN(TAG, "chn %d: crop is not implemented on T32 yet, ignored", encChn);
    e = &enc_chn[encChn];
    pthread_mutex_lock(&enc_lock);
    if (e->created) {
        pthread_mutex_unlock(&enc_lock);
        IMP_LOG_ERR(TAG, "chn %d exists", encChn);
        return -1;
    }
    memset(e, 0, sizeof(*e));
    e->created = 1;                     /* reserved; the rest outside the lock */
    e->attr = *attr;
    e->qp = jpeg_qp_of((int)attr->rcAttr.attrRcMode.attrJPEGFixQp.qp);
    e->max_streams = T32_ENC_DEF_STREAMS;
    e->bs_size = jpeg_bs_size(attr);
    e->pipefd[0] = e->pipefd[1] = -1;
    e->src_fs = -1;
    pthread_mutex_unlock(&enc_lock);

    if (OpenIMP_T32_VpuOpen() != 0)
        goto fail_open;
    if (OpenIMP_T32_VpuRequest(&e->vpu, OI_T32_VPU_CODEC_JPEGENC) != 0)
        goto fail_req;
    if (OpenIMP_T32_VpuBufAlloc(&e->job, T32_JPEG_JOB_BYTES, "t32_jpeg_job") != 0 ||
        chn_alloc_bufs(e, e->max_streams) != 0)
        goto fail_bufs;
    if (pipe2(e->pipefd, O_CLOEXEC | O_NONBLOCK) != 0)
        goto fail_bufs;
    pthread_cond_init(&e->work, NULL);
    pthread_cond_init(&e->done, NULL);
    if (pthread_create(&e->thread, NULL, enc_thread, e) != 0)
        goto fail_thread;
    e->thread_up = 1;
    if (vpu_test_mode() && OpenIMP_T32_VpuReadReg(T32_JPEG_VERSION_REG, &version) == 0)
        IMP_LOG_INFO(TAG, "JPEG engine version register 0x%08x", version);
    IMP_LOG_INFO(TAG, "chn %d: JPEG %ux%u jpegQp %d, %d stream buffers of %u bytes, vpu channel %u",
                 encChn, attr->encAttr.picWidth, attr->encAttr.picHeight, e->qp, e->nslots,
                 e->bs_size, e->vpu.node.channel_id);
    return 0;

fail_thread:
    pthread_cond_destroy(&e->work);
    pthread_cond_destroy(&e->done);
    close(e->pipefd[0]);
    close(e->pipefd[1]);
fail_bufs:
    chn_free_bufs(e);
    OpenIMP_T32_VpuBufFree(&e->job);
    OpenIMP_T32_VpuRelease(&e->vpu);
fail_req:
    OpenIMP_T32_VpuClose();
fail_open:
    pthread_mutex_lock(&enc_lock);
    e->created = 0;
    pthread_mutex_unlock(&enc_lock);
    return -1;
}

static void chn_stop_locked(struct t32_enc_chn *e)
{
    e->recv = 0;
    while (e->pending || e->busy)
        pthread_cond_wait(&e->done, &enc_lock);
}

int IMP_Encoder_DestroyChn(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (!e) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    chn_stop_locked(e);
    e->registered = 0;
    e->quit = 1;
    pthread_cond_signal(&e->work);
    pthread_mutex_unlock(&enc_lock);
    if (e->thread_up)
        pthread_join(e->thread, NULL);
    chn_free_bufs(e);
    OpenIMP_T32_VpuBufFree(&e->job);
    OpenIMP_T32_VpuRelease(&e->vpu);
    OpenIMP_T32_VpuClose();
    close(e->pipefd[0]);
    close(e->pipefd[1]);
    pthread_cond_destroy(&e->work);
    pthread_cond_destroy(&e->done);
    IMP_LOG_INFO(TAG, "chn %d off: %u frames, %u coded, %u skipped, %u over-limit retries, %u errors",
                 encChn, e->frames, e->encoded, e->skipped, e->retries, e->errors);
    pthread_mutex_lock(&enc_lock);
    memset(e, 0, sizeof(*e));
    pthread_mutex_unlock(&enc_lock);
    return 0;
}

int IMP_Encoder_GetChnAttr(int encChn, IMPEncoderChnAttr *attr)
{
    struct t32_enc_chn *e;

    if (!attr)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *attr = e->attr;
        attr->rcAttr.attrRcMode.attrJPEGFixQp.qp = (uint32_t)e->qp;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetChnEncType(int encChn, IMPPayloadType *encType)
{
    struct t32_enc_chn *e;

    if (!encType)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *encType = e->attr.encAttr.enType;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetJpegQp(int encChn, int jpegQp)
{
    struct t32_enc_chn *e;

    if (jpegQp < 0 || jpegQp > T32_JPEG_MAX_QP)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        e->qp = jpegQp;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetJpegQp(int encChn, int *jpegQp)
{
    struct t32_enc_chn *e;

    if (!jpegQp)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *jpegQp = e->qp;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetJpegBsSize(uint32_t jpegBsSize)
{
    enc_jpeg_bs_size = jpegBsSize;
    return 0;
}

int IMP_Encoder_SetChnAttrRcMode(int encChn, IMPEncoderAttrRcMode *rcMode)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (!rcMode)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && rcMode->rcMode == IMP_ENC_RC_MODE_FIXQP) {
        e->attr.rcAttr.attrRcMode = *rcMode;
        e->qp = jpeg_qp_of((int)rcMode->attrJPEGFixQp.qp);
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetChnAttrRcMode(int encChn, IMPEncoderAttrRcMode *rcMode)
{
    struct t32_enc_chn *e;

    if (!rcMode)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *rcMode = e->attr.rcAttr.attrRcMode;
        rcMode->attrJPEGFixQp.qp = (uint32_t)e->qp;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetChnFrmRate(int encChn, const IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (!rate || !rate->frmRateNum || !rate->frmRateDen)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        e->attr.rcAttr.outFrmRate = *rate;
        e->rate_acc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetChnFrmRate(int encChn, IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (!rate)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *rate = e->attr.rcAttr.outFrmRate;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetMaxStreamCnt(int encChn, int cnt)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (cnt < 1 || cnt > T32_ENC_MAX_STREAMS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && !e->recv && !e->busy && !e->pending && !e->rcount) {
        int i, held = 0;
        for (i = 0; i < e->nslots; i++)
            held |= e->slot[i].state != SLOT_FREE;
        if (!held) {
            chn_free_bufs(e);
            e->max_streams = cnt;
            rc = chn_alloc_bufs(e, cnt);
        }
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetMaxStreamCnt(int encChn, int *cnt)
{
    struct t32_enc_chn *e;

    if (!cnt)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *cnt = e->nslots;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/* ---- registration and receiving ---------------------------------------------- */
int IMP_Encoder_RegisterChn(int encGroup, int encChn)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && !e->registered && enc_grp[encGroup].created) {
        e->grp = encGroup;
        e->registered = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "RegisterChn(%d, %d) failed", encGroup, encChn);
    return rc;
}

int IMP_Encoder_UnRegisterChn(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->registered) {
        chn_stop_locked(e);
        e->registered = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_StartRecvPic(int encChn)
{
    struct t32_enc_chn *e;
    int rc = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->registered) {
        e->recv = 1;
        e->rate_acc = 0;
        e->src_fs = -1;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_StopRecvPic(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        chn_stop_locked(e);
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/* ---- streams ------------------------------------------------------------------ */
static int wait_ready_locked(struct t32_enc_chn *e, int64_t timeout_ms)
{
    struct timespec t;

    if (e->rcount)
        return 0;
    if (timeout_ms == 0)
        return -1;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += timeout_ms / 1000;
        t.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000L;
        }
    }
    while (!e->rcount && e->created && !e->quit) {
        if (timeout_ms < 0)
            pthread_cond_wait(&e->done, &enc_lock);
        else if (pthread_cond_timedwait(&e->done, &enc_lock, &t) == ETIMEDOUT)
            break;
    }
    return e->rcount ? 0 : -1;
}

int IMP_Encoder_PollingStream(int encChn, uint32_t timeoutMsec)
{
    struct t32_enc_chn *e;
    int rc = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        rc = wait_ready_locked(e, (int64_t)timeoutMsec);
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetStream(int encChn, IMPEncoderStream *stream, int block)
{
    struct t32_enc_chn *e;
    struct t32_enc_slot *s;
    char c;
    int si;

    if (!stream)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (!e || wait_ready_locked(e, block ? -1 : 0) != 0) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    si = e->ready[e->rhead];
    e->rhead = (e->rhead + 1) % T32_ENC_MAX_STREAMS;
    e->rcount--;
    s = &e->slot[si];
    s->state = SLOT_USER;
    if (read(e->pipefd[0], &c, 1) != 1)
        IMP_LOG_DBG(TAG, "stream notify pipe empty");
    memset(stream, 0, sizeof(*stream));
    stream->pack = &s->pack;
    stream->packCount = 1;
    stream->seq = s->seq;
    stream->refType = IMP_Encoder_FSTYPE_IDR;
    pthread_mutex_unlock(&enc_lock);
    return 0;
}

int IMP_Encoder_ReleaseStream(int encChn, IMPEncoderStream *stream)
{
    struct t32_enc_chn *e;
    int i, rc = -1;

    if (!stream || !stream->pack)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    for (i = 0; e && i < e->nslots; i++)
        if (stream->pack == &e->slot[i].pack && e->slot[i].state == SLOT_USER) {
            e->slot[i].state = SLOT_FREE;
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&enc_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "ReleaseStream(%d): not a stream of this channel", encChn);
    return rc;
}

int IMP_Encoder_FlushStream(int encChn)
{
    struct t32_enc_chn *e;
    char c;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    while (e && e->rcount) {
        e->slot[e->ready[e->rhead]].state = SLOT_FREE;
        e->rhead = (e->rhead + 1) % T32_ENC_MAX_STREAMS;
        e->rcount--;
        if (read(e->pipefd[0], &c, 1) != 1)
            break;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_Query(int encChn, IMPEncoderCHNStat *stat)
{
    struct t32_enc_chn *e;
    int i;

    if (!stat)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        memset(stat, 0, sizeof(*stat));
        stat->registered = e->registered;
        stat->leftPics = (uint32_t)(e->pending + e->busy);
        for (i = 0; i < e->rcount; i++)
            stat->leftStreamBytes += e->slot[e->ready[(e->rhead + i) % T32_ENC_MAX_STREAMS]].pack.length;
        stat->leftStreamFrames = (uint32_t)e->rcount;
        stat->curPacks = e->rcount ? 1 : 0;
        stat->work_done = e->recv ? 0 : 1;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetFd(int encChn)
{
    struct t32_enc_chn *e;
    int fd = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        fd = e->pipefd[0];              /* readable while a stream is ready */
    pthread_mutex_unlock(&enc_lock);
    return fd;
}

int IMP_Encoder_RequestIDR(int encChn)
{
    int ok;

    pthread_mutex_lock(&enc_lock);
    ok = chn_get(encChn) != NULL;       /* every JPEG picture stands alone */
    pthread_mutex_unlock(&enc_lock);
    return ok ? 0 : -1;
}
