/*
 * T33 AE exposure info probe (#527): ISP + sensor bring-up, FrameSource
 * channel 0 (1920x1080 NV12) running, then IMP_ISP_Tuning_GetAeExprInfo
 * every T33A_MS ms (default 500) for N reads, printing the live words a
 * streamer reads for day/night: AeIntegrationTime (+24), AeAGain (+28),
 * AeDGain (+32), AeIspDGain (+36), AeMinIntegrationTime (+72),
 * AeMaxIntegrationTime (+88), AeMaxAGain (+92), plus all 51 words once.
 * Built against the vendor T33 2.0.2.1 headers; the same binary runs on the
 * vendor libimp and on OpenIMP (both export the vendor ABI).
 * usage: t33_aeexpr_sample [reads] [sensor] [i2c addr]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>

#define MUST(x) do { int r_ = (x); if (r_) { printf("T33A FAIL %s = %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 20, i, w;
    unsigned ms = getenv("T33A_MS") ? (unsigned)atoi(getenv("T33A_MS")) : 500;
    IMPSensorInfo si;
    IMPFSChnAttr fa;
    IMPISPAEExprInfo e;
    uint32_t *u = (uint32_t *)&e;

    memset(&si, 0, sizeof(si));
    snprintf(si.name, sizeof(si.name), "%s", argc > 2 ? argv[2] : "cv2005");
    si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof(si.i2c.type), "%.19s", si.name);
    si.i2c.addr = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 0x35;
    si.rst_gpio = -1; si.pwdn_gpio = -1; si.power_gpio = -1;
    printf("T33A sizeof %u\n", (unsigned)sizeof(e));
    MUST(IMP_ISP_Open());
    MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
    MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
    MUST(IMP_System_Init());
    MUST(IMP_ISP_EnableTuning());
    memset(&fa, 0, sizeof(fa));
    fa.picWidth = 1920; fa.picHeight = 1080; fa.pixFmt = PIX_FMT_NV12;
    fa.outFrmRateNum = 25; fa.outFrmRateDen = 1; fa.nrVBs = 2; fa.type = FS_PHY_CHANNEL;
    fa.scaler.outwidth = 1920; fa.scaler.outheight = 1080;
    fa.crop.width = 1920; fa.crop.height = 1080;
    MUST(IMP_FrameSource_CreateChn(0, &fa));
    MUST(IMP_FrameSource_SetChnAttr(0, &fa));
    MUST(IMP_FrameSource_EnableChn(0));
    for (i = 0; i < n; i++) {
        int r;

        usleep(ms * 1000);
        memset(&e, 0xa5, sizeof(e));
        r = IMP_ISP_Tuning_GetAeExprInfo(IMPVI_MAIN, &e);
        printf("T33A %d rc=%d unit %u mode %u it %u again %u dgain %u ispdgain %u minit %u maxit %u maxagain %u maxdgain %u maxispdg %u\n",
               i, r, u[0], u[1], u[6], u[7], u[8], u[9], u[18], u[22], u[23], u[24], u[25]);
        if (i == 0 || i == n - 1) {
            printf("T33W %d:", i);
            for (w = 0; w < (int)(sizeof(e) / 4); w++)
                printf(" %u", u[w]);
            printf("\n");
        }
        fflush(stdout);
    }
    IMP_FrameSource_DisableChn(0);
    IMP_FrameSource_DestroyChn(0);
    IMP_ISP_DisableTuning();
    IMP_System_Exit();
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN, &si);
    IMP_ISP_Close();
    printf("T33A done\n");
    return 0;
}
