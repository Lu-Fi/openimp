/*
 * Minimal T32 stream bring-up for the ioctl trace oracle (#287).
 *
 * Built against the OpenIMP public headers with PLATFORM_T32 and linked
 * against whichever libimp is installed (vendor 2.2.0 for the reference,
 * OpenIMP later), it runs: ISP open, AddSensor, EnableSensor, System_Init,
 * EnableTuning, FrameSource channel 0 (1920x1080 NV12), one encoder
 * channel bound to it (h264 default, jpeg or h265 by argument), N streams,
 * then the full teardown.  Each API step is announced to the LD_PRELOAD
 * recorder (oi_trace_mark) so the ioctl log is phase-tagged.
 *
 * "isp" runs only the ISP / sensor lifecycle (#288): ISP open, AddSensor,
 * EnableSensor, System_Init, the /proc/jz/isp/isp-m0 status lines
 * (prefix T32P), then System_Exit, DisableSensor, DelSensor, ISP close;
 * "isp N" repeats the whole cycle N times in one process.
 * "bin" is "isp" plus a night and a day IMP_ISP_Tuning_SetISPRunningMode
 * (IQ-bin switch, #289) between System_Init and the teardown.
 *
 * usage: t32_trace_sample [h264|h265|jpeg|none|isp|bin] [streams] [sensor] [i2c addr]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#if !defined(PLATFORM_T32)
#error "build with -DPLATFORM_T32"
#endif

/* T32 profile words come from imp_encoder.h (type byte JPEG 0, AVC 1,
 * HEVC 2 on PLATFORM_T32, asserted there; #302) */
#define T32_PROFILE_JPEG      IMP_ENC_PROFILE_JPEG
#define T32_PROFILE_AVC_MAIN  IMP_ENC_PROFILE_AVC_MAIN
#define T32_PROFILE_HEVC_MAIN IMP_ENC_PROFILE_HEVC_MAIN

static void (*mark)(const char *);

static void proc_status(void)
{
    char line[160];
    FILE *f = fopen("/proc/jz/isp/isp-m0", "r");

    if (!f) {
        printf("T32P no /proc/jz/isp/isp-m0\n");
        return;
    }
    while (fgets(line, sizeof line, f))
        printf("T32P %s", line);
    fclose(f);
    fflush(stdout);
}

static int step(const char *name, int rc)
{
    printf("T32S %-40s rc=%d\n", name, rc);
    fflush(stdout);
    return rc;
}

#define STEP(call) (mark ? mark(#call) : (void)0, step(#call, (call)))
#define MUST(call) do { if (STEP(call)) goto fail; } while (0)

int main(int argc, char **argv)
{
    const char *codec = argc > 1 ? argv[1] : "h264";
    int streams = argc > 2 ? atoi(argv[2]) : 5;
    const char *sensor = argc > 3 ? argv[3] : "cv8001";
    int addr = argc > 4 ? (int)strtol(argv[4], NULL, 0) : 0x35;
    int bin_test = !strcmp(codec, "bin");
    int isp_only = !strcmp(codec, "isp") || bin_test;
    int enc = strcmp(codec, "none") != 0 && !isp_only;
    IMPSensorInfo si;
    IMPFSChnAttr fs;
    static IMPEncoderCHNAttr ea;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_ENC, 0, 0 };
    IMPEncoderProfile prof = !strcmp(codec, "jpeg") ? T32_PROFILE_JPEG :
                             !strcmp(codec, "h265") ? T32_PROFILE_HEVC_MAIN : T32_PROFILE_AVC_MAIN;
    IMPEncoderRcMode rc = prof == T32_PROFILE_JPEG ? IMP_ENC_RC_MODE_FIXQP : IMP_ENC_RC_MODE_CBR;
    int i, got = 0;

    mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "oi_trace_mark");

    memset(&si, 0, sizeof si);
    snprintf(si.name, sizeof si.name, "%s", sensor);
    si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof si.i2c.type, "%s", sensor);
    si.i2c.addr = addr;
    si.i2c.i2c_adapter = 0;
    si.rst_gpio = si.pwdn_gpio = si.power_gpio = -1;
    si.switch_gpio = -1;

    memset(&fs, 0, sizeof fs);
    fs.picWidth = 1920;
    fs.picHeight = 1080;
    fs.pixFmt = PIX_FMT_NV12;
    fs.scaler.enable = 1;
    fs.scaler.outwidth = 1920;
    fs.scaler.outheight = 1080;
    fs.outFrmRateNum = 15;
    fs.outFrmRateDen = 1;
    fs.nrVBs = 2;
    fs.type = FS_PHY_CHANNEL;

    if (isp_only && streams > 1) {      /* "isp N": N-1 extra full cycles first */
        for (i = 1; i < streams; i++) {
            MUST(IMP_ISP_Open());
            MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
            MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
            MUST(IMP_System_Init());
            proc_status();
            MUST(IMP_System_Exit());
            MUST(IMP_ISP_DisableSensor(IMPVI_MAIN));
            MUST(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
            MUST(IMP_ISP_Close());
        }
    }
    MUST(IMP_ISP_Open());
    MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
    MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
    MUST(IMP_System_Init());
    if (bin_test) {                     /* #289: day/night bin switch */
        IMPISPRunningMode rm = IMPISP_RUNNING_MODE_NIGHT;
        STEP(IMP_ISP_EnableTuning());   /* vendor needs it; OpenIMP T32: stub */
        STEP(IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &rm));
        STEP(IMP_ISP_Tuning_GetISPRunningMode(IMPVI_MAIN, &rm));
        printf("T32S running mode %d\n", (int)rm);
        proc_status();
        rm = IMPISP_RUNNING_MODE_DAY;
        STEP(IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &rm));
        STEP(IMP_ISP_DisableTuning());
    }
    if (isp_only) {
        proc_status();
        STEP(IMP_System_Exit());
        STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
        STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
        STEP(IMP_ISP_Close());
        proc_status();
        printf("T32S end ok=1\n");
        return 0;
    }
    MUST(IMP_ISP_EnableTuning());
    MUST(IMP_FrameSource_CreateChn(0, &fs));
    MUST(IMP_FrameSource_SetChnAttr(0, &fs));
    if (enc) {
        MUST(IMP_Encoder_CreateGroup(0));
        MUST(IMP_Encoder_SetDefaultParam(&ea, prof, rc, 1920, 1080, 15, 1, 15, 0, -1, 1000));
        MUST(IMP_Encoder_CreateChn(0, &ea));
        MUST(IMP_Encoder_RegisterChn(0, 0));
        MUST(IMP_System_Bind(&src, &dst));
    }
    MUST(IMP_FrameSource_EnableChn(0));
    if (enc) {
        MUST(IMP_Encoder_StartRecvPic(0));
        if (mark)
            mark("streams");
        for (i = 0; i < streams; i++) {
            IMPEncoderStream st;
            if (IMP_Encoder_PollingStream(0, 3000) || IMP_Encoder_GetStream(0, &st, 1))
                continue;
            got++;
            IMP_Encoder_ReleaseStream(0, &st);
        }
        printf("T32S streams %d/%d\n", got, streams);
        STEP(IMP_Encoder_StopRecvPic(0));
        STEP(IMP_FrameSource_DisableChn(0));
        STEP(IMP_System_UnBind(&src, &dst));
        STEP(IMP_Encoder_UnRegisterChn(0));
        STEP(IMP_Encoder_DestroyChn(0));
        STEP(IMP_Encoder_DestroyGroup(0));
    } else {
        IMPFrameInfo *frame;
        MUST(IMP_FrameSource_SetFrameDepth(0, 1));
        if (mark)
            mark("frames");
        for (i = 0; i < streams; i++) {
            if (IMP_FrameSource_GetFrame(0, (void **)&frame))
                continue;
            got++;
            IMP_FrameSource_ReleaseFrame(0, frame);
        }
        printf("T32S frames %d/%d\n", got, streams);
        STEP(IMP_FrameSource_SetFrameDepth(0, 0));
        STEP(IMP_FrameSource_DisableChn(0));
    }
    STEP(IMP_FrameSource_DestroyChn(0));
    STEP(IMP_ISP_DisableTuning());
    STEP(IMP_System_Exit());
    STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
    STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
    STEP(IMP_ISP_Close());
    printf("T32S end ok=%d\n", got == streams);
    return got == streams ? 0 : 2;
fail:
    printf("T32S end failed\n");
    return 1;
}
