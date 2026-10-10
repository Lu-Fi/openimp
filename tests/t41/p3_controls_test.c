/* T41 P3 tuning controls: vendor control IDs, envelope and pointer
 * pass-through of the public structures (host test, needs the T41 1.2.0
 * vendor headers via T41_HEADERS). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <imp/imp_isp.h>

int OpenIMP_P1_TuningIOCtl(uint32_t command, void *argument);
int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path);
int OpenIMP_P1_GetDefaultBinPath(IMPVI_NUM num, char *path, size_t size);
int OpenIMP_P1_IspIOCtl(uint32_t command, void *argument);
int OpenIMP_P1_GetSensorName(char name[32], int32_t *cbus_type);

/* ISPDevice-fd entries: what the library asked the main ISP node for */
static struct {
    uint32_t command;
    unsigned char arg[64];
    int calls;
    int result;
    int32_t bus;
} isp;

int OpenIMP_P1_IspIOCtl(uint32_t command, void *argument)
{
    isp.command = command;
    memcpy(isp.arg, argument,
           (command == 0xc004542cU || command == 0xc004542dU) ? 40 : sizeof(isp.arg));
    isp.calls++;
    if (command == 0x8040540eU)             /* the driver returns the value */
        *(uint32_t *)((char *)argument + 56) = 0x5a;
    return isp.result;
}

int OpenIMP_P1_GetSensorName(char name[32], int32_t *cbus_type)
{
    memset(name, 0, 32);
    strcpy(name, "gc5603");
    *cbus_type = isp.bus;
    return 0;
}

static struct {
    uint32_t command;
    int32_t vinum, direction, control;
    uintptr_t payload;
    int calls;
    int result;
} last;

int OpenIMP_P1_TuningIOCtl(uint32_t command, void *argument)
{
    /* Same layout as the P3 envelope: 16 bytes on the 32-bit target. */
    const struct {
        int32_t vinum, direction, control;
        uintptr_t payload;
    } *request = argument;

    last.command = command;
    last.vinum = request->vinum;
    last.direction = request->direction;
    last.control = request->control;
    last.payload = request->payload;
    last.calls++;
    return last.result;
}

int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path)
{
    (void)num;
    (void)path;
    return 0;
}

int OpenIMP_P1_GetDefaultBinPath(IMPVI_NUM num, char *path, size_t size)
{
    /* The record lives in P1 (openimp_p1.c).  This host test only covers the
     * P3 wrapper, so the stub reports "never set", which is the state a caller
     * without a preceding IMP_ISP_SetDefaultBinPath is in. */
    (void)num;
    (void)path;
    (void)size;
    return -1;
}

#define EXPECT(call, dir, id, ptr)                                            \
    do {                                                                      \
        int before = last.calls;                                              \
        assert((call) == 0);                                                  \
        assert(last.calls == before + 1);                                     \
        assert(last.command == 0xc0105435U);                                  \
        assert(last.vinum == IMPVI_MAIN);                                     \
        assert(last.direction == (dir));                                      \
        assert(last.control == (id));                                         \
        assert(last.payload == (uintptr_t)(ptr));                             \
    } while (0)

int main(void)
{
    IMPISPAEScenceAttr scence;
    IMPISPModuleRatioAttr ratio;
    IMPISPAEExprInfo expr;
    IMPISPCCMAttr ccm;
    IMPISPGammaAttr gamma;
    IMPISPCSCAttr csc;
    IMPISPModuleCtl ctl;
    IMPISPAutoZoom zoom;
    IMPISPWdrOutputMode wdr;
    IMPISPSENSORAttr sensor;
    IMPISPAEWeightAttr weight;
    IMPISPCoefftWb coefft;
    IMPISPMaskBlockAttr mask;
    IMPISPScalerLvAttr scaler;
    IMPISPSensorFps fps;
    int before;

    /* Vendor structure sizes the open-tx-isp routes rely on. */
    assert(sizeof(IMPISPAEScenceAttr) == 52 || sizeof(void *) != 4);
    assert(sizeof(IMPISPModuleRatioAttr) == 128);
    assert(sizeof(IMPISPAEExprInfo) == 232 || sizeof(void *) != 4);

    EXPECT(IMP_ISP_Tuning_SetAeScenceAttr(IMPVI_MAIN, &scence), 0, 0x08000024, &scence);
    EXPECT(IMP_ISP_Tuning_GetAeScenceAttr(IMPVI_MAIN, &scence), 1, 0x08000024, &scence);
    EXPECT(IMP_ISP_Tuning_SetModule_Ratio(IMPVI_MAIN, &ratio), 0, 0x080000a4, &ratio);
    EXPECT(IMP_ISP_Tuning_GetModule_Ratio(IMPVI_MAIN, &ratio), 1, 0x080000a4, &ratio);
    EXPECT(IMP_ISP_Tuning_SetAeExprInfo(IMPVI_MAIN, &expr), 0, 0x08000023, &expr);
    EXPECT(IMP_ISP_Tuning_GetAeExprInfo(IMPVI_MAIN, &expr), 1, 0x08000023, &expr);
    EXPECT(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, &ccm), 0, 0x08000080, &ccm);
    EXPECT(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, &ccm), 1, 0x08000080, &ccm);
    EXPECT(IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, &gamma), 0, 0x08000025, &gamma);
    EXPECT(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, &csc), 0, 0x08000096, &csc);
    EXPECT(IMP_ISP_Tuning_SetModuleControl(IMPVI_MAIN, &ctl), 0, 0x08000072, &ctl);
    EXPECT(IMP_ISP_Tuning_SetAutoZoom(IMPVI_MAIN, &zoom), 0, 0x08000077, &zoom);
    EXPECT(IMP_ISP_Tuning_SetWdrOutputMode(IMPVI_MAIN, &wdr), 0, 0x08000054, &wdr);

    /* Controls open-tx-isp T41 routes since claude/t41-connect: pointer
     * pass-through with the stock payload sizes the driver copies. */
    assert(sizeof(IMPISPSENSORAttr) == 20);
    assert(sizeof(IMPISPAEWeightAttr) == 460);
    assert(sizeof(IMPISPCoefftWb) == 6);
    EXPECT(IMP_ISP_Tuning_GetSensorAttr(IMPVI_MAIN, &sensor), 1, 0x08000033, &sensor);
    EXPECT(IMP_ISP_Tuning_SetAeWeight(IMPVI_MAIN, &weight), 0, 0x08000021, &weight);
    EXPECT(IMP_ISP_Tuning_GetAeWeight(IMPVI_MAIN, &weight), 1, 0x08000021, &weight);
    EXPECT(IMP_ISP_Tuning_Awb_SetRgbCoefft(IMPVI_MAIN, &coefft), 0, 0x08000098, &coefft);
    EXPECT(IMP_ISP_Tuning_Awb_GetRgbCoefft(IMPVI_MAIN, &coefft), 1, 0x08000098, &coefft);
    /* The driver refuses these (MSCA, no open path): -EOPNOTSUPP. */
    EXPECT(IMP_ISP_Tuning_SetMaskBlock(IMPVI_MAIN, &mask), 0, 0x08000074, &mask);
    EXPECT(IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, &scaler), 0, 0x080000a6, &scaler);

    /* SetSensorFPS: stock s_ctrl 0x08000070 takes num<<16|den inline. */
    fps.num = 15;
    fps.den = 1;
    EXPECT(IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps), 0, 0x08000070,
           (15U << 16) | 1U);
    fps.num = 0x10000;
    before = last.calls;
    assert(IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps) == -1);
    fps.num = 25;
    fps.den = 0;
    assert(IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps) == -1);
    assert(last.calls == before);

    /* Invalid arguments never reach the driver. */
    before = last.calls;
    assert(IMP_ISP_Tuning_SetAeScenceAttr(IMPVI_MAIN, NULL) == -1);
    assert(IMP_ISP_Tuning_SetModule_Ratio(IMPVI_BUTT, &ratio) == -1);
    assert(last.calls == before);

    /* Driver errors (e.g. -EOPNOTSUPP) are returned, not hidden. */
    last.result = -1;
    assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, &ccm) == -1);
    assert(IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, &scaler) == -1);
    /* A refused rate is not cached: GetSensorFPS keeps 15/1. */
    fps.num = 10;
    fps.den = 1;
    assert(IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps) == -1);
    fps.num = fps.den = 0;
    assert(IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &fps) == -1);
    assert(fps.num == 15 && fps.den == 1);

    /* IMP_ISP_GetDefaultBinPath: no IMP_ISP_SetDefaultBinPath preceded it, so
     * the kernel owns the default and libimp cannot know it.  The wrapper must
     * fail instead of answering "success" with an empty string. */
    {
        char bin_path[64];

        memset(bin_path, 'x', sizeof(bin_path));
        assert(IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, bin_path) == -1);
        assert(bin_path[0] == '\0');
        assert(IMP_ISP_GetDefaultBinPath(IMPVI_BUTT, bin_path) == -1);
        assert(IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, NULL) == -1);
    }

    /* AF weight: tuning control 0x8000032, pointer pass-through */
    {
        IMPISPWeight af;

        last.result = 0;
        EXPECT(IMP_ISP_Tuning_SetAfWeight(IMPVI_MAIN, &af), 0, 0x8000032, &af);
        EXPECT(IMP_ISP_Tuning_GetAfWeight(IMPVI_MAIN, &af), 1, 0x8000032, &af);
    }
    /* sensor register: ISP node, 64-byte request {name, type, ..., reg at 48,
     * value at 56}; SPI sensors are refused */
    {
        IMPISPSensorRegister reg;
        uint32_t v;

        last.result = 0;
        isp.bus = 1;
        reg.addr = 0x3107;
        reg.value = 0;
        assert(IMP_ISP_GetSensorRegister(IMPVI_MAIN, &reg) == 0);
        assert(isp.command == 0x8040540eU && isp.calls == 1);
        assert(strcmp((char *)isp.arg, "gc5603") == 0);
        memcpy(&v, isp.arg + 32, 4);
        assert(v == 0);                     /* vinum */
        memcpy(&v, isp.arg + 36, 4);
        assert(v == 1);                     /* bus type */
        memcpy(&v, isp.arg + 48, 4);
        assert(v == 0x3107 && reg.value == 0x5a);
        reg.value = 0x11;
        assert(IMP_ISP_SetSensorRegister(IMPVI_MAIN, &reg) == 0);
        assert(isp.command == 0xc040540dU);
        memcpy(&v, isp.arg + 56, 4);
        assert(v == 0x11);
        isp.bus = 2;
        assert(IMP_ISP_GetSensorRegister(IMPVI_MAIN, &reg) == -1);
        assert(IMP_ISP_GetSensorRegister(IMPVI_BUTT, &reg) == -1);
        assert(isp.calls == 2);
    }
    /* frame drop: three channels, lsize 0..31 */
    {
        IMPISPFrameDropAttr fd, back;

        memset(&fd, 0, sizeof(fd));
        fd.fdrop[1].enable = IMPISP_TUNING_OPS_MODE_ENABLE;
        fd.fdrop[1].lsize = 3;
        fd.fdrop[1].fmark = 0x5;
        isp.calls = 0;
        assert(IMP_ISP_SetFrameDrop(IMPVI_MAIN, &fd) == 0);
        assert(isp.command == 0xc004542cU && isp.calls == 1);
        {   /* 40-byte request: channel word, then entry 1 at 4 + 12 */
            uint32_t w[10];
            memcpy(w, isp.arg, 40);
            assert(w[0] == 0 && w[4] == 1 && w[5] == 3 && w[6] == 0x5);
        }
        fd.fdrop[2].lsize = 32;
        assert(IMP_ISP_SetFrameDrop(IMPVI_MAIN, &fd) == -1);
        assert(isp.calls == 1);
        assert(IMP_ISP_GetFrameDrop(IMPVI_MAIN, &back) == 0);
        assert(isp.command == 0xc004542dU);
        isp.result = -1;
        assert(IMP_ISP_GetFrameDrop(IMPVI_MAIN, &back) == -1);
        isp.result = 0;
    }
    /* WDR switch: ioctl 0x80045413 / 0x80045414, _GET answers from memory */
    {
        IMPISPTuningOpsMode m = IMPISP_TUNING_OPS_MODE_ENABLE, g = IMPISP_TUNING_OPS_MODE_DISABLE;

        assert(IMP_ISP_WDR_ENABLE_GET(IMPVI_MAIN, &g) == 0 && g == IMPISP_TUNING_OPS_MODE_DISABLE);
        assert(IMP_ISP_WDR_ENABLE(IMPVI_MAIN, &m) == 0);
        assert(isp.command == 0x80045413U);
        assert(IMP_ISP_WDR_ENABLE_GET(IMPVI_MAIN, &g) == 0 && g == IMPISP_TUNING_OPS_MODE_ENABLE);
        m = IMPISP_TUNING_OPS_MODE_DISABLE;
        assert(IMP_ISP_WDR_ENABLE(IMPVI_MAIN, &m) == 0);
        assert(isp.command == 0x80045414U);
        isp.result = -1;
        m = IMPISP_TUNING_OPS_MODE_ENABLE;
        assert(IMP_ISP_WDR_ENABLE(IMPVI_MAIN, &m) == -1);
        assert(IMP_ISP_WDR_ENABLE_GET(IMPVI_MAIN, &g) == 0 && g == IMPISP_TUNING_OPS_MODE_DISABLE);
        isp.result = 0;
    }
    puts("t41 p3 controls tests passed");
    return 0;
}
