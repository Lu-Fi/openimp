/* jpegreuse - device proof of OpenIMP_Encoder_StreamIsReused (Helix SoCs, T31).
 *   jpegreuse SENSOR ADDR W H [secs [jpeg channels 1..8 [squeeze 0/1]]]
 * An H.264 channel runs at full rate (keeps the encoder core busy) while a
 * JPEG channel in the same group is polled as fast as it answers.  For each
 * JPEG: reused flag from the library, content hash, time stamp.  Checks:
 * a reused picture is byte-identical to the previous one, a fresh one is
 * not; both are valid JPEGs; time stamps never go back.  Stop the streamer
 * first. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <openimp/openimp_encoder_telemetry.h>
#include "dma_alloc.h"

__attribute__((weak)) int IMP_Log_Get_Option(void) { return 0; }
__attribute__((weak)) int imp_log_fun(int level, int option, int type, const char *tag, const char *file,
                                      int line, const char *func, const char *fmt, ...)
{ (void)level;(void)option;(void)type;(void)tag;(void)file;(void)line;(void)func;(void)fmt; return 0; }
int OpenIMP_Encoder_StreamIsReused(int encChn, const IMPEncoderStream *stream) __attribute__((weak));

#define CK(e, m) do { int r_ = (e); if (r_ < 0) { printf("[E] %s -> %d\n", m, r_); exit(1); } } while (0)
static volatile int run = 1;
static volatile unsigned vframes;
static unsigned hold_ms;      /* video consumer keeps every 50th stream this long */

static void *video(void *a)
{
    (void)a;
    while (run) {
        IMPEncoderStream s;
        if (IMP_Encoder_PollingStream(0, 200) != 0) continue;
        memset(&s, 0, sizeof s);
        if (IMP_Encoder_GetStream(0, &s, true) != 0) continue;
        vframes++;
        if (hold_ms && vframes % 50 == 0) usleep(hold_ms * 1000);
        IMP_Encoder_ReleaseStream(0, &s);
    }
    return NULL;
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* Squeezer: every 4 s it takes the free video memory in 128 KiB blocks so the
 * JPEG copy buffer cannot be allocated (rmem short -> the library re-delivers
 * the last picture), and gives it back 4 s later. Phase is printed. */
extern uintptr_t IMP_Alloc(void *name_or_size, intptr_t size, char *tag);
extern int IMP_Free(uintptr_t phys);
static volatile int squeeze_on, squeeze_cycle;
static void *squeezer(void *a)
{
    IMPDMABufferInfo held[256]; int n, i;
    (void)a;
    while (run) {
        for (i = 0; i < 40 && run; i++) usleep(100000);
        if (!run) break;
        for (n = 0; n < 256; n++) {
            memset(&held[n], 0, sizeof held[n]);
            if (IMP_Alloc(&held[n], 128 * 1024, "hog") != 0 || !held[n].phys_addr) break;
        }
        squeeze_cycle++; squeeze_on = 1;
        printf("[S] squeeze on: %d blocks (%d KiB) taken\n", n, n * 128);
        for (i = 0; i < 40 && run; i++) usleep(100000);
        squeeze_on = 0;
        for (i = 0; i < n; i++) IMP_Free(held[i].phys_addr);
        printf("[S] squeeze off\n");
    }
    return NULL;
}

typedef struct {
    int chn, secs;
    unsigned n_phase[2][2];     /* [squeeze on][reused] */
    unsigned n, n_re, bad_re, bad_fresh, bad_jpeg, back, query_bad;
} JW;

static void *jworker(void *arg)
{
    JW *w = arg;
    struct timespec t0, t1;
    uint64_t prev_hash = 0; int64_t prev_ts = 0; int have_prev = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        IMPEncoderStream s; const uint8_t *p; uint32_t len; int re; uint64_t h; int64_t ts;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec - t0.tv_sec >= w->secs) break;
        if (IMP_Encoder_PollingStream(w->chn, 500) != 0) continue;
        memset(&s, 0, sizeof s);
        if (IMP_Encoder_GetStream(w->chn, &s, true) != 0) continue;
        re = OpenIMP_Encoder_StreamIsReused ? OpenIMP_Encoder_StreamIsReused(w->chn, &s) : -2;
        len = s.pack[0].length;
#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
        p = (const uint8_t *)(uintptr_t)s.pack[0].virAddr;
#else
        p = (const uint8_t *)(uintptr_t)(s.virAddr + s.pack[0].offset);
#endif
        h = fnv(p, len);
        ts = s.pack[0].timestamp;
        if (len < 4 || p[0] != 0xff || p[1] != 0xd8 || p[len - 2] != 0xff || p[len - 1] != 0xd9) w->bad_jpeg++;
        if (re != 0 && re != 1) w->query_bad++;
        w->n_phase[squeeze_on][re == 1]++;
        if (have_prev) {
            if (re == 1 && h != prev_hash) w->bad_re++;
            if (re == 0 && h == prev_hash) w->bad_fresh++;
            if (ts < prev_ts) w->back++;
        }
        if (re == 1) w->n_re++;
        w->n++;
        if (w->chn == 1 && (w->n <= 6 || (re == 1 && w->n_re <= 8)))
            printf("[J] ch%d #%u reused=%d len=%u hash=%016llx ts=%lld\n", w->chn, w->n, re, len, (unsigned long long)h, (long long)ts);
        prev_hash = h; prev_ts = ts; have_prev = 1;
        IMP_Encoder_ReleaseStream(w->chn, &s);
    }
    return NULL;
}

static void mkattr(IMPEncoderCHNAttr *a, int jpeg, int W, int H)
{
    memset(a, 0, sizeof *a);
#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
    a->encAttr.enType = jpeg ? PT_JPEG : PT_H264;
    a->encAttr.profile = jpeg ? 0 : 1;
    a->encAttr.picWidth = W; a->encAttr.picHeight = H;
    a->rcAttr.outFrmRate.frmRateNum = 25; a->rcAttr.outFrmRate.frmRateDen = 1;
    a->rcAttr.maxGop = 50;
    if (jpeg) {
        a->rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_FIXQP;
        a->rcAttr.attrRcMode.attrH264FixQp.qp = 38;
    } else {
        a->rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_CBR;
        a->rcAttr.attrRcMode.attrH264Cbr.maxQp = 45;
        a->rcAttr.attrRcMode.attrH264Cbr.minQp = 20;
        a->rcAttr.attrRcMode.attrH264Cbr.outBitRate = 4000;
    }
#else
    CK(IMP_Encoder_SetDefaultParam(a, jpeg ? IMP_ENC_PROFILE_JPEG : IMP_ENC_PROFILE_AVC_HIGH,
        jpeg ? IMP_ENC_RC_MODE_FIXQP : IMP_ENC_RC_MODE_CBR, W, H, 25, 1, jpeg ? 0 : 50, 2, jpeg ? 80 : -1, jpeg ? 0 : 4000), "SetDefaultParam");
#endif
}

int main(int argc, char **argv)
{
    int W, H, secs = argc > 5 ? atoi(argv[5]) : 20;
    int nj = argc > 6 ? atoi(argv[6]) : 1;
    IMPSensorInfo sn; IMPFSChnAttr fa; IMPEncoderCHNAttr a;
    IMPCell fs = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };
    pthread_t tv, tsq;
    int squeeze = 0;
    JW w[8]; pthread_t jt[8]; int i; unsigned tn = 0, tre = 0, bad = 0;
    if (argc < 5) return 2;
    squeeze = argc > 7 ? atoi(argv[7]) : 0;
    if (nj < 1) nj = 1;
    if (nj > 8) nj = 8;
    W = atoi(argv[3]); H = atoi(argv[4]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&sn, 0, sizeof sn);
    strncpy(sn.name, argv[1], sizeof sn.name - 1);
    sn.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sn.i2c.type, argv[1], sizeof sn.i2c.type - 1);
    sn.i2c.addr = (int)strtol(argv[2], NULL, 0);
    sn.rst_gpio = sn.pwdn_gpio = sn.power_gpio = -1;
    CK(IMP_ISP_Open(), "ISP_Open");
    CK(IMP_ISP_AddSensor(&sn), "AddSensor");
    CK(IMP_ISP_EnableSensor(), "EnableSensor");
    CK(IMP_System_Init(), "System_Init");
    CK(IMP_ISP_EnableTuning(), "EnableTuning");
    memset(&fa, 0, sizeof fa);
    fa.picWidth = W; fa.picHeight = H; fa.pixFmt = PIX_FMT_NV12;
    fa.outFrmRateNum = 25; fa.outFrmRateDen = 1; fa.nrVBs = 2; fa.type = FS_PHY_CHANNEL;
    CK(IMP_FrameSource_CreateChn(0, &fa), "FS_CreateChn");
    CK(IMP_FrameSource_EnableChn(0), "FS_Enable");
    sleep(1);
    mkattr(&a, 0, W, H);
    CK(IMP_Encoder_CreateChn(0, &a), "CreateChn0");
    mkattr(&a, 1, W, H);
    for (i = 1; i <= nj; i++) CK(IMP_Encoder_CreateChn(i, &a), "CreateChnJ");
    CK(IMP_Encoder_CreateGroup(0), "CreateGroup");
    for (i = 0; i <= nj; i++) CK(IMP_Encoder_RegisterChn(0, i), "RegisterChn");
    CK(IMP_System_Bind(&fs, &enc), "Bind");
    for (i = 0; i <= nj; i++) CK(IMP_Encoder_StartRecvPic(i), "StartRecvPic");
    printf("[I] OpenIMP_Encoder_StreamIsReused symbol %s\n", OpenIMP_Encoder_StreamIsReused ? "present" : "ABSENT");
    pthread_create(&tv, NULL, video, NULL);
    if (squeeze) pthread_create(&tsq, NULL, squeezer, NULL);
    memset(w, 0, sizeof w);
    for (i = 0; i < nj; i++) { w[i].chn = 1 + i; w[i].secs = secs; pthread_create(&jt[i], NULL, jworker, &w[i]); }
    for (i = 0; i < nj; i++) pthread_join(jt[i], NULL);
    run = 0; pthread_join(tv, NULL);
    if (squeeze) pthread_join(tsq, NULL);
    for (i = 0; i < nj; i++) {
        printf("[R] ch%d: %u JPEG (%u reused, %u fresh); reused-but-different=%u fresh-but-identical=%u invalid-jpeg=%u ts-back=%u query-error=%u\n",
               w[i].chn, w[i].n, w[i].n_re, w[i].n - w[i].n_re, w[i].bad_re, w[i].bad_fresh, w[i].bad_jpeg, w[i].back, w[i].query_bad);
        printf("[R] ch%d by phase: free memory: %u fresh %u reused; squeezed: %u fresh %u reused\n", w[i].chn,
               w[i].n_phase[0][0], w[i].n_phase[0][1], w[i].n_phase[1][0], w[i].n_phase[1][1]);
       
        tn += w[i].n; tre += w[i].n_re;
        bad += w[i].bad_re + w[i].bad_fresh + w[i].bad_jpeg + w[i].back + w[i].query_bad;
    }
    printf("[R] %d s, %d JPEG channels: %u JPEG, %u reused, %u video frames: %s\n", secs, nj, tn, tre, vframes,
           (tre && tn > tre && !bad) ? "PASS" : "CHECK");
    for (i = 0; i <= nj; i++) IMP_Encoder_StopRecvPic(i);
    IMP_System_UnBind(&fs, &enc);
    for (i = nj; i >= 0; i--) IMP_Encoder_UnRegisterChn(i);
    for (i = nj; i >= 0; i--) IMP_Encoder_DestroyChn(i);
    IMP_Encoder_DestroyGroup(0);
    IMP_FrameSource_DisableChn(0); IMP_FrameSource_DestroyChn(0);
    IMP_System_Exit();
    return 0;
}
