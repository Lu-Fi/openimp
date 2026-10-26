/* T40 AE API check while the streamer runs (2026-10-26, T40-AE-API).
 * Sends the vendor T40 1.3.1 AE tuning structures with the same isp-m0 tuning ioctl the libimp uses
 * (0xc0105436 {vinum, dir, cid, pointer}); /dev/isp-m0 open/release have no side effects on the open
 * driver, so the running streamer keeps its ISP session.  The picture is measured by the caller
 * (driver frame-3A luma / streamer snapshots).
 * usage: t40_aetune <cmd> [args]
 *   expr                      GetAeExprInfo (canary-checked)
 *   stats                     GetAeStatistics summary
 *   scence                    GetAeScenceAttr
 *   comp <0..255>             SetAeScenceAttr TargetComp (GLOBAL; 128 -> DISABLE)
 *   hlc <0..10> | blc <0..10> SetAeScenceAttr HLC/BLC GLOBAL strength (0 -> DISABLE)
 *   manual <it|0> <ag|0>      SetAeExprInfo manual IT (lines) / AG (x1024), 0 = auto
 *   freeze <0|1>              SetAeExprInfo AeMode
 *   minag <x1024|0>           SetAeExprInfo min analog gain
 *   weight <top|bottom|off>   SetAeWeight (metering rows 0-2 / 12-14)
 *   speed <s> <d>             SetAeSpeed / prints GetAeSpeed
 *   face <l t r b target|off> SetFaceAe / prints GetFaceAe
 *   af <mode> <freq>          SetAntiFlickerAttr / prints Get
 *   thresh <a b c d>          SetStatisConfig AE histThresh
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <imp/imp_isp.h>

struct req { int32_t vinum, dir, cid; uint32_t ptr; };
static int fd;

static int tun(int dir, int cid, void *p)
{
    struct req r = { 0, dir, cid, (uint32_t)(uintptr_t)p };
    int rc = ioctl(fd, 0xc0105436U, &r);
    return rc ? -errno : 0;
}
#define SET(cid, p) tun(0, cid, p)
#define GET(cid, p) tun(1, cid, p)

static void pexpr(void)
{
    static unsigned char b[sizeof(IMPISPAEExprInfo) + 16];
    IMPISPAEExprInfo e;
    int rc, ok = 1, i;

    memset(b, 0xa5, sizeof b);
    rc = GET(0x8000023, b);
    for (i = sizeof(IMPISPAEExprInfo); i < (int)sizeof b; i++) if (b[i] != 0xa5) ok = 0;
    memcpy(&e, b, sizeof e);
    printf("expr rc=%d canary=%s mode=%d itm=%d agm=%d IT=%u AG=%u DG=%u ISPDG=%u minAGm=%d minAG=%u maxITm=%d maxIT=%u maxAGm=%d maxAG=%u totdB=%u EV=%u EVLog2=%u\n",
           rc, ok ? "ok" : "BAD", e.AeMode, e.AeIntegrationTimeMode, e.AeAGainManualMode, e.AeIntegrationTime, e.AeAGain, e.AeDGain, e.AeIspDGain,
           e.AeMinAGainMode, e.AeMinAGain, e.AeMaxIntegrationTimeMode, e.AeMaxIntegrationTime, e.AeMaxAGainMode, e.AeMaxAGain, e.TotalGainDb,
           e.ExposureValue, e.EVLog2);
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "expr";
    int rc = 0;

    fd = open("/dev/isp-m0", O_RDWR);
    if (fd < 0) { perror("isp-m0"); return 2; }
    if (!strcmp(c, "expr")) {
        pexpr();
    } else if (!strcmp(c, "stats")) {
        static IMPISPAEStatisInfo s;
        unsigned long long sum = 0; unsigned nz = 0, i, zmin = ~0u, zmax = 0, h5 = 0;
        rc = GET(0x8000022, &s);
        for (i = 0; i < 256; i++) { sum += s.ae_hist_256bin[i]; nz += s.ae_hist_256bin[i] != 0; }
        for (i = 0; i < 5; i++) h5 += s.ae_hist_5bin[i];
        for (i = 0; i < 225; i++) { unsigned v = ((uint32_t *)s.ae_statis.statis)[i]; if (v < zmin) zmin = v; if (v > zmax) zmax = v; }
        printf("stats rc=%d sum=%llu nz=%u hist5=%u,%u,%u,%u,%u (%u) zones %u..%u [7][7]=%u [0][0]=%u [14][14]=%u\n", rc, sum, nz,
               s.ae_hist_5bin[0], s.ae_hist_5bin[1], s.ae_hist_5bin[2], s.ae_hist_5bin[3], s.ae_hist_5bin[4], h5, zmin, zmax,
               s.ae_statis.statis[7][7], s.ae_statis.statis[0][0], s.ae_statis.statis[14][14]);
    } else if (!strcmp(c, "scence")) {
        IMPISPAEScenceAttr a; memset(&a, 0, sizeof a); rc = GET(0x8000024, &a);
        printf("scence rc=%d hlc %d/%u blc %d/%u comp %d/%u luma %u scene %u stable %d target %u ae_mean %u\n", rc, a.AeHLCEn, a.AeHLCStrength,
               a.AeBLCEn, a.AeBLCStrength, a.AeTargetCompEn, a.AeTargetComp, a.luma, a.luma_scence, a.stable, a.target, a.ae_mean);
    } else if (!strcmp(c, "comp") || !strcmp(c, "hlc") || !strcmp(c, "blc")) {
        IMPISPAEScenceAttr a; unsigned v = argc > 2 ? atoi(argv[2]) : 128;
        memset(&a, 0, sizeof a);
        if (c[0] == 'c') { a.AeTargetCompEn = v == 128 ? IMP_ISP_AE_SCENCE_DISABLE : TISP_AE_SCENCE_GLOBAL_ENABLE; a.AeTargetComp = v; }
        else if (c[0] == 'h') { a.AeHLCEn = v ? TISP_AE_SCENCE_GLOBAL_ENABLE : IMP_ISP_AE_SCENCE_DISABLE; a.AeHLCStrength = v; }
        else { a.AeBLCEn = v ? TISP_AE_SCENCE_GLOBAL_ENABLE : IMP_ISP_AE_SCENCE_DISABLE; a.AeBLCStrength = v; }
        rc = SET(0x8000024, &a);
        printf("%s %u rc=%d\n", c, v, rc);
    } else if (!strcmp(c, "manual") || !strcmp(c, "freeze") || !strcmp(c, "minag")) {
        IMPISPAEExprInfo e; memset(&e, 0, sizeof e);
        if (c[0] == 'm' && c[1] == 'a') {
            unsigned it = argc > 2 ? atoi(argv[2]) : 0, ag = argc > 3 ? atoi(argv[3]) : 0;
            if (it) { e.AeIntegrationTimeMode = IMPISP_TUNING_OPS_TYPE_MANUAL; e.AeIntegrationTime = it; }
            if (ag) { e.AeAGainManualMode = IMPISP_TUNING_OPS_TYPE_MANUAL; e.AeAGain = ag; }
        } else if (c[0] == 'f') {
            e.AeMode = argc > 2 && atoi(argv[2]) ? IMPISP_TUNING_OPS_TYPE_MANUAL : IMPISP_TUNING_OPS_TYPE_AUTO;
        } else {
            unsigned ag = argc > 2 ? atoi(argv[2]) : 0;
            if (ag) { e.AeMinAGainMode = IMPISP_TUNING_OPS_TYPE_MANUAL; e.AeMinAGain = ag; }
        }
        rc = SET(0x8000023, &e);
        printf("%s rc=%d\n", c, rc);
        pexpr();
    } else if (!strcmp(c, "weight")) {
        IMPISPAEWeightAttr w; int k; const char *m = argc > 2 ? argv[2] : "off";
        memset(&w, 0, sizeof w);
        if (strcmp(m, "off")) {
            w.roi_enable = IMPISP_TUNING_OPS_MODE_ENABLE;
            for (k = 0; k < 225; k++) ((unsigned char *)w.ae_roi.weight)[k] = (m[0] == 't' ? k < 45 : k >= 180) ? 8 : 0;
        }
        rc = SET(0x8000021, &w);
        memset(&w, 0, sizeof w); GET(0x8000021, &w);
        printf("weight %s rc=%d readback [0][0]=%u [14][14]=%u\n", m, rc, w.ae_roi.weight[0][0], w.ae_roi.weight[14][14]);
    } else if (!strcmp(c, "speed")) {
        IMPISPAeSpeedAttr s;
        if (argc > 3) { s.step_s = atoi(argv[2]); s.step_d = atoi(argv[3]); rc = SET(0x800009a, &s); }
        memset(&s, 0, sizeof s); GET(0x800009a, &s);
        printf("speed rc=%d readback %u/%u\n", rc, s.step_s, s.step_d);
    } else if (!strcmp(c, "face")) {
        IMPISPFaceAttr f; memset(&f, 0, sizeof f);
        if (argc > 6) { f.enable = IMPISP_TUNING_OPS_MODE_ENABLE; f.left = atoi(argv[2]); f.top = atoi(argv[3]); f.right = atoi(argv[4]); f.bottom = atoi(argv[5]); f.target = atoi(argv[6]); }
        rc = SET(0x8000035, &f);
        memset(&f, 0, sizeof f); GET(0x8000035, &f);
        printf("face rc=%d readback en%d %u,%u-%u,%u target %u\n", rc, f.enable, f.left, f.top, f.right, f.bottom, f.target);
    } else if (!strcmp(c, "af")) {
        IMPISPAntiflickerAttr a;
        if (argc > 3) { a.mode = atoi(argv[2]); a.freq = atoi(argv[3]); rc = SET(0x8000026, &a); }
        memset(&a, 0, sizeof a); GET(0x8000026, &a);
        printf("af rc=%d readback mode %d freq %u\n", rc, a.mode, a.freq);
    } else if (!strcmp(c, "thresh")) {
        IMPISPStatisConfig s; memset(&s, 0, sizeof s); GET(0x8000000, &s);
        if (argc > 5) { s.ae.ae_sta_en = IMPISP_TUNING_OPS_MODE_ENABLE; s.ae.histThresh[0] = atoi(argv[2]); s.ae.histThresh[1] = atoi(argv[3]);
                        s.ae.histThresh[2] = atoi(argv[4]); s.ae.histThresh[3] = atoi(argv[5]); rc = SET(0x8000000, &s); }
        memset(&s, 0, sizeof s); GET(0x8000000, &s);
        printf("thresh rc=%d readback %u/%u/%u/%u\n", rc, s.ae.histThresh[0], s.ae.histThresh[1], s.ae.histThresh[2], s.ae.histThresh[3]);
    }
    close(fd);
    return rc ? 1 : 0;
}
