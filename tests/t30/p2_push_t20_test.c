/*
 * p2_push_t20_test - the T10/T20 push model of the encoder (#896): the
 * capture worker publishes a frame and kicks the Encoder-N thread
 * (OpenIMP_FS_PushEncode), which encodes it into the stream ring and gives
 * back what nobody took.  src/t40/openimp_p2_encoder.c against a stub
 * codec and a fake VBM ready queue (tagged frames, FIFO like
 * kernel_interface.c):
 *
 *  - #916: a frame published while Encoder-N still encodes the previous
 *    one, and not kicked yet, survives that encode's give-back and is
 *    encoded after its kick;
 *  - #917: the push encode goes on while the application holds a stream
 *    (stream ring up to the codec's stream buffers less one);
 *  - #918: no give-back while another reader pulls the source
 *    (IMP_FrameSource_GetFrame next to the push encoder);
 *  - two encoder channels on two sources, one of them slow: no frame lost;
 *  - 20 StopRecvPic/StartRecvPic cycles while frames flow: no capture
 *    buffer left held, encoding resumes after each start;
 *  - #920 L2: Encoder-N runs at the normal policy whatever its creator's.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>
#include "dma_alloc.h"

int IMP_Encoder_SetChnColor2Grey(int, const IMPEncoderColor2GreyCfg *);
int IMP_Encoder_GetChnColor2Grey(int, IMPEncoderColor2GreyCfg *);
int IMP_Encoder_SetChnROI(int, const IMPEncoderROICfg *);
int IMP_Encoder_GetChnAttr(int, IMPEncoderCHNAttr *);
int IMP_Encoder_SetFisheyeEnableStatus(int, int);
int IMP_Encoder_GetFisheyeEnableStatus(int, int *);
int IMP_Encoder_SetChnDenoise(int, const IMPEncoderAttrDenoise *);
int IMP_Encoder_GetChnDenoise(int, IMPEncoderAttrDenoise *);
int IMP_Encoder_SetMbRC(int, int);
int IMP_Encoder_SetSuperFrameCfg(int, const IMPEncoderSuperFrmCfg *);
#if defined(PLATFORM_T20)
int IMP_Encoder_SetChnRcTrigLevel(int, float, int);
#endif
int IMP_Encoder_GetSuperFrameCfg(int, IMPEncoderSuperFrmCfg *);
int IMP_Encoder_SetH264TransCfg(int, const IMPEncoderH264TransCfg *);
int IMP_Encoder_SetQpgMode(int, const IMPEncoderQpgMode *);
int IMP_Encoder_GetQpgMode(int, IMPEncoderQpgMode *);
int IMP_Encoder_SetH265TransCfg(int, const IMPEncoderH265TransCfg *);
int IMP_Encoder_GetH265TransCfg(int, IMPEncoderH265TransCfg *);
int IMP_Encoder_SetJpegeQl(int, IMPEncoderJpegeQl *);
int IMP_Encoder_GetJpegeQl(int, IMPEncoderJpegeQl *);

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the rest of libimp ---- */
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{ (void)info; (void)size; (void)tag; return -1; }
int DMA_FreePhys(uint32_t phys) { (void)phys; return 0; }
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{ (void)virt; (void)size; (void)dir; return 0; }
int OpenIMP_HelixJpeg_Available(void) { return 0; }
int openimp_t23_persist_enabled(void) { return 0; }
int openimp_t23_persist_write(const char *a, const void *b, unsigned int c)
{ (void)a; (void)b; (void)c; return 0; }
int openimp_t31_osd_apply_ex(int group, void *frame, unsigned int flags)
{ (void)group; (void)frame; (void)flags; return 0; }

/* ---- stub codec ---- */
static int c2g_calls, c2g_value = -1;
static int mbrc_calls, mbrc_value = -1;
static int sf_calls;
static uint32_t sf_mode, sf_i, sf_p;

int AL_Codec_Encode_JpegSkipped(void *c) { (void)c; return 0; }
int AL_Codec_Encode_InsertUserData(void *c, const void *d, unsigned int s, unsigned int n, unsigned int m)
{ (void)c; (void)d; (void)s; (void)n; (void)m; return 0; }
int AL_Codec_Encode_SetJpegSkip(void *c, int a) { (void)c; (void)a; return 0; }
int AL_Codec_Encode_SetJpegSrcPitch(void *c, uint32_t p) { (void)c; (void)p; return 0; }
static int jql_calls, jql_en = -1;
static uint8_t jql_tab[128];
int AL_Codec_Encode_SetJpegQl(void *c, int a, const uint8_t *b)
{ (void)c; jql_calls++; jql_en = a; if (b) memcpy(jql_tab, b, sizeof(jql_tab)); return 0; }
#define STUB0(name) int name(void *codec) { (void)codec; return 0; }
#define STUB1(name, t) int name(void *codec, t a) { (void)codec; (void)a; return 0; }
STUB0(AL_Codec_Encode_RequestIDR)
STUB1(AL_Codec_Encode_SetEntropyMode, int)
STUB1(AL_Codec_Encode_SetGopLength, int)
int AL_Codec_Encode_GetGopLength(void *codec, uint32_t *gop) { (void)codec; (void)gop; return -1; }
int AL_Codec_Encode_GetRunningRate(void *codec, uint32_t *n, uint32_t *d) { (void)codec; (void)n; (void)d; return -1; }
STUB1(AL_Codec_Encode_SetQp, void *)
STUB1(AL_Codec_Encode_SetStreamBufferCount, int)
STUB1(AL_Codec_Encode_SetStreamBufferSize, int)
STUB1(AL_Codec_Encode_SetQpIPDelta, int)
STUB1(AL_Codec_Encode_SetRcParam, void *)
STUB1(AL_Codec_Encode_SetRcExtras, const void *)
STUB1(AL_Codec_Encode_SetFrameRate, void *)
STUB1(AL_Codec_Encode_SetGopParam, void *)
int AL_Codec_Encode_SetBitRate(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetQpBounds(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetDefaultParam(void *p) { (void)p; return 0; }
int AL_Codec_Encode_SetSameSceneGops(void *c, uint32_t g) { (void)c; (void)g; return 0; }
static int hs_calls, hs_type = -1, hs_m, hs_n, hs_scene, hs_black;
int AL_Codec_Encode_SetHSkip(void *c, uint32_t t, uint32_t m, uint32_t n, int sc, int bl)
{ (void)c; hs_calls++; hs_type = (int)t; hs_m = (int)m; hs_n = (int)n; hs_scene = sc; hs_black = bl; return 0; }
static int fe_flag = -1;
int AL_Codec_Encode_SetFisheye(void *c, int en)
{ (void)c; fe_flag = en; return 0; }
static int cr_calls = -1;
int AL_Codec_Encode_SetChangeRef(void *c, int en)
{ (void)c; cr_calls = en; return 0; }
#if defined(PLATFORM_T20)
static int sharp_calls, sharp_value, sharp_mode;
int AL_Codec_Encode_GetPictureQp(void *c, int *qp) { (void)c; *qp = 0; return 0; }
int IMP_ISP_Tuning_SetSharpness_internal(uint32_t v, int32_t m) { sharp_calls++; sharp_value = (int)v; sharp_mode = m; return 0; }
#endif
int AL_Codec_Encode_SetMbRC(void *c, int e) { (void)c; mbrc_calls++; mbrc_value = e; return 0; }
int AL_Codec_Encode_SetColor2Grey(void *c, int e) { (void)c; c2g_calls++; c2g_value = e; return 0; }
static uint8_t roi_entry[8][7];
static int roi_calls, chroma_calls, chroma_value;
int AL_Codec_Encode_SetRoi(void *c, uint32_t i, const uint8_t e[7])
{ (void)c; if (i >= 8u) return -1; roi_calls++; memcpy(roi_entry[i], e, 7); return 0; }
int AL_Codec_Encode_SetChromaQpOffset(void *c, int o)
{ (void)c; chroma_calls++; chroma_value = o; return 0; }
#if defined(PLATFORM_T21) && !defined(PLATFORM_T20)
static int chroma_applied;      /* what the encoder codes with now (the IDR picture takes chroma_value over) */
int AL_Codec_Encode_GetChromaQpOffset(void *c, int *o)
{ (void)c; *o = chroma_applied; return 0; }
#endif
int IMP_Encoder_SetChnCrop(int encChn, const IMPEncoderCropCfg *cfg);
int IMP_Encoder_GetChnCrop(int encChn, IMPEncoderCropCfg *cfg);
static int crop_calls, crop_en;
static uint32_t crop_v[4];
int AL_Codec_Encode_SetCrop(void *c, int e, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)c; crop_calls++; crop_en = e; crop_v[0] = x; crop_v[1] = y; crop_v[2] = w; crop_v[3] = h; return 0; }
int AL_Codec_Encode_GetCrop(void *c, int *e, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{ (void)c; *e = crop_en; *x = crop_v[0]; *y = crop_v[1]; *w = crop_v[2]; *h = crop_v[3]; return 0; }
static int trig_calls, trig_mode;
static float trig_level;
int AL_Codec_Encode_SetRcTrig(void *c, float l, int m)
{ (void)c; trig_calls++; trig_level = l; trig_mode = m; return 0; }
int AL_Codec_Encode_SetSuperFrame(void *c, uint32_t m, uint32_t i, uint32_t p)
{ (void)c; sf_calls++; sf_mode = m; sf_i = i; sf_p = p; return 0; }

int IMP_Encoder_CreateGroup(int);
int IMP_Encoder_RegisterChn(int, int);
int IMP_Encoder_StartRecvPic(int);
int IMP_Encoder_StopRecvPic(int);
int IMP_Encoder_PollingStream(int, uint32_t);
int IMP_Encoder_ReleaseStream(int, IMPEncoderStream *);
int OpenIMP_FS_PushEncode(int source_channel);

/* ---- fake VBM ready queue (2 sources) ---- */
#define SRCS 2
#define QMAX 8
typedef struct { void *f; unsigned int tag; } QE;
static pthread_mutex_t vq = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vcond = PTHREAD_COND_INITIALIZER;
static QE q[SRCS][QMAX];
static int qh[SRCS], qn[SRCS];
static unsigned int pub_tag[SRCS], ready_seq[SRCS];
static int held[SRCS], given_back[SRCS], released[SRCS], qdrop[SRCS];
static int old_expire_all;      /* the #896 "expire everything" call */
static int reader_active[SRCS];
static uint8_t fbuf[SRCS][QMAX][1024] __attribute__((aligned(16)));
static unsigned int fnext[SRCS];

static void publish(int s)
{
    pthread_mutex_lock(&vq);
    if (qn[s] == QMAX) {
        qh[s] = (qh[s] + 1) % QMAX;
        qn[s]--;
        qdrop[s]++;
    }
    q[s][(qh[s] + qn[s]) % QMAX].f = fbuf[s][fnext[s]++ % QMAX];
    q[s][(qh[s] + qn[s]) % QMAX].tag = ++pub_tag[s];
    qn[s]++;
    ready_seq[s]++;
    pthread_cond_broadcast(&vcond);
    pthread_mutex_unlock(&vq);
}

static int qlen(int s)
{
    int n;

    pthread_mutex_lock(&vq);
    n = qn[s];
    pthread_mutex_unlock(&vq);
    return n;
}

static int atomic_read(int *p)
{
    int v;

    pthread_mutex_lock(&vq);
    v = *p;
    pthread_mutex_unlock(&vq);
    return v;
}

int VBMGetFrame(int chn, void **frame)
{
    int ret = -1;

    if (chn < 0 || chn >= SRCS)
        return -1;
    pthread_mutex_lock(&vq);
    if (qn[chn]) {
        *frame = q[chn][qh[chn]].f;
        qh[chn] = (qh[chn] + 1) % QMAX;
        qn[chn]--;
        held[chn]++;
        ret = 0;
    }
    pthread_mutex_unlock(&vq);
    return ret;
}

int IMP_FrameSource_GetFrame(int chn, void **frame) { (void)chn; (void)frame; return -1; }
int IMP_FrameSource_ReleaseFrame(int chn, void *frame)
{
    (void)frame;
    if (chn < 0 || chn >= SRCS)
        return -1;
    pthread_mutex_lock(&vq);
    held[chn]--;
    released[chn]++;
    pthread_mutex_unlock(&vq);
    return 0;
}

unsigned int VBMReadySequence(int chn)
{
    unsigned int v;

    pthread_mutex_lock(&vq);
    v = chn >= 0 && chn < SRCS ? ready_seq[chn] : 0u;
    pthread_mutex_unlock(&vq);
    return v;
}

int VBMWaitReady(int chn, unsigned int seq, uint32_t us)
{
    struct timespec until;
    int moved;

    if (chn < 0 || chn >= SRCS)
        return -1;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)us * 1000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&vq);
    while (ready_seq[chn] == seq &&
           pthread_cond_timedwait(&vcond, &vq, &until) != ETIMEDOUT)
        ;
    moved = ready_seq[chn] != seq;
    pthread_mutex_unlock(&vq);
    return moved ? 0 : -1;
}

void VBMWakeReaders(int chn)
{
    if (chn < 0 || chn >= SRCS)
        return;
    pthread_mutex_lock(&vq);
    ready_seq[chn]++;
    pthread_cond_broadcast(&vcond);
    pthread_mutex_unlock(&vq);
}

void VBMNotePull(int chn) { (void)chn; }

int VBMExpireReady(int chn, uint32_t max_age_ms, int query)
{
    int n;

    if (chn < 0 || chn >= SRCS)
        return 0;
    pthread_mutex_lock(&vq);
    n = qn[chn];
    if (!query && max_age_ms == 0u) {
        old_expire_all++;
        given_back[chn] += n;
        qh[chn] = (qh[chn] + n) % QMAX;
        qn[chn] = 0;
    }
    pthread_mutex_unlock(&vq);
    return n;
}

unsigned int VBMReadyTag(int chn)
{
    unsigned int v;

    pthread_mutex_lock(&vq);
    v = chn >= 0 && chn < SRCS ? pub_tag[chn] : 0u;
    pthread_mutex_unlock(&vq);
    return v;
}

int VBMExpireReadyUpTo(int chn, unsigned int tag)
{
    int n = 0;

    if (chn < 0 || chn >= SRCS)
        return 0;
    pthread_mutex_lock(&vq);
    while (qn[chn] && (int32_t)(q[chn][qh[chn]].tag - tag) <= 0) {
        qh[chn] = (qh[chn] + 1) % QMAX;
        qn[chn]--;
        given_back[chn]++;
        n++;
    }
    pthread_mutex_unlock(&vq);
    return n;
}

void VBMNoteFrameReader(int chn) { (void)chn; }
int VBMFrameReaderActive(int chn)
{
    return chn >= 0 && chn < SRCS ? atomic_read(&reader_active[chn]) : 0;
}

/* ---- stub codec: one stream per picture ---- */
typedef struct { int id; int streams; } FakeCodec;
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ccond = PTHREAD_COND_INITIALIZER;
static int codecs, proc_count[4], proc_inside[4], gate[4], proc_sleep_us[4];
static int enc_policy = -1;
static uint32_t hw_stream[4][32];

int AL_Codec_Encode_Create(void **codec, void *params)
{
    FakeCodec *c = calloc(1, 64);

    (void)params;
    if (!c)
        return -1;
    pthread_mutex_lock(&cm);
    c->id = codecs++ % 4;
    pthread_mutex_unlock(&cm);
    *codec = c;
    return 0;
}
int AL_Codec_Encode_Destroy(void *codec) { free(codec); return 0; }

int AL_Codec_Encode_Process(void *codec, void *f, void *u)
{
    FakeCodec *c = codec;
    int us;

    (void)f; (void)u;
    pthread_mutex_lock(&cm);
    proc_inside[c->id]++;
    pthread_cond_broadcast(&ccond);
    while (gate[c->id])
        pthread_cond_wait(&ccond, &cm);
    us = proc_sleep_us[c->id];
    enc_policy = sched_getscheduler(0);
    pthread_mutex_unlock(&cm);
    if (us)
        usleep((useconds_t)us);
    pthread_mutex_lock(&cm);
    proc_inside[c->id]--;
    proc_count[c->id]++;
    c->streams++;
    pthread_cond_broadcast(&ccond);
    pthread_mutex_unlock(&cm);
    return 0;
}

int AL_Codec_Encode_GetStream(void *codec, void **s, void **u)
{
    FakeCodec *c = codec;
    int ret = 1;

    pthread_mutex_lock(&cm);
    if (c->streams) {
        c->streams--;
        *s = hw_stream[c->id];
        *u = NULL;
        ret = 0;
    }
    pthread_mutex_unlock(&cm);
    return ret;
}
int AL_Codec_Encode_ReleaseStream(void *c, void *s, void *u) { (void)c; (void)s; (void)u; return 0; }

static int count_of(int *p)
{
    int v;

    pthread_mutex_lock(&cm);
    v = *p;
    pthread_mutex_unlock(&cm);
    return v;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* wait (max 2 s) until *p reaches v */
static int wait_count(int *p, int v)
{
    uint64_t t0 = now_ms();

    while (count_of(p) < v) {
        if (now_ms() - t0 > 2000u)
            return 0;
        usleep(200);
    }
    return 1;
}

static int wait_given_back(int s, int v)
{
    uint64_t t0 = now_ms();

    while (atomic_read(&given_back[s]) < v) {
        if (now_ms() - t0 > 2000u)
            return 0;
        usleep(200);
    }
    return 1;
}

/* the encode thread finished the give-back step: nothing inside Process
 * and the queue no longer changes */
static void settle(void)
{
    usleep(20000);
}

static void make_attr(IMPEncoderCHNAttr *attr, int denoise)
{
    memset(attr, 0, sizeof(*attr));
    attr->encAttr.enType = PT_H264;
    attr->encAttr.profile = 1;
    attr->encAttr.picWidth = 640;
    attr->encAttr.picHeight = 360;
    attr->rcAttr.outFrmRate.frmRateNum = 25;
    attr->rcAttr.outFrmRate.frmRateDen = 1;
    attr->rcAttr.maxGop = 50;
    attr->rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_CBR;
    attr->rcAttr.attrRcMode.attrH264Cbr.maxQp = 45;
    attr->rcAttr.attrRcMode.attrH264Cbr.minQp = 20;
    attr->rcAttr.attrRcMode.attrH264Cbr.outBitRate = 1000;
    attr->rcAttr.attrDenoise.enable = denoise;
}

/* the application side: take every finished stream of channel ch */
static int drain_streams(int ch)
{
    IMPEncoderStream st;
    int n = 0;

    memset(&st, 0, sizeof(st));
    while (IMP_Encoder_PollingStream(ch, 0) == 0) {
        if (IMP_Encoder_ReleaseStream(ch, &st) != 0)
            break;
        n++;
    }
    return n;
}

/* one sensor frame of source s: publish, then kick (the capture worker) */
static void frame_tick(int s)
{
    publish(s);
    (void)OpenIMP_FS_PushEncode(s);
}

#define INTERVAL_US 41000       /* just over the 25 fps output interval */

/* #916 */
static void test_unkicked_frame_survives(void)
{
    int base = count_of(&proc_count[0]);
    int gb = atomic_read(&given_back[0]);

    drain_streams(0);
    usleep(INTERVAL_US);
    pthread_mutex_lock(&cm);
    gate[0] = 1;
    pthread_mutex_unlock(&cm);
    frame_tick(0);                                  /* frame N, kicked */
    CHECK(wait_count(&proc_inside[0], 1));          /* Encoder-0 encodes N */
    publish(0);                                     /* N+1 published, not kicked yet */
    pthread_mutex_lock(&cm);
    gate[0] = 0;
    pthread_cond_broadcast(&ccond);
    pthread_mutex_unlock(&cm);
    CHECK(wait_count(&proc_count[0], base + 1));
    settle();
    CHECK(old_expire_all == 0);
    CHECK(qlen(0) == 1);                            /* N+1 still queued */
    CHECK(atomic_read(&given_back[0]) == gb);
    usleep(INTERVAL_US);
    (void)OpenIMP_FS_PushEncode(0);                 /* the kick for N+1 */
    CHECK(wait_count(&proc_count[0], base + 2));    /* N+1 encoded */
    settle();
    CHECK(qlen(0) == 0);
    CHECK(drain_streams(0) == 2);
}

/* #917 */
static void test_encode_while_app_holds_stream(void)
{
    IMPEncoderStream st;
    int base, n;

    memset(&st, 0, sizeof(st));
    drain_streams(0);
    usleep(INTERVAL_US);
    base = count_of(&proc_count[0]);
    frame_tick(0);
    CHECK(wait_count(&proc_count[0], base + 1));
    settle();
    CHECK(IMP_Encoder_PollingStream(0, 0) == 0);    /* the app holds it */
    usleep(INTERVAL_US);
    frame_tick(0);                                  /* next capture while held */
    CHECK(wait_count(&proc_count[0], base + 2));
    settle();
    CHECK(qlen(0) == 0);
    CHECK(IMP_Encoder_ReleaseStream(0, &st) == 0);
    n = drain_streams(0);
    CHECK(n == 1);                                  /* the one encoded while held */
}

/* #918 */
static void test_no_giveback_while_reader(void)
{
    int base = count_of(&proc_count[0]);
    int gb;

    drain_streams(0);
    usleep(3 * INTERVAL_US);                /* pacing restarts: next due = now + interval */
    pthread_mutex_lock(&vq);
    reader_active[0] = 1;
    pthread_mutex_unlock(&vq);
    frame_tick(0);                          /* encoded */
    CHECK(wait_count(&proc_count[0], base + 1));
    usleep(2000);
    gb = atomic_read(&given_back[0]);
    frame_tick(0);                          /* early: skipped by the output pacing */
    usleep(5000);
    CHECK(count_of(&proc_count[0]) == base + 1);
    CHECK(qlen(0) == 1);                    /* left for the reader (worker ages it out) */
    CHECK(atomic_read(&given_back[0]) == gb);
    pthread_mutex_lock(&vq);
    reader_active[0] = 0;
    pthread_mutex_unlock(&vq);
    frame_tick(0);                          /* early again: skipped, both go back */
    CHECK(wait_given_back(0, gb + 2));
    settle();
    CHECK(qlen(0) == 0);
    drain_streams(0);
}

/* two channels on two sources, channel 1 slow (waits on the shared
 * encoder or a big frame): every frame of both is encoded */
static void test_two_channels(void)
{
    int b0 = count_of(&proc_count[0]), b1 = count_of(&proc_count[1]);
    int gb0 = atomic_read(&given_back[0]), gb1 = atomic_read(&given_back[1]);
    int got0 = 0, got1 = 0, i;
    const int frames = 25;

    drain_streams(0);
    drain_streams(1);
    usleep(INTERVAL_US);
    pthread_mutex_lock(&cm);
    proc_sleep_us[1] = 38000;               /* encode ends inside the next frame's window */
    pthread_mutex_unlock(&cm);
    for (i = 0; i < frames; i++) {
        frame_tick(0);
        frame_tick(1);
        usleep(INTERVAL_US);
        got0 += drain_streams(0);
        got1 += drain_streams(1);
    }
    CHECK(wait_count(&proc_count[1], b1 + frames));
    settle();
    got0 += drain_streams(0);
    got1 += drain_streams(1);
    pthread_mutex_lock(&cm);
    proc_sleep_us[1] = 0;
    pthread_mutex_unlock(&cm);
    CHECK(count_of(&proc_count[0]) - b0 == frames);
    CHECK(count_of(&proc_count[1]) - b1 == frames);
    CHECK(got0 == frames && got1 == frames);
    CHECK(atomic_read(&given_back[0]) == gb0 && atomic_read(&given_back[1]) == gb1);
    CHECK(qlen(0) == 0 && qlen(1) == 0);
    if (count_of(&proc_count[1]) - b1 != frames || got1 != frames)
        fprintf(stderr, "two_channels: ch0 %d/%d ch1 %d/%d streams %d %d\n",
                count_of(&proc_count[0]) - b0, frames,
                count_of(&proc_count[1]) - b1, frames, got0, got1);
}

/* the capture worker at sensor rate, as fast as the test runs */
static volatile int ticker_run;
static void *ticker(void *arg)
{
    (void)arg;
    while (ticker_run) {
        frame_tick(0);
        frame_tick(1);
        usleep(5000);
    }
    return NULL;
}

static void *app_reader(void *arg)
{
    (void)arg;
    while (ticker_run) {
        drain_streams(0);
        drain_streams(1);
        usleep(2000);
    }
    return NULL;
}

static void test_stop_start_cycles(void)
{
    pthread_t tk, rd;
    int cycle, ok = 1;

    ticker_run = 1;
    CHECK(pthread_create(&tk, NULL, ticker, NULL) == 0);
    CHECK(pthread_create(&rd, NULL, app_reader, NULL) == 0);
    for (cycle = 0; cycle < 20; cycle++) {
        int base = count_of(&proc_count[0]);

        if (!wait_count(&proc_count[0], base + 2))
            ok = 0;
        CHECK(IMP_Encoder_StopRecvPic(0) == 0);
        CHECK(IMP_Encoder_StopRecvPic(1) == 0);
        usleep(10000);
        CHECK(IMP_Encoder_StartRecvPic(0) == 0);
        CHECK(IMP_Encoder_StartRecvPic(1) == 0);
    }
    CHECK(ok);                              /* encoding resumed after every start */
    ticker_run = 0;
    pthread_join(tk, NULL);
    pthread_join(rd, NULL);
    settle();
    /* the worker hands back the last unkicked frames itself (age path);
     * the encoder holds no capture buffer */
    CHECK(atomic_read(&held[0]) == 0 && atomic_read(&held[1]) == 0);
    drain_streams(0);
    drain_streams(1);
}

int main(void)
{
    IMPEncoderCHNAttr attr;
    IMPCell fs0 = { DEV_ID_FS, 0, 0 }, fs1 = { DEV_ID_FS, 1, 0 };
    IMPCell enc0 = { DEV_ID_ENC, 0, 0 }, enc1 = { DEV_ID_ENC, 1, 0 };
    struct sched_param sp = { .sched_priority = 1 };
    int fifo;

    /* #920 L2: if the host lets this thread (the "capture worker") run
     * SCHED_FIFO, Encoder-N, created by its first kick, must not */
    fifo = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    CHECK(IMP_Encoder_CreateGroup(0) == 0);
    CHECK(IMP_Encoder_CreateGroup(1) == 0);
    make_attr(&attr, 0);
    CHECK(IMP_Encoder_CreateChn(0, &attr) == 0);
    CHECK(IMP_Encoder_CreateChn(1, &attr) == 0);
    CHECK(IMP_Encoder_RegisterChn(0, 0) == 0);
    CHECK(IMP_Encoder_RegisterChn(1, 1) == 0);
    CHECK(IMP_System_Bind(&fs0, &enc0) == 0);
    CHECK(IMP_System_Bind(&fs1, &enc1) == 0);
    CHECK(IMP_Encoder_StartRecvPic(0) == 0);
    CHECK(IMP_Encoder_StartRecvPic(1) == 0);

    frame_tick(0);
    CHECK(wait_count(&proc_count[0], 1));
    settle();
    CHECK(enc_policy == SCHED_OTHER);
    if (fifo) {
        sp.sched_priority = 0;
        (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    } else {
        fprintf(stderr, "p2_push_t20_test: no SCHED_FIFO here, L2 checks the policy only\n");
    }
    drain_streams(0);

    test_unkicked_frame_survives();
    test_encode_while_app_holds_stream();
    test_no_giveback_while_reader();
    test_two_channels();
    test_stop_start_cycles();
    if (failures) {
        fprintf(stderr, "p2_push_t20_test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "p2_push_t20_test: ok\n");
    return 0;
}
