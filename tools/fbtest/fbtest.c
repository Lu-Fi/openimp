/* fbtest - device proof of the fallback frame (OpenIMP_FrameSource_SetFallback), T41.
 *   fbtest SENSOR ADDR W H STREAM_REG OUTDIR
 * One H.264 channel with a consumer thread.  Sensor frames are stopped and
 * (env FBTEST_ON / FBTEST_OFF override the on/off values 1/0, gc5603: 0x09/0x08)
 * restarted by writing STREAM_REG (SmartSens 0x0100, bit 0) through
 * IMP_ISP_SetSensorRegister; the library, modules and the encoder stay up.
 *   1 real           4 s   (default state of the fallback is printed: enabled=1 since 26.10.)
 *   2 off, no fb     4 s   (control: nothing arrives)
 *   3 resume         4 s
 *   4 off, fb on     9 s   (message frames after the dwell time)
 *   5 resume         6 s   (switch back)
 *   6 fb switched off while active (off again 5 s, then SetFallback(0)), resume
 * Every phase is written to OUTDIR/phaseN.h264 (starts at an IDR, capped);
 * per phase: frames, fps, time stamps (monotonic?), state of Get.
 * Stop the streamer first; the files go to /tmp (keep them small). */
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
#include <openimp_fallback.h>

__attribute__((weak)) int IMP_Log_Get_Option(void) { return 0; }
__attribute__((weak)) int imp_log_fun(int level, int option, int type, const char *tag, const char *file,
                                      int line, const char *func, const char *fmt, ...)
{ (void)level;(void)option;(void)type;(void)tag;(void)file;(void)line;(void)func;(void)fmt; return 0; }
int OpenIMP_FrameSource_SetFallback(int chn, int on) __attribute__((weak));
int OpenIMP_FrameSource_GetFallback(int chn, OpenIMPFallbackState *s) __attribute__((weak));

#define CK(e, m) do { int r_ = (e); if (r_ < 0) { printf("[E] %s -> %d\n", m, r_); exit(1); } } while (0)
static volatile int run = 1;
static volatile unsigned frames;
static volatile int64_t last_ts, ts_back, ts_first, ts_last;
static FILE *volatile out;
static volatile size_t out_bytes;
#define OUT_CAP (1500u * 1024u)

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000; }

static void *consumer(void *a)
{
    (void)a;
    while (run) {
        IMPEncoderStream s; uint32_t k;
        if (IMP_Encoder_PollingStream(0, 200) != 0) continue;
        memset(&s, 0, sizeof s);
        if (IMP_Encoder_GetStream(0, &s, true) != 0) continue;
        frames++;
        if (s.packCount) {
            int64_t ts = s.pack[s.packCount - 1].timestamp;
            if (ts < last_ts) ts_back++;
            if (!ts_first) ts_first = ts;
            ts_last = last_ts = ts;
        }
        if (out) {
            for (k = 0; k < s.packCount && out_bytes < OUT_CAP; k++) {
                uint32_t off = s.pack[k].offset, len = s.pack[k].length;
                if (off + len > s.streamSize) {
                    uint32_t first = s.streamSize - off;
                    fwrite((void *)(uintptr_t)(s.virAddr + off), 1, first, out);
                    fwrite((void *)(uintptr_t)s.virAddr, 1, len - first, out);
                } else fwrite((void *)(uintptr_t)(s.virAddr + off), 1, len, out);
                out_bytes += len;
            }
        }
        IMP_Encoder_ReleaseStream(0, &s);
    }
    return NULL;
}

static const char *outdir;
static void sensor_stream(int on, uint32_t reg)
{
    IMPISPSensorRegister r; int rc;
    const char *ev = getenv(on ? "FBTEST_ON" : "FBTEST_OFF");  /* GalaxyCore gc5603: 0x0100 = 0x09 on / 0x08 off */
    r.addr = reg; r.value = ev ? (uint32_t)strtoul(ev, NULL, 0) : (on ? 1 : 0);
    rc = IMP_ISP_SetSensorRegister(IMPVI_MAIN, &r);
    printf("[S] %lld sensor stream %s (reg 0x%x) rc=%d\n", (long long)now_ms(), on ? "ON" : "OFF", reg, rc);
}

static void phase(int n, const char *name, int secs)
{
    char path[256]; unsigned f0; int i; int64_t ts_back0;
    snprintf(path, sizeof path, "%s/phase%d.h264", outdir, n);
    out_bytes = 0; out = fopen(path, "wb");
    IMP_Encoder_RequestIDR(0);
    f0 = frames; ts_back0 = ts_back; ts_first = 0;
    for (i = 0; i < secs; i++) {
        OpenIMPFallbackState st = { .size = sizeof st };
        sleep(1);
        if (OpenIMP_FrameSource_GetFallback) OpenIMP_FrameSource_GetFallback(0, &st);
        printf("[P%d] t=%ds frames=%u fb: enabled=%u active=%u error=%d generated=%u act=%u real_age=%u ms\n",
               n, i + 1, frames - f0, st.enabled, st.active, st.error, st.generated, st.activations, st.last_real_age_ms);
    }
    if (out) { fclose(out); out = NULL; }
    printf("[R%d] %s: %u frames in %d s (%.1f fps), ts back %lld, ts span %lld ms, %zu bytes -> %s\n", n, name, frames - f0, secs,
           (frames - f0) / (double)secs, (long long)(ts_back - ts_back0), (long long)((ts_last - ts_first) / 1000), out_bytes, path);
}

int main(int argc, char **argv)
{
    int W, H; uint32_t reg;
    IMPSensorInfo sn; IMPFSChnAttr fa; IMPEncoderChnAttr a;
    IMPCell fs = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };
    pthread_t tc;
    if (argc < 7) return 2;
    W = atoi(argv[3]); H = atoi(argv[4]); reg = (uint32_t)strtoul(argv[5], NULL, 0); outdir = argv[6];
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[I] fallback API %s\n", OpenIMP_FrameSource_SetFallback ? "present" : "ABSENT");
    memset(&sn, 0, sizeof sn);
    strncpy(sn.name, argv[1], sizeof sn.name - 1);
    sn.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sn.i2c.type, argv[1], sizeof sn.i2c.type - 1);
    sn.i2c.addr = (int)strtol(argv[2], NULL, 0);
    sn.rst_gpio = sn.pwdn_gpio = sn.power_gpio = -1;
    CK(IMP_ISP_Open(), "ISP_Open");
    CK(IMP_ISP_AddSensor(IMPVI_MAIN, &sn), "AddSensor");
    CK(IMP_ISP_EnableSensor(IMPVI_MAIN, &sn), "EnableSensor");
    CK(IMP_System_Init(), "System_Init");
    CK(IMP_ISP_EnableTuning(), "EnableTuning");
    memset(&fa, 0, sizeof fa);
    fa.picWidth = W; fa.picHeight = H; fa.pixFmt = PIX_FMT_NV12;
    fa.outFrmRateNum = 15; fa.outFrmRateDen = 1; fa.nrVBs = 2; fa.type = FS_PHY_CHANNEL;
    CK(IMP_FrameSource_CreateChn(0, &fa), "FS_CreateChn");
    CK(IMP_FrameSource_EnableChn(0), "FS_Enable");
    sleep(1);
    memset(&a, 0, sizeof a);
    CK(IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_AVC_HIGH, IMP_ENC_RC_MODE_CBR, W, H, 15, 1, 30, 2, -1, 1000), "SetDefaultParam");
    CK(IMP_Encoder_CreateChn(0, &a), "CreateChn");
    CK(IMP_Encoder_CreateGroup(0), "CreateGroup");
    CK(IMP_Encoder_RegisterChn(0, 0), "RegisterChn");
    CK(IMP_System_Bind(&fs, &enc), "Bind");
    CK(IMP_Encoder_StartRecvPic(0), "StartRecvPic");
    pthread_create(&tc, NULL, consumer, NULL);
    phase(1, "real frames", 4);
    if (OpenIMP_FrameSource_SetFallback) printf("[F] control: SetFallback(0,0) rc=%d (phase 1 shows the default state)\n", OpenIMP_FrameSource_SetFallback(0, 0));
    sensor_stream(0, reg);
    phase(2, "sensor off, fallback off (control)", 4);
    sensor_stream(1, reg);
    phase(3, "sensor resumed", 4);
    if (OpenIMP_FrameSource_SetFallback) printf("[F] SetFallback(0,1) rc=%d\n", OpenIMP_FrameSource_SetFallback(0, 1));
    sensor_stream(0, reg);
    phase(4, "sensor off, fallback on", 9);
    sensor_stream(1, reg);
    phase(5, "sensor resumed (switch back)", 6);
    sensor_stream(0, reg);
    sleep(3);
    if (OpenIMP_FrameSource_SetFallback) printf("[F] SetFallback(0,0) while active rc=%d\n", OpenIMP_FrameSource_SetFallback(0, 0));
    phase(6, "fallback switched off while the sensor is off", 4);
    sensor_stream(1, reg);
    phase(7, "sensor resumed, fallback off", 4);
    run = 0; pthread_join(tc, NULL);
    CK(IMP_Encoder_StopRecvPic(0), "StopRecvPic");
    IMP_System_UnBind(&fs, &enc);
    IMP_Encoder_UnRegisterChn(0);
    IMP_Encoder_DestroyChn(0); IMP_Encoder_DestroyGroup(0);
    IMP_FrameSource_DisableChn(0); IMP_FrameSource_DestroyChn(0);
    IMP_System_Exit();
    return 0;
}
