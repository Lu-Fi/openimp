/* t30_encode: short H.264 capture through whatever libimp.so the loader finds
 * (the T30 test kit points LD_LIBRARY_PATH at the OpenIMP library).
 *
 *   t30_encode SENSOR I2C_ADDR W H SECONDS OUTFILE
 *
 * FrameSource ch0 (sensor size) -> encoder group 0 -> H.264 CBR, 15 fps, GOP 30.
 * Writes the Annex-B stream to OUTFILE and prints one summary line per second
 * plus a final "[R]" line (frames, bytes, IDR count, measured fps).
 * Built against the T30 SDK 1.0.5 vendor headers (IMPEncoderCHNAttr family).
 * No audio, no files other than OUTFILE, no change to any configuration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#define FPS 15
#define GOP 30

static volatile const char *g_step = "start";
static void on_alarm(int s)
{
    static const char m[] = "[E] t30_encode watchdog: stuck in step: ";
    (void)s;
    if (write(1, m, sizeof(m) - 1) < 0) _exit(3);
    if (write(1, (const char *)g_step, strlen((const char *)g_step)) < 0) _exit(3);
    if (write(1, "\n", 1) < 0) _exit(3);
    _exit(3);
}
#define STEP(s) do { g_step = s; alarm(30); } while (0)
#define CK(call) do { STEP(#call); int r_ = (call); printf("[S] %-40s -> %d\n", #call, r_); \
    if (r_ < 0) { rc = 1; goto out; } } while (0)

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    IMPSensorInfo sensor;
    IMPFSChnAttr fs;
    IMPEncoderCHNAttr ea;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_ENC, 0, 0 };
    FILE *fp;
    int w, h, secs, rc = 0, up = 0, i;
    long frames = 0, idr = 0, bytes = 0, sec_bytes = 0, sec_frames = 0;
    int64_t t0, tend, tsec;

    if (argc < 7) {
        fprintf(stderr, "usage: %s SENSOR I2C_ADDR W H SECONDS OUTFILE\n", argv[0]);
        return 2;
    }
    w = atoi(argv[3]); h = atoi(argv[4]); secs = atoi(argv[5]);
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, on_alarm);
    fp = fopen(argv[6], "wb");
    if (!fp) { perror(argv[6]); return 2; }

    memset(&sensor, 0, sizeof(sensor));
    strncpy(sensor.name, argv[1], sizeof(sensor.name) - 1);
    sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(sensor.i2c.type, argv[1], sizeof(sensor.i2c.type) - 1);
    sensor.i2c.addr = (int)strtol(argv[2], NULL, 0);
    sensor.rst_gpio = sensor.pwdn_gpio = sensor.power_gpio = -1;

    CK(IMP_ISP_Open());
    up |= 1;
    CK(IMP_ISP_AddSensor(&sensor));
    up |= 2;
    CK(IMP_ISP_EnableSensor());
    up |= 4;
    CK(IMP_System_Init());
    up |= 8;
    CK(IMP_ISP_EnableTuning());
    up |= 16;
    CK(IMP_ISP_Tuning_SetSensorFPS(FPS, 1));

    memset(&fs, 0, sizeof(fs));
    fs.picWidth = w; fs.picHeight = h; fs.pixFmt = PIX_FMT_NV12;
    fs.outFrmRateNum = FPS; fs.outFrmRateDen = 1; fs.nrVBs = 2;
    fs.type = FS_PHY_CHANNEL;
    CK(IMP_FrameSource_CreateChn(0, &fs));
    up |= 32;

    memset(&ea, 0, sizeof(ea));
    ea.encAttr.picWidth = w; ea.encAttr.picHeight = h;
    ea.encAttr.enType = PT_H264; ea.encAttr.profile = 1;
    ea.encAttr.userData.maxUserDataCnt = 2;
    ea.encAttr.userData.maxUserDataSize = 64;
    ea.rcAttr.outFrmRate.frmRateNum = FPS; ea.rcAttr.outFrmRate.frmRateDen = 1;
    ea.rcAttr.maxGop = GOP;
    ea.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_CBR;
    ea.rcAttr.attrRcMode.attrH264Cbr.maxQp = 45;
    ea.rcAttr.attrRcMode.attrH264Cbr.minQp = 15;
    ea.rcAttr.attrRcMode.attrH264Cbr.outBitRate = (w >= 1280) ? 2000 : 800; /* kbit/s */
    ea.rcAttr.attrRcMode.attrH264Cbr.frmQPStep = 3;
    ea.rcAttr.attrRcMode.attrH264Cbr.gopQPStep = 15;
    ea.rcAttr.attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
    ea.rcAttr.attrHSkip.hSkipAttr.m = GOP - 1;
    ea.rcAttr.attrHSkip.hSkipAttr.n = 1;
    ea.rcAttr.attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;

    CK(IMP_Encoder_CreateGroup(0));
    up |= 64;
    CK(IMP_Encoder_CreateChn(0, &ea));
    up |= 128;
    CK(IMP_Encoder_RegisterChn(0, 0));
    up |= 256;
    CK(IMP_System_Bind(&src, &dst));
    up |= 512;
    CK(IMP_FrameSource_EnableChn(0));
    up |= 1024;
    sleep(2); /* AE/AWB settle */
    CK(IMP_Encoder_StartRecvPic(0));
    up |= 2048;

    t0 = tsec = now_us();
    tend = t0 + (int64_t)secs * 1000000;
    while (now_us() < tend) {
        IMPEncoderStream st;
        unsigned k;

        STEP("IMP_Encoder_PollingStream");
        if (IMP_Encoder_PollingStream(0, 2000) != 0) {
            printf("[E] no stream frame within 2 s (frames so far: %ld)\n", frames);
            rc = 1; break;
        }
        STEP("IMP_Encoder_GetStream");
        memset(&st, 0, sizeof(st));
        if (IMP_Encoder_GetStream(0, &st, true) != 0) { printf("[E] GetStream failed\n"); rc = 1; break; }
        for (k = 0; k < st.packCount; k++) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)st.pack[k].virAddr;
            size_t n = st.pack[k].length;
            if (fwrite(p, 1, n, fp) != n) { printf("[E] short write\n"); rc = 1; }
            bytes += n; sec_bytes += n;
            /* IDR slice = NAL type 5 behind a 3/4-byte start code */
            if (n > 4 && p[0] == 0 && p[1] == 0 && (((p[2] == 1) && (p[3] & 31) == 5) ||
                ((p[2] == 0) && p[3] == 1 && (p[4] & 31) == 5)))
                idr++;
        }
        frames++; sec_frames++;
        STEP("IMP_Encoder_ReleaseStream");
        IMP_Encoder_ReleaseStream(0, &st);
        if (now_us() - tsec >= 1000000) {
            printf("[P] %ld frames, %ld bytes in the last second\n", sec_frames, sec_bytes);
            sec_frames = sec_bytes = 0; tsec += 1000000;
        }
    }
    {
        double dt = (now_us() - t0) / 1e6;
        printf("[R] t30_encode %dx%d: %ld frames, %ld bytes, %ld IDR, %.1f fps over %.1f s -> %s\n",
               w, h, frames, bytes, idr, dt > 0 ? frames / dt : 0.0, dt, argv[6]);
        if (frames == 0) rc = 1;
    }
out:
    STEP("teardown");
    if (up & 2048) IMP_Encoder_StopRecvPic(0);
    if (up & 1024) IMP_FrameSource_DisableChn(0);
    if (up & 512) IMP_System_UnBind(&src, &dst);
    if (up & 256) IMP_Encoder_UnRegisterChn(0);
    if (up & 128) IMP_Encoder_DestroyChn(0);
    if (up & 64) IMP_Encoder_DestroyGroup(0);
    if (up & 32) IMP_FrameSource_DestroyChn(0);
    if (up & 16) IMP_ISP_DisableTuning();
    if (up & 8) IMP_System_Exit();
    if (up & 4) IMP_ISP_DisableSensor();
    if (up & 2) IMP_ISP_DelSensor(&sensor);
    if (up & 1) IMP_ISP_Close();
    fclose(fp);
    (void)i;
    alarm(0);
    printf("[R] exit %d\n", rc);
    return rc;
}
