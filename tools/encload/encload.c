/* encload - T40 encoder load client for CPU samples (no RTSP, nothing stored).
 *
 *   encload SECS [day|night|auto]
 *
 * ISP open (Eufy .184 board: sc830ai @0x30, i2c adapter 1, reset gpio 91, MCLK1, MIPI CSI0, default boot 4, the
 * description the vendor ko needs), FrameSource ch0 1920x1080 NV12 scaled from the sensor, 20 fps; one H.264
 * channel (High, CBR 2048 kbit/s, GOP 40, QP 20..45) bound to it; every stream packet is read and dropped for
 * SECS seconds.  day/night force IMP_ISP_Tuning_SetISPRunningMode, auto leaves it.  The same binary runs on the
 * open stack and, through LD_LIBRARY_PATH, on the vendor libimp 1.3.1, so both encode the same configuration.
 * Stop the streamer first.  Prints frames, bytes and fps at the end.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#define EW 1920
#define EH 1080
#define EFPS 20
#define KBPS 2048
#define GOP 40

__attribute__((weak)) int IMP_Log_Get_Option(void) { return 0; }

#define CK(e, m) do { int r_ = (e); if (r_ != 0) { printf("[E] %s -> %d\n", m, r_); return 1; } } while (0)

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    IMPSensorInfo s;
    IMPFSChnAttr fs;
    IMPEncoderChnAttr a;
    IMPCell fsc = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };
    int secs = argc > 1 ? atoi(argv[1]) : 60, frames = 0;
    const char *mode = argc > 2 ? argv[2] : "auto";
    FILE *out = argc > 3 ? fopen(argv[3], "wb") : NULL;   /* raw Annex B, for before/after byte compares */
    long long bytes = 0;
    int64_t t0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&s, 0, sizeof(s));
    strcpy(s.name, "sc830ai");
    s.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strcpy(s.i2c.type, "sc830ai");
    s.i2c.addr = 0x30;
    s.i2c.i2c_adapter_id = 1;
    s.rst_gpio = 91; s.pwdn_gpio = -1; s.power_gpio = -1;
    s.video_interface = IMPISP_SENSOR_VI_MIPI_CSI0;
    s.mclk = IMPISP_SENSOR_MCLK1;
    s.default_boot = 4;

    CK(IMP_ISP_Open(), "IMP_ISP_Open");
    CK(IMP_ISP_AddSensor(IMPVI_MAIN, &s), "IMP_ISP_AddSensor");
    CK(IMP_ISP_EnableSensor(IMPVI_MAIN, &s), "IMP_ISP_EnableSensor");
    CK(IMP_System_Init(), "IMP_System_Init");
    CK(IMP_ISP_EnableTuning(), "IMP_ISP_EnableTuning");
    if (!strcmp(mode, "day") || !strcmp(mode, "night")) {
        IMPISPRunningMode m = !strcmp(mode, "night") ? IMPISP_RUNNING_MODE_NIGHT : IMPISP_RUNNING_MODE_DAY;

        printf("[I] SetISPRunningMode(%s) -> %d\n", mode, IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &m));
    }

    memset(&fs, 0, sizeof(fs));
    fs.picWidth = EW; fs.picHeight = EH; fs.pixFmt = PIX_FMT_NV12;
    fs.outFrmRateNum = EFPS; fs.outFrmRateDen = 1; fs.nrVBs = 2; fs.type = FS_PHY_CHANNEL;
    fs.scaler.enable = 1; fs.scaler.outwidth = EW; fs.scaler.outheight = EH;
    CK(IMP_FrameSource_CreateChn(0, &fs), "IMP_FrameSource_CreateChn");

    memset(&a, 0, sizeof(a));
    CK(IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_AVC_HIGH, IMP_ENC_RC_MODE_CBR, EW, EH, EFPS, 1, GOP, 2, -1, KBPS),
       "IMP_Encoder_SetDefaultParam");
    a.rcAttr.attrRcMode.attrCbr.uTargetBitRate = KBPS;
    a.rcAttr.attrRcMode.attrCbr.iMinQP = 20; a.rcAttr.attrRcMode.attrCbr.iMaxQP = 45;
    CK(IMP_Encoder_CreateGroup(0), "IMP_Encoder_CreateGroup");
    CK(IMP_Encoder_CreateChn(0, &a), "IMP_Encoder_CreateChn");
    CK(IMP_Encoder_RegisterChn(0, 0), "IMP_Encoder_RegisterChn");
    CK(IMP_System_Bind(&fsc, &enc), "IMP_System_Bind");
    CK(IMP_FrameSource_EnableChn(0), "IMP_FrameSource_EnableChn");
    CK(IMP_Encoder_StartRecvPic(0), "IMP_Encoder_StartRecvPic");
    printf("[I] encoding H.264 %dx%d %d fps CBR %d kbit/s GOP %d for %d s\n", EW, EH, EFPS, KBPS, GOP, secs);

    t0 = now_ms();
    while (now_ms() - t0 < (int64_t)secs * 1000) {
        IMPEncoderStream st;
        unsigned k;

        if (IMP_Encoder_PollingStream(0, 1000) != 0)
            continue;
        if (IMP_Encoder_GetStream(0, &st, 1) != 0)
            continue;
        for (k = 0; k < st.packCount; k++) {
            bytes += st.pack[k].length;
            if (out) {  /* T40 packs are offsets into the stream ring (may wrap) */
                const uint8_t *base = (const uint8_t *)(uintptr_t)st.virAddr;
                uint32_t off = st.pack[k].offset, len = st.pack[k].length;

                if (off + len <= st.streamSize) {
                    fwrite(base + off, 1, len, out);
                } else {
                    fwrite(base + off, 1, st.streamSize - off, out);
                    fwrite(base, 1, len - (st.streamSize - off), out);
                }
            }
        }
        frames++;
        IMP_Encoder_ReleaseStream(0, &st);
    }
    printf("[R] %d frames, %.1f fps, %lld bytes, %.0f kbit/s\n", frames, frames * 1000.0 / (now_ms() - t0), bytes,
           bytes * 8.0 / (now_ms() - t0));

    IMP_Encoder_StopRecvPic(0);
    IMP_FrameSource_DisableChn(0);
    IMP_System_UnBind(&fsc, &enc);
    IMP_Encoder_UnRegisterChn(0);
    IMP_Encoder_DestroyChn(0);
    IMP_Encoder_DestroyGroup(0);
    IMP_FrameSource_DestroyChn(0);
    IMP_System_Exit();
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN, &s);
    IMP_ISP_DisableTuning();
    IMP_ISP_Close();
    return 0;
}
