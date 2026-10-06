/* roitest - encoder ROI on-device test (T21/T23 family A, T31/T41 family B).
 *
 *   roitest SENSOR I2C_ADDR W H OUTDIR PHASE...
 *   PHASE = label:seconds:spec        label starting with '~': no IDR request
 *   spec  = none | region[+region...]
 *   region= idx,mode,qp,x,y,w,h       mode a = absolute QP, d = delta QP
 *                                     pixels; family A p1 = x+w-1, y+h-1
 *
 * Every phase first disables all regions, applies the spec, requests an IDR
 * (unless '~'), then writes the H.264 stream to OUTDIR/label.h264 and prints
 * one result line:  [R] label frames bytes kbps idr_avg p_avg
 * Env: ROITEST_FS_FIRST=1 (enable FS before creating the encoder), ROITEST_OUT=WxH (scale ch0), ROITEST_RC=cbr|fixqp (cbr), ROITEST_KBPS (2000), ROITEST_QP (30),
 *      ROITEST_FPS (15), ROITEST_MINQP/ROITEST_MAXQP (15/45).
 * Stop the streamer first; the camera's /tmp is RAM, use short phases.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#if defined(PLATFORM_T31) || defined(PLATFORM_T41)
# define ENC_NEW 1
#endif
#if defined(PLATFORM_T31)
/* OpenIMP-only family B API (beyond vendor on T31) */
# define ROI_WIN 10
typedef struct { uint32_t x, y, w, h; } RRect;
typedef struct { bool enable; RRect rect; int mode; int8_t qp; } RWin;
typedef struct { RWin st_roi[ROI_WIN]; } RAttr;

# define HAVE_B 1
#elif defined(PLATFORM_T41)
# define HAVE_B 1
typedef IMPEncoderRoiAttr RAttr;
#endif

#ifdef HAVE_B
/* weak: a libimp without the family B API gets a clear message */
int IMP_Encoder_SetChnRoiAttr(int encChn, RAttr *a) __attribute__((weak));
int IMP_Encoder_GetChnRoiAttr(int encChn, RAttr *a) __attribute__((weak));
#endif
/* the vendor libsysutils/libalog expect these from the application or libimp */
#include <stdarg.h>
__attribute__((weak)) int IMP_Log_Get_Option(void) { return 0; }
__attribute__((weak)) void imp_log_fun(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)level; (void)tag;
    va_start(ap, fmt);
    if (getenv("ROITEST_LOG")) vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#define CK(e, m) do { int r_ = (e); if (r_ < 0) { printf("[E] %s -> %d\n", m, r_); exit(1); } } while (0)

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int nal_has_idr(const unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i + 4 < n; i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1f) == 5)
            return 1;
    return 0;
}

typedef struct { int idx, abs, qp, x, y, w, h; } Reg;

static int parse_spec(const char *s, Reg *r)
{
    int n = 0;
    char buf[256], *tok, *sv = NULL;

    if (!strcmp(s, "none")) return 0;
    snprintf(buf, sizeof(buf), "%s", s);
    for (tok = strtok_r(buf, "+", &sv); tok && n < 10; tok = strtok_r(NULL, "+", &sv)) {
        char m;
        if (sscanf(tok, "%d,%c,%d,%d,%d,%d,%d", &r[n].idx, &m, &r[n].qp, &r[n].x, &r[n].y, &r[n].w, &r[n].h) != 7) {
            printf("[E] bad region '%s'\n", tok);
            exit(2);
        }
        r[n].abs = m == 'a';
        n++;
    }
    return n;
}

static void apply_roi(int chn, const Reg *r, int n)
{
    int i, ret;
#ifdef HAVE_B
    RAttr a;
    memset(&a, 0, sizeof(a));
    for (i = 0; i < n; i++) {
        a.st_roi[r[i].idx].enable = true;
        a.st_roi[r[i].idx].rect.x = r[i].x; a.st_roi[r[i].idx].rect.y = r[i].y;
        a.st_roi[r[i].idx].rect.w = r[i].w; a.st_roi[r[i].idx].rect.h = r[i].h;
        a.st_roi[r[i].idx].mode = r[i].abs ? 1 : 0;
        a.st_roi[r[i].idx].qp = (int8_t)r[i].qp;
    }
    if (!IMP_Encoder_SetChnRoiAttr) { printf("[R] SetChnRoiAttr not exported\n"); return; }
    ret = IMP_Encoder_SetChnRoiAttr(chn, &a);
    printf("[R] SetChnRoiAttr(%d regions) ret=%d\n", n, ret);
    memset(&a, 0, sizeof(a));
    ret = IMP_Encoder_GetChnRoiAttr(chn, &a);
    for (i = 0; i < n; i++)
        printf("[R]   get win%d en=%d %u,%u %ux%u mode=%d qp=%d (ret %d)\n", r[i].idx, a.st_roi[r[i].idx].enable,
               a.st_roi[r[i].idx].rect.x, a.st_roi[r[i].idx].rect.y, a.st_roi[r[i].idx].rect.w,
               a.st_roi[r[i].idx].rect.h, (int)a.st_roi[r[i].idx].mode, a.st_roi[r[i].idx].qp, ret);
#else
    IMPEncoderROICfg c;
    for (i = 0; i < 8; i++) {
        memset(&c, 0, sizeof(c));
        c.u32Index = i;
        IMP_Encoder_SetChnROI(chn, &c);
    }
    for (i = 0; i < n; i++) {
        memset(&c, 0, sizeof(c));
        c.u32Index = r[i].idx; c.bEnable = true; c.bRelatedQp = !r[i].abs; c.s32Qp = r[i].qp;
        c.rect.p0.x = r[i].x; c.rect.p0.y = r[i].y;
        c.rect.p1.x = r[i].x + r[i].w - 1; c.rect.p1.y = r[i].y + r[i].h - 1;
        ret = IMP_Encoder_SetChnROI(chn, &c);
        memset(&c, 0, sizeof(c));
        c.u32Index = r[i].idx;
        {
            int g = IMP_Encoder_GetChnROI(chn, &c);
            printf("[R]   reg%d set=%d get=%d en=%d rel=%d qp=%d (%d,%d)-(%d,%d)\n", r[i].idx, ret, g, c.bEnable,
                   c.bRelatedQp, c.s32Qp, c.rect.p0.x, c.rect.p0.y, c.rect.p1.x, c.rect.p1.y);
        }
    }
#endif
}

int main(int argc, char **argv)
{
    int W, H, fps, kbps, qp, minqp, maxqp, i, fixqp, chn = 0;
    const char *rc = getenv("ROITEST_RC"), *outdir;
    IMPSensorInfo sn;
    IMPFSChnAttr fa;
    IMPCell fs = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };
    unsigned char *buf = malloc(2 << 20);

    if (argc < 7) { fprintf(stderr, "usage: %s SENSOR ADDR W H OUTDIR label:secs:spec...\n", argv[0]); return 2; }
    W = atoi(argv[3]); H = atoi(argv[4]); outdir = argv[5];
    fps = getenv("ROITEST_FPS") ? atoi(getenv("ROITEST_FPS")) : 15;
    kbps = getenv("ROITEST_KBPS") ? atoi(getenv("ROITEST_KBPS")) : 2000;
    qp = getenv("ROITEST_QP") ? atoi(getenv("ROITEST_QP")) : 30;
    minqp = getenv("ROITEST_MINQP") ? atoi(getenv("ROITEST_MINQP")) : 15;
    maxqp = getenv("ROITEST_MAXQP") ? atoi(getenv("ROITEST_MAXQP")) : 45;
    fixqp = rc && !strcmp(rc, "fixqp");
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&sn, 0, sizeof(sn));
    strncpy(sn.name, argv[1], sizeof(sn.name) - 1);
    sn.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sn.i2c.type, argv[1], sizeof(sn.i2c.type) - 1);
    sn.i2c.addr = (int)strtol(argv[2], NULL, 0);
    sn.rst_gpio = sn.pwdn_gpio = sn.power_gpio = -1;
#ifdef PLATFORM_T41
    CK(IMP_ISP_Open(), "ISP_Open");
    CK(IMP_ISP_AddSensor(IMPVI_MAIN, &sn), "AddSensor");
    CK(IMP_ISP_EnableSensor(IMPVI_MAIN, &sn), "EnableSensor");
#else
    CK(IMP_ISP_Open(), "ISP_Open");
    CK(IMP_ISP_AddSensor(&sn), "AddSensor");
    CK(IMP_ISP_EnableSensor(), "EnableSensor");
#endif
    CK(IMP_System_Init(), "System_Init");
    CK(IMP_ISP_EnableTuning(), "EnableTuning");

    memset(&fa, 0, sizeof(fa));
    fa.picWidth = W; fa.picHeight = H; fa.pixFmt = PIX_FMT_NV12;
    fa.outFrmRateNum = fps; fa.outFrmRateDen = 1; fa.nrVBs = getenv("ROITEST_NRVB") ? atoi(getenv("ROITEST_NRVB")) : 2; fa.type = FS_PHY_CHANNEL;
    if (getenv("ROITEST_OUT")) {   /* WxH: scale the channel (encoder size) */
        int ow = 0, oh = 0;
        sscanf(getenv("ROITEST_OUT"), "%dx%d", &ow, &oh);
        if (ow > 0 && oh > 0) {
            fa.scaler.enable = 1; fa.scaler.outwidth = ow; fa.scaler.outheight = oh;
            W = ow; H = oh;
        }
    }
    CK(IMP_FrameSource_CreateChn(0, &fa), "FS_CreateChn");
    if (getenv("ROITEST_FS_FIRST")) {   /* apitest order: FS enabled before the encoder exists */
        CK(IMP_FrameSource_EnableChn(0), "FS_Enable");
        sleep(2);
    }

#ifdef ENC_NEW
    {
        IMPEncoderChnAttr a;
        memset(&a, 0, sizeof(a));
        CK(IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_AVC_HIGH, fixqp ? IMP_ENC_RC_MODE_FIXQP : IMP_ENC_RC_MODE_CBR,
                                       W, H, fps, 1, fps * 2, 2, fixqp ? qp : -1, fixqp ? 0 : kbps), "SetDefaultParam");
        if (fixqp) {
            a.rcAttr.attrRcMode.attrFixQp.iInitialQP = qp;
#ifdef PLATFORM_T41
            a.rcAttr.attrRcMode.attrFixQp.iMinQP = qp;
            a.rcAttr.attrRcMode.attrFixQp.iMaxQP = qp;
#endif
        } else {
            a.rcAttr.attrRcMode.attrCbr.iMinQP = minqp;
            a.rcAttr.attrRcMode.attrCbr.iMaxQP = maxqp;
        }
        CK(IMP_Encoder_CreateChn(chn, &a), "Enc_CreateChn");
    }
#else
    {
        IMPEncoderCHNAttr a;
        memset(&a, 0, sizeof(a));
        a.encAttr.picWidth = W; a.encAttr.picHeight = H;
        a.encAttr.enType = PT_H264; a.encAttr.profile = 1;
        a.rcAttr.outFrmRate.frmRateNum = fps; a.rcAttr.outFrmRate.frmRateDen = 1;
        a.rcAttr.maxGop = fps * 2;
        if (fixqp) {
            a.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
            a.rcAttr.attrRcMode.attrH264FixQp.qp = qp;
        } else {
            a.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_CBR;
            a.rcAttr.attrRcMode.attrH264Cbr.maxQp = maxqp; a.rcAttr.attrRcMode.attrH264Cbr.minQp = minqp;
            a.rcAttr.attrRcMode.attrH264Cbr.outBitRate = kbps;
            a.rcAttr.attrRcMode.attrH264Cbr.frmQPStep = 3; a.rcAttr.attrRcMode.attrH264Cbr.gopQPStep = 15;
        }
        a.rcAttr.attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
        a.rcAttr.attrHSkip.hSkipAttr.m = fps * 2 - 1; a.rcAttr.attrHSkip.hSkipAttr.n = 1;
        a.rcAttr.attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
        CK(IMP_Encoder_CreateChn(chn, &a), "Enc_CreateChn");
    }
#endif
    CK(IMP_Encoder_CreateGroup(0), "CreateGroup");
    CK(IMP_Encoder_RegisterChn(0, chn), "RegisterChn");
    CK(IMP_System_Bind(&fs, &enc), "Bind");
    if (!getenv("ROITEST_FS_FIRST")) CK(IMP_FrameSource_EnableChn(0), "FS_Enable");
    CK(IMP_Encoder_StartRecvPic(chn), "StartRecvPic");
    printf("[T] roitest %dx%d %s %s\n", W, H, fixqp ? "FixQP" : "CBR", fixqp ? "" : "");
    sleep(3);

    for (i = 6; i < argc; i++) {
        char *lab = strdup(argv[i]), *secs, *spec;
        Reg reg[10];
        int n, idr_req = 1, frames = 0, idrs = 0;
        long bytes = 0, idr_b = 0, p_b = 0;
        int64_t t_end, t0;
        char path[512];
        FILE *f;

        secs = strchr(lab, ':'); *secs++ = 0;
        spec = strchr(secs, ':'); *spec++ = 0;
        if (lab[0] == '~') { idr_req = 0; lab++; }
        n = parse_spec(spec, reg);
        apply_roi(chn, reg, n);
        if (idr_req) IMP_Encoder_RequestIDR(chn);
        snprintf(path, sizeof(path), "%s/%s.h264", outdir, lab);
        f = fopen(path, "wb");
        t0 = now_ms();
        t_end = t0 + 1000 * atoi(secs);
        while (now_ms() < t_end) {
            IMPEncoderStream s;
            size_t len = 0;
            uint32_t k;
            if (IMP_Encoder_PollingStream(chn, 1000) != 0) continue;
            memset(&s, 0, sizeof(s));
            if (IMP_Encoder_GetStream(chn, &s, true) != 0) continue;
            for (k = 0; k < s.packCount; k++) {
                const IMPEncoderPack *p = &s.pack[k];
#ifdef ENC_NEW
                const unsigned char *base = (const unsigned char *)(uintptr_t)s.virAddr;
                size_t rem = s.streamSize > p->offset ? s.streamSize - p->offset : 0;
                if (!p->length || len + p->length > (2u << 20)) continue;
                if (rem && rem < p->length) {
                    memcpy(buf + len, base + p->offset, rem);
                    memcpy(buf + len + rem, base, p->length - rem);
                } else
                    memcpy(buf + len, base + p->offset, p->length);
#else
                if (!p->length || len + p->length > (2u << 20)) continue;
                memcpy(buf + len, (const void *)(uintptr_t)p->virAddr, p->length);
#endif
                len += p->length;
            }
            IMP_Encoder_ReleaseStream(chn, &s);
            if (f) fwrite(buf, 1, len, f);
            frames++; bytes += len;
            if (nal_has_idr(buf, len)) { idrs++; idr_b += len; } else p_b += len;
        }
        if (f) fclose(f);
        printf("[R] %s frames=%d bytes=%ld kbps=%.0f idrs=%d idr_avg=%ld p_avg=%ld\n", lab, frames, bytes,
               bytes * 8.0 / (double)(now_ms() - t0), idrs, idrs ? idr_b / idrs : 0,
               frames > idrs ? p_b / (frames - idrs) : 0);
    }
    apply_roi(chn, NULL, 0);
    IMP_Encoder_StopRecvPic(chn);
    IMP_FrameSource_DisableChn(0);
    IMP_System_UnBind(&fs, &enc);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    IMP_Encoder_DestroyGroup(0);
    IMP_FrameSource_DestroyChn(0);
    IMP_System_Exit();
    IMP_ISP_DisableTuning();
#ifdef PLATFORM_T41
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN, &sn);
#else
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&sn);
#endif
    IMP_ISP_Close();
    return 0;
}
