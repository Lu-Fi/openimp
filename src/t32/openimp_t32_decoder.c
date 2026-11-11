/*
 * OpenIMP T32 IMP_Decoder (#301, planned; not a release gate): the JPEG
 * decoder of the JPEG v1.2 engine ("helix0") through the kernel's
 * /dev/soc_vpu (openimp_t32_vpu.c), as the vendor libimp 2.0.0 does it.
 *
 * Vendor reference: libimp 2.0.0 (the T32Pro / T33 SDK; the T32 PRJ007
 * libimp 2.2.0 has no IMP_Decoder at all), ijpegd + hwicodec_pf_jpegd_t32p,
 * RE in audit/issue301/dec and traced in QEMU (tests/t32/
 * vendor-2.0.0-ioctl-seq-dec.txt, vendor-2.0.0-dec-job.txt):
 *
 * - channel 0 only, PT_JPEG only, output pixelFormat one of the four
 *   PIX_FMT_BAYER_*8 (the engine's decode_mode: RGGB 0, BGGR 1, GRBG 2,
 *   GBRG 3); YUV420P / NV12 / NV21 pass the format switch but are refused
 *   by ijpegd, every other format by the switch.  maxWidth >= 256,
 *   maxHeight >= 16.
 * - CreateChn: CHANNEL_REQ codecdir JPEGDEC (0x10001), nrKeepStream nodes,
 *   each an rmem picture buffer of align16(2 * maxHeight) * align128(maxWidth)
 *   bytes and an input buffer of the same size; a decode thread and a tick
 *   thread that paces it at frmRateNum / frmRateDen while receiving.
 * - SendStreamTimeout copies the file into a free node (waiting up to the
 *   timeout for one) and queues it; the decode thread parses the headers
 *   (DQT, SOF, DRI, SOS), copies the scan as it is in the file (stuffing,
 *   RSTn) and appends an EOI, builds the job and runs it:
 *   FLUSH_CACHE job (256 B, write back), FLUSH_CACHE scan (write back),
 *   CHANNEL_RUN on HELIX|1, FLUSH_CACHE picture (invalidate).
 * - job (jpeg_reg_info of the 2.0.0 kernel, T32P_JPEGD_SliceInit): qmem =
 *   quantization tables 0 and 1 in file (zigzag) order, four to a word;
 *   jpeg_ctrl 0x2000770; geometry from the channel attributes, not from the
 *   file: W = align16(maxWidth), H = align8(maxHeight), nmcu =
 *   (W / 16) * H / 8 - 1 (16x8 MCUs, 4:2:2), frm_info H << 16 | W, stride
 *   align128(W), pixel_num W * H * 2; marker_en = (restart interval - 1 != 0),
 *   marker_num = restart interval - 1; decode_mode as above.  The engine
 *   writes 2 * H lines of W bytes, align128(W) apart.
 * - the frame: width align128(file width), height align8(file height) of the
 *   last file with a SOF (0 x 0 before the first), pixfmt the channel's,
 *   size the buffer's, the stream's timestamp.  A decode that fails still
 *   delivers its (stale) frame, like the vendor; a file without a scan is
 *   not run on the engine here (the vendor runs it on leftovers).
 * - a refused CreateChn leaves no channel behind (the vendor's stays half
 *   set up: every later CreateChn fails, GetFrame crashes).
 *
 * The vendor decodes into a buffer of its own and copies it into the frame
 * (memcpy of the engine's output length); here the engine writes the frame
 * buffer directly (same bytes, one copy less).
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>

#include "imp_log_int.h"
#include "core/thread_name.h"
#include "t32/openimp_t32_jpegd.h"
#include "t32/openimp_t32_vpu.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_decoder.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-DEC"
#define DEC_VPU_ID (OI_T32_VPU_HELIX_ID | 1u)

/* vendor imp_decoder.h (2.0.0) */
typedef struct {
    int decType;                /* IMPPayloadType: PT_JPEG only */
    uint32_t maxWidth;
    uint32_t maxHeight;
    int pixelFormat;            /* PIX_FMT_BAYER_*8 */
    uint32_t nrKeepStream;
    uint32_t frmRateNum;
    uint32_t frmRateDen;
} T32DecoderChnAttr;

typedef struct {
    int i_payload;
    uint8_t *p_payload;
    int64_t timeStamp;
} T32DecoderStream;

_Static_assert(sizeof(T32DecoderChnAttr) == 28, "IMPDecoderCHNAttr");
_Static_assert(sizeof(T32DecoderStream) == 16, "IMPDecoderStream");

typedef struct DecNode {
    struct DecNode *next;
    IMPFrameInfo frame;         /* handed out by GetFrame */
    T32VpuBuf pic;              /* the engine's output = the frame's data */
    uint8_t *in;                /* the file, copied by SendStreamTimeout */
    uint32_t in_len;
    int64_t ts;
    int held;                   /* GetFrame'd, not yet released */
} DecNode;

typedef struct {
    DecNode *head, *tail;
} DecList;

typedef struct {
    int created;
    int receiving;
    T32DecoderChnAttr attr;
    uint32_t mode;              /* engine decode_mode */
    uint32_t w, h;              /* engine geometry: align16 / align8 of max */
    uint32_t buf_size;          /* align16(2 * maxHeight) * align128(maxWidth) */
    uint32_t file_w, file_h;    /* the last SOF's (0 before the first), as ijpegd keeps them */
    DecNode *nodes;
    DecList free_list, in_list, out_list;
    pthread_mutex_t lock;
    sem_t free_sem, in_sem, out_sem, tick_sem;
    pthread_t dec_thread, tick_thread;
    int threads;
    T32VpuChannel vpu;
    T32VpuBuf job, scan;
} T32DecChn;

static T32DecChn g_dec;
static pthread_mutex_t g_api_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static void list_push(DecList *l, DecNode *n)
{
    n->next = NULL;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
}

static DecNode *list_pop(T32DecChn *c, DecList *l)
{
    DecNode *n;

    pthread_mutex_lock(&c->lock);
    n = l->head;
    if (n) {
        l->head = n->next;
        if (!l->head)
            l->tail = NULL;
        n->next = NULL;
    }
    pthread_mutex_unlock(&c->lock);
    return n;
}

static void list_put(T32DecChn *c, DecList *l, DecNode *n)
{
    pthread_mutex_lock(&c->lock);
    list_push(l, n);
    pthread_mutex_unlock(&c->lock);
}

static int sem_wait_ms(sem_t *s, uint32_t ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (sem_timedwait(s, &ts) != 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* one node: parse, run the engine into the node's picture buffer */
static int dec_one(T32DecChn *c, DecNode *n)
{
    T32DecHeader hd;
    T32VpuResult r;
    uint32_t out_len;

    int rc = OpenIMP_T32_DecParse(n->in, n->in_len, &hd, t32_vpu_buf_va(&c->scan), c->scan.size);

    if (hd.sof) {
        c->file_w = hd.width;
        c->file_h = hd.height;
    }
    n->frame.width = align_up(c->file_w, 128);
    n->frame.height = align_up(c->file_h, 8);
    if (rc != 0) {                  /* the vendor runs the engine on it anyway */
        IMP_LOG_ERR(TAG, "not a baseline JPEG (%u bytes)", n->in_len);
        return -1;
    }
    if ((uint64_t)hd.width * hd.height * 3 / 2 > c->buf_size) {
        IMP_LOG_ERR(TAG, "%ux%u over the %u byte frame", hd.width, hd.height, c->buf_size);
        return -1;
    }
    OpenIMP_T32_DecBuildJob(t32_vpu_buf_va(&c->job), &hd, c->w, c->h, c->mode,
                            t32_vpu_buf_pa(&n->pic), t32_vpu_buf_pa(&c->scan));
    OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&c->job), T32_JPEG_JOB_BYTES, OI_T32_VPU_FLUSH_WBACK);
    OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&c->scan), hd.scan_len, OI_T32_VPU_FLUSH_WBACK);
    if (OpenIMP_T32_VpuRun(&c->vpu, DEC_VPU_ID, t32_vpu_buf_pa(&c->job), &r) != 0) {
        IMP_LOG_ERR(TAG, "decode: vpu channel run failed");
        return -1;
    }
    out_len = r.output_len;
    if (out_len < 1 || (c->w & 0x7f))
        out_len = align_up(2 * c->h, 16) * align_up(c->w, 128);
    if (out_len > c->buf_size)
        out_len = c->buf_size;
    OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&n->pic), out_len, OI_T32_VPU_FLUSH_INV);
    return 0;
}

static void *dec_thread(void *arg)
{
    T32DecChn *c = arg;

    openimp_thread_setname("T32-jpegd");
    for (;;) {
        DecNode *n;
        int pending = 0;

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        sem_getvalue(&c->in_sem, &pending);
        if (pending == 0)
            sem_wait(&c->tick_sem);
        else
            sem_trywait(&c->tick_sem);
        if (sem_wait_ms(&c->in_sem, 100) != 0)
            continue;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        n = list_pop(c, &c->in_list);
        if (!n)
            continue;
        if (dec_one(c, n) != 0)
            IMP_LOG_ERR(TAG, "ijpegd decode failed");
        n->frame.timeStamp = n->ts;
        list_put(c, &c->out_list, n);
        sem_post(&c->out_sem);
    }
    return NULL;
}

static void *tick_thread(void *arg)
{
    T32DecChn *c = arg;

    openimp_thread_setname("T32-jpegd-tick");
    for (;;) {
        if (c->receiving)
            sem_post(&c->tick_sem);
        /* the vendor's period; den 0 polls without delay there, 1 ms here */
        uint64_t us = (uint64_t)c->attr.frmRateDen * 1000000u / c->attr.frmRateNum;
        usleep((useconds_t)(us ? us : 1000));
    }
    return NULL;
}

static void chn_free(T32DecChn *c)
{
    if (c->threads) {
        pthread_cancel(c->tick_thread);
        pthread_join(c->tick_thread, NULL);
        pthread_cancel(c->dec_thread);
        pthread_join(c->dec_thread, NULL);
        sem_destroy(&c->tick_sem);
        sem_destroy(&c->out_sem);
        sem_destroy(&c->in_sem);
        sem_destroy(&c->free_sem);
        pthread_mutex_destroy(&c->lock);
    }
    for (uint32_t i = 0; c->nodes && i < c->attr.nrKeepStream; i++) {
        OpenIMP_T32_VpuBufFree(&c->nodes[i].pic);
        free(c->nodes[i].in);
    }
    free(c->nodes);
    if (c->vpu.requested)
        OpenIMP_T32_VpuRelease(&c->vpu);
    OpenIMP_T32_VpuBufFree(&c->scan);
    OpenIMP_T32_VpuBufFree(&c->job);
    if (c->created)
        OpenIMP_T32_VpuClose();
    memset(c, 0, sizeof(*c));
}

/* the vendor's IMPPixelFormat switch, then ijpegd's Bayer-only check */
static int dec_mode(int fmt, uint32_t *mode)
{
    switch (fmt) {
    case PIX_FMT_BAYER_RGGB8: *mode = 0; return 0;
    case PIX_FMT_BAYER_BGGR8: *mode = 1; return 0;
    case PIX_FMT_BAYER_GRBG8: *mode = 2; return 0;
    case PIX_FMT_BAYER_GBRG8: *mode = 3; return 0;
    default: return -1;
    }
}

int IMP_Decoder_CreateChn(int decChn, const T32DecoderChnAttr *attr)
{
    T32DecChn *c = &g_dec;
    uint32_t mode;

    if (decChn != 0 || !attr) {
        IMP_LOG_ERR(TAG, "CreateChn: invalid channel %d or attribute", decChn);
        return -1;
    }
    pthread_mutex_lock(&g_api_lock);
    if (c->created) {
        pthread_mutex_unlock(&g_api_lock);
        IMP_LOG_ERR(TAG, "CreateChn: channel %d exists", decChn);
        return -1;
    }
    if (attr->decType != PT_JPEG || dec_mode(attr->pixelFormat, &mode) != 0 ||
        attr->maxWidth < 256 || attr->maxHeight < 16 || attr->nrKeepStream == 0 ||
        attr->frmRateNum == 0 || attr->maxWidth > 8192 || attr->maxHeight > 8192 ||
        attr->nrKeepStream > 32) {
        pthread_mutex_unlock(&g_api_lock);
        IMP_LOG_ERR(TAG, "CreateChn: type %d format %d %ux%u keep %u rate %u/%u not supported",
                    attr->decType, attr->pixelFormat, attr->maxWidth, attr->maxHeight,
                    attr->nrKeepStream, attr->frmRateNum, attr->frmRateDen);
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->attr = *attr;
    c->mode = mode;
    c->w = align_up(attr->maxWidth, 16);
    c->h = align_up(attr->maxHeight, 8);
    c->buf_size = align_up(2 * attr->maxHeight, 16) * align_up(attr->maxWidth, 128);
    if (OpenIMP_T32_VpuOpen() != 0)
        goto fail_unlocked_open;
    c->created = 1;                 /* VpuClose in chn_free */
    if (OpenIMP_T32_VpuRequest(&c->vpu, OI_T32_VPU_CODEC_JPEGDEC) != 0 ||
        OpenIMP_T32_VpuBufAlloc(&c->job, T32_JPEG_JOB_BYTES, "dec-job") != 0 ||
        OpenIMP_T32_VpuBufAlloc(&c->scan, c->buf_size, "dec-scan") != 0)
        goto fail;
    c->nodes = calloc(attr->nrKeepStream, sizeof(*c->nodes));
    if (!c->nodes)
        goto fail;
    for (uint32_t i = 0; i < attr->nrKeepStream; i++) {
        DecNode *n = &c->nodes[i];

        if (OpenIMP_T32_VpuBufAlloc(&n->pic, c->buf_size, "dec-frame") != 0 ||
            !(n->in = malloc(c->buf_size)))
            goto fail;
        n->frame.width = c->w;
        n->frame.height = align_up(attr->maxHeight, 16);
        n->frame.pixfmt = (uint32_t)attr->pixelFormat;
        n->frame.size = c->buf_size;
        n->frame.phyAddr = t32_vpu_buf_pa(&n->pic);
        n->frame.virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&n->pic);
        list_push(&c->free_list, n);
    }
    if (pthread_mutex_init(&c->lock, NULL) != 0)
        goto fail;
    sem_init(&c->free_sem, 0, attr->nrKeepStream);
    sem_init(&c->in_sem, 0, 0);
    sem_init(&c->out_sem, 0, 0);
    sem_init(&c->tick_sem, 0, 0);
    if (pthread_create(&c->dec_thread, NULL, dec_thread, c) != 0) {
        sem_destroy(&c->tick_sem);
        sem_destroy(&c->out_sem);
        sem_destroy(&c->in_sem);
        sem_destroy(&c->free_sem);
        pthread_mutex_destroy(&c->lock);
        goto fail;
    }
    if (pthread_create(&c->tick_thread, NULL, tick_thread, c) != 0) {
        pthread_cancel(c->dec_thread);
        pthread_join(c->dec_thread, NULL);
        sem_destroy(&c->tick_sem);
        sem_destroy(&c->out_sem);
        sem_destroy(&c->in_sem);
        sem_destroy(&c->free_sem);
        pthread_mutex_destroy(&c->lock);
        goto fail;
    }
    c->threads = 1;
    pthread_mutex_unlock(&g_api_lock);
    IMP_LOG_INFO(TAG, "channel %d: %ux%u format %d, %u buffers of %u bytes", decChn,
                 attr->maxWidth, attr->maxHeight, attr->pixelFormat, attr->nrKeepStream, c->buf_size);
    return 0;
fail:
    chn_free(c);
fail_unlocked_open:
    memset(c, 0, sizeof(*c));
    pthread_mutex_unlock(&g_api_lock);
    IMP_LOG_ERR(TAG, "CreateChn: channel %d setup failed", decChn);
    return -1;
}

static T32DecChn *chn_get(int decChn, const char *fn)
{
    if (decChn != 0) {
        IMP_LOG_ERR(TAG, "%s: invalid channel %d", fn, decChn);
        return NULL;
    }
    if (!g_dec.created || !g_dec.threads) {
        IMP_LOG_ERR(TAG, "%s: channel %d not created", fn, decChn);
        return NULL;
    }
    return &g_dec;
}

int IMP_Decoder_DestroyChn(int decChn)
{
    T32DecChn *c;

    pthread_mutex_lock(&g_api_lock);
    if (!(c = chn_get(decChn, "DestroyChn"))) {
        pthread_mutex_unlock(&g_api_lock);
        return -1;
    }
    chn_free(c);
    pthread_mutex_unlock(&g_api_lock);
    return 0;
}

int IMP_Decoder_StartRecvPic(int decChn)
{
    T32DecChn *c = chn_get(decChn, "StartRecvPic");

    if (!c)
        return -1;
    pthread_mutex_lock(&c->lock);
    c->receiving = 1;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int IMP_Decoder_StopRecvPic(int decChn)
{
    T32DecChn *c = chn_get(decChn, "StopRecvPic");

    if (!c)
        return -1;
    pthread_mutex_lock(&c->lock);
    c->receiving = 0;
    pthread_mutex_unlock(&c->lock);
    return 0;
}

int IMP_Decoder_SendStreamTimeout(int decChn, T32DecoderStream *stream, uint32_t timeoutMsec)
{
    T32DecChn *c = chn_get(decChn, "SendStreamTimeout");
    DecNode *n;

    if (!c)
        return -1;
    if (!stream || !stream->p_payload || stream->i_payload <= 0 ||
        (uint32_t)stream->i_payload > c->buf_size) {
        IMP_LOG_ERR(TAG, "SendStreamTimeout: invalid stream (%d bytes, at most %u)",
                    stream ? stream->i_payload : -1, c->buf_size);
        return -1;
    }
    if (sem_wait_ms(&c->free_sem, timeoutMsec) != 0) {
        IMP_LOG_ERR(TAG, "SendStreamTimeout: no free buffer in %u ms", timeoutMsec);
        return -1;
    }
    if (!(n = list_pop(c, &c->free_list))) {
        IMP_LOG_ERR(TAG, "SendStreamTimeout: no free node");
        return -1;
    }
    memcpy(n->in, stream->p_payload, (size_t)stream->i_payload);
    n->in_len = (uint32_t)stream->i_payload;
    n->ts = stream->timeStamp;
    list_put(c, &c->in_list, n);
    sem_post(&c->in_sem);
    return 0;
}

int IMP_Decoder_PollingFrame(int decChn, uint32_t timeoutMsec)
{
    T32DecChn *c = chn_get(decChn, "PollingFrame");

    if (!c)
        return -1;
    if (sem_wait_ms(&c->out_sem, timeoutMsec) != 0) {
        IMP_LOG_DBG(TAG, "PollingFrame: no frame in %u ms", timeoutMsec);
        return -1;
    }
    return 0;
}

int IMP_Decoder_GetFrame(int decChn, IMPFrameInfo **frame)
{
    T32DecChn *c = chn_get(decChn, "GetFrame");
    DecNode *n;

    if (!c || !frame)
        return -1;
    if (!(n = list_pop(c, &c->out_list))) {
        IMP_LOG_ERR(TAG, "GetFrame: no decoded frame");
        return -1;
    }
    n->held = 1;
    *frame = &n->frame;
    return 0;
}

int IMP_Decoder_ReleaseFrame(int decChn, IMPFrameInfo *frame)
{
    T32DecChn *c = chn_get(decChn, "ReleaseFrame");
    DecNode *n = NULL;

    if (!c)
        return -1;
    for (uint32_t i = 0; frame && i < c->attr.nrKeepStream; i++)
        if (&c->nodes[i].frame == frame && c->nodes[i].held)
            n = &c->nodes[i];
    if (!n) {
        IMP_LOG_ERR(TAG, "ReleaseFrame: not a frame of GetFrame");
        return -1;
    }
    n->held = 0;
    list_put(c, &c->free_list, n);
    sem_post(&c->free_sem);
    return 0;
}
