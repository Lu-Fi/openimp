/* cropt - T23 IMP_ISP_Tuning_SetFrontCrop on-device test (valid and invalid windows).
 *
 *   cropt SENSOR I2C_ADDR W H OUTDIR
 *
 * FS ch0 = W/2 x H/2 and ch1 = 320x180, both scaled from the sensor picture.  Each case sets a front
 * crop window, prints the return value and the Get readback, then grabs one frame from each channel
 * (raw NV12, OUTDIR/<case>.ch<N>.nv12).  A frame that does not arrive within 3 s prints NOFRAME.
 * Valid = the window is at least as large as every channel output (the MSCA cannot upscale);
 * invalid windows must return an error and keep the pictures flowing.
 * Env: CROPT_CASES=name,name (subset), CROPT_CH0=WxH / CROPT_CH1=WxH (channel sizes, default W/2xH/2 and
 *      320x180; T10/T20 "shrink only": use CROPT_CH0=<sensor size> CROPT_CH1=640x360), CROPT_STOP=1 (exit 3 at the first stall).  Stop the streamer first.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>

#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21)
/* OpenIMP extension (not in the vendor headers): 20 bytes, enable in the low byte */
typedef struct { unsigned char fcrop_enable; unsigned int fcrop_top, fcrop_left, fcrop_width, fcrop_height; } IMPISPFrontCrop;
int IMP_ISP_Tuning_SetFrontCrop(IMPISPFrontCrop *);
int IMP_ISP_Tuning_GetFrontCrop(IMPISPFrontCrop *);
#endif
__attribute__((weak)) int IMP_Log_Get_Option(void) { return 0; }
__attribute__((weak)) void imp_log_fun(int level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; (void)fmt; }

static int grab(int ch, const char *path)
{
    int t, got = 0;
    time_t t0 = time(NULL);
    for (t = 0; t < 40; t++) {
        IMPFrameInfo *fr = NULL;
        if (IMP_FrameSource_GetFrame(ch, &fr) == 0 && fr) {
            unsigned w = fr->width, h = fr->height, ah = (h + 15) & ~15u;
            if (++got < 4) {   /* the first frames may predate the crop: save the 4th */
                IMP_FrameSource_ReleaseFrame(ch, fr);
                continue;
            }
            const unsigned char *p = (const unsigned char *)(uintptr_t)fr->virAddr;
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(p, 1, w * h, f); fwrite(p + (size_t)w * ah, 1, w * h / 2, f); fclose(f); }
            printf(" ch%d=%ux%u(%lds)", ch, w, h, (long)(time(NULL) - t0));
            IMP_FrameSource_ReleaseFrame(ch, fr);
            return 0;
        }
        if (t > 12) break;
        usleep(100000);
    }
    printf(" ch%d=NOFRAME", ch);
    return -1;
}

typedef struct { const char *name; int l, t, w, h; int valid; } Case;

int main(int argc, char **argv)
{
    int W, H, i, bad = 0, c0w, c0h, c1w, c1h;
    IMPSensorInfo sn;
    IMPFSChnAttr f0, f1;
    char path[600];
    const char *only = getenv("CROPT_CASES");

    if (argc < 6) { fprintf(stderr, "usage: %s SENSOR ADDR W H OUTDIR\n", argv[0]); return 2; }
    W = atoi(argv[3]); H = atoi(argv[4]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&sn, 0, sizeof(sn));
    strncpy(sn.name, argv[1], sizeof(sn.name) - 1);
    sn.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sn.i2c.type, argv[1], sizeof(sn.i2c.type) - 1);
    sn.i2c.addr = (int)strtol(argv[2], NULL, 0);
    sn.rst_gpio = sn.pwdn_gpio = sn.power_gpio = -1;
    if (IMP_ISP_Open() < 0 || IMP_ISP_AddSensor(&sn) < 0 || IMP_ISP_EnableSensor() < 0 ||
        IMP_System_Init() < 0 || IMP_ISP_EnableTuning() < 0) { printf("[E] ISP init\n"); return 1; }
    IMP_ISP_Tuning_SetSensorFPS(15, 1);

    {
        const char *e0 = getenv("CROPT_CH0"), *e1 = getenv("CROPT_CH1");
        c0w = W / 2; c0h = H / 2; c1w = 320; c1h = 180;
        if (e0) sscanf(e0, "%dx%d", &c0w, &c0h);
        if (e1) sscanf(e1, "%dx%d", &c1w, &c1h);
    }
    memset(&f0, 0, sizeof(f0));
    f0.picWidth = c0w; f0.picHeight = c0h; f0.pixFmt = PIX_FMT_NV12;
    f0.outFrmRateNum = 15; f0.outFrmRateDen = 1; f0.nrVBs = 2; f0.type = FS_PHY_CHANNEL;
    f0.scaler.enable = !(c0w == W && c0h == H); f0.scaler.outwidth = c0w; f0.scaler.outheight = c0h;   /* no scaler on a full-size channel (T10/T20 ch0) */
    f1 = f0; f1.picWidth = c1w; f1.picHeight = c1h; f1.scaler.outwidth = c1w; f1.scaler.outheight = c1h;
    if (IMP_FrameSource_CreateChn(0, &f0) < 0 || IMP_FrameSource_CreateChn(1, &f1) < 0) { printf("[E] CreateChn\n"); return 1; }
    IMP_FrameSource_SetFrameDepth(0, 1); IMP_FrameSource_SetFrameDepth(1, 1);
    if (IMP_FrameSource_EnableChn(0) < 0 || IMP_FrameSource_EnableChn(1) < 0) { printf("[E] EnableChn\n"); return 1; }
    sleep(4);
    printf("[T] cropt sensor %dx%d, ch0 %dx%d ch1 %dx%d\n", W, H, c0w, c0h, c1w, c1h);

    {
        int w75 = (W * 3 / 4) & ~1, h75 = (H * 3 / 4) & ~1, l8 = (W / 8) & ~1, t8 = (H / 8) & ~1;
        Case cs[] = {
            { "base-off",        0, 0, 0, 0, 1 },
            { "full-en",         0, 0, W, H, 1 },
            { "v1024-center",    ((W - 1024 * W / 1280) / 2) & ~15, ((H - 576 * H / 720) / 2) & ~15, (1024 * W / 1280) & ~15, (576 * H / 720) & ~15, 1 },
            { "v60-aligned",     ((W - 768 * W / 1280) / 2) & ~15, ((H - 432 * H / 720) / 2) & ~15, (768 * W / 1280) & ~15, (432 * H / 720) & ~15, 1 },
            { "v75-center",      l8, t8, w75, h75, 1 },
            { "v75-topleft",     0, 0, w75, h75, 1 },
            { "v75-bottomright", W - w75, H - h75, w75, h75, 1 },
            { "v50-eq-main",     (W / 4) & ~1, (H / 4) & ~1, W / 2, H / 2, 1 },
            { "bad-below-main",  (W / 4) & ~1, (H / 4) & ~1, W / 2 - 2, H / 2 - 2, 0 },
            { "bad-30pct",       (W / 3) & ~1, (H / 3) & ~1, (W * 3 / 10) & ~1, (H * 3 / 10) & ~1, 0 },
            { "bad-beyond-sensor", W / 2, H / 2, w75, h75, 0 },
            { "bad-zero-width",  0, 0, 0, H, 0 },
            { "bad-zero-height", 0, 0, W, 0, 0 },
            { "bad-wider-sensor", 0, 0, W + 64, H, 0 },
            { "v75-center-again", l8, t8, w75, h75, 1 },
            { "final-off",       0, 0, 0, 0, 1 },
        };
        Case wc[64];
        int ncs = (int)(sizeof(cs) / sizeof(cs[0]));
        Case *list = cs;
        const char *ew = getenv("CROPT_WIN");   /* "l,t,w,h;l,t,w,h": run only these (all expected valid) */

        if (ew) {
            char *dup = strdup(ew), *tok, *sp = NULL;
            static char names[64][40];
            ncs = 0;
            wc[ncs++] = cs[0];
            for (tok = strtok_r(dup, ";", &sp); tok && ncs < 63; tok = strtok_r(NULL, ";", &sp)) {
                int l, t, w, h;
                if (sscanf(tok, "%d,%d,%d,%d", &l, &t, &w, &h) != 4) continue;
                snprintf(names[ncs], sizeof(names[ncs]), "w%d_%d_%dx%d", l, t, w, h);
                wc[ncs].name = names[ncs]; wc[ncs].l = l; wc[ncs].t = t; wc[ncs].w = w; wc[ncs].h = h; wc[ncs].valid = 1;
                ncs++;
            }
            list = wc;
        }
        for (i = 0; i < ncs; i++) {
            IMPISPFrontCrop fc, g;
            int r, e;
            if (!ew && only && !strstr(only, list[i].name)) continue;
            memset(&fc, 0, sizeof(fc)); memset(&g, 0, sizeof(g));
            fc.fcrop_enable = (list[i].valid && !list[i].w) ? 0 : 1;
            fc.fcrop_left = list[i].l; fc.fcrop_top = list[i].t; fc.fcrop_width = list[i].w; fc.fcrop_height = list[i].h;
            errno = 0;
            r = IMP_ISP_Tuning_SetFrontCrop(&fc);
            e = errno;
            IMP_ISP_Tuning_GetFrontCrop(&g);
            printf("[R] %s %s set=%d(errno %d) get=en%d %u,%u %ux%u |", list[i].name, list[i].valid ? "VALID" : "INVALID", r, e,
                   g.fcrop_enable, g.fcrop_left, g.fcrop_top, g.fcrop_width, g.fcrop_height);
            usleep(1500000);
            snprintf(path, sizeof(path), "%s/%s.ch0.nv12", argv[5], list[i].name);
            if (grab(0, path) < 0) bad++;
            snprintf(path, sizeof(path), "%s/%s.ch1.nv12", argv[5], list[i].name);
            if (grab(1, path) < 0) bad++;
            printf(" -> %s\n", (list[i].valid ? r == 0 : r != 0) ? "OK" : "UNEXPECTED");
            if (getenv("CROPT_STOP") && list[i].valid && r == 0 && bad) { printf("[T] stalled after %s\n", list[i].name); return 3; }
        }
    }
    printf("[T] done noframe=%d\n", bad);
    IMP_FrameSource_DisableChn(1); IMP_FrameSource_DisableChn(0);
    IMP_FrameSource_DestroyChn(1); IMP_FrameSource_DestroyChn(0);
    return bad ? 1 : 0;
}
