/* T41 P3 tuning controls: vendor control IDs, envelope and pointer
 * pass-through of the public structures (host test, needs the T41 1.2.6 or 1.2.0-zh
 * vendor headers via T41_HEADERS). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <imp/imp_isp.h>

int OpenIMP_P1_TuningIOCtl(uint32_t command, void *argument);
int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path);
int OpenIMP_P1_TuningReady(void);
int OpenIMP_P1_SensorRegister(int32_t num, uint32_t *reg, int set);

/* 0: no ISP device, 1: tuning off, 2: tuning on */
static int fake_ready = 2;
/* what the stock driver would have in its 40-byte CCM block / 92-byte CSC
 * block when the ioctl is a get */
static uint32_t fake_block[23];
static size_t fake_block_bytes;
static uint32_t seen_block[23];

static struct {
    int calls, set;
    int32_t num;
    uint32_t addr, value;
    int result;
} sreg;

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
    if (request->payload && fake_block_bytes) {
        memcpy(seen_block, (void *)request->payload, fake_block_bytes);
        if (request->direction == 1 && last.result == 0)
            memcpy((void *)request->payload, fake_block, fake_block_bytes);
    }
    return last.result;
}

int OpenIMP_P1_TuningReady(void)
{
    return fake_ready;
}

int OpenIMP_P1_SensorRegister(int32_t num, uint32_t *reg, int set)
{
    sreg.calls++;
    sreg.set = set;
    sreg.num = num;
    if (!reg)
        return -4087;
    sreg.addr = reg[0];
    sreg.value = reg[1];
    if (!set && !sreg.result)
        reg[1] = 0x5a;
    return sreg.result;
}

int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path)
{
    (void)num;
    (void)path;
    return 0;
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



/* Vendor T41 libimp 1.2.6: error ladder, CCM/CSC conversions, gamma
 * validation, sensor register request. */
static void vendor_exact(IMPISPCCMAttr *ccm, IMPISPGammaAttr *gamma,
                         IMPISPCSCAttr *csc, IMPISPAutoZoom *zoom,
                         IMPISPWdrOutputMode *wdr)
{
    IMPISPSensorRegister reg;
    int before, i;

    assert(sizeof(IMPISPCCMAttr) == 44 && sizeof(IMPISPGammaAttr) == 264);
    assert(sizeof(IMPISPCSCAttr) == 48 && sizeof(IMPISPAutoZoom) == 60);
    assert(sizeof(IMPISPSensorRegister) == 8);

    /* ladder: no device, NULL, vinum, tuning off */
    fake_ready = 0;
    before = last.calls;
    assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, ccm) == -4088);
    assert(IMP_ISP_Tuning_GetGammaAttr(IMPVI_MAIN, gamma) == -4088);
    assert(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, csc) == -4088);
    fake_ready = 1;
    assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, ccm) == -4091);
    assert(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, ccm) == -4091);
    assert(IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, gamma) == -4091);
    assert(IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_MAIN, csc) == -4091);
    assert(IMP_ISP_Tuning_SetAutoZoom(IMPVI_MAIN, zoom) == -4091);
    assert(IMP_ISP_Tuning_GetWdrOutputMode(IMPVI_MAIN, wdr) == -4091);
    fake_ready = 2;
    assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, NULL) == -4087);
    assert(IMP_ISP_Tuning_GetAutoZoom(IMPVI_MAIN, NULL) == -4087);
    assert(IMP_ISP_Tuning_SetWdrOutputMode(IMPVI_MAIN, NULL) == -4087);
    assert(IMP_ISP_Tuning_SetCCMAttr((IMPVI_NUM)2, ccm) == -4084);
    assert(IMP_ISP_Tuning_GetGammaAttr((IMPVI_NUM)-1, gamma) == -4084);
    assert(last.calls == before);

    /* the stock library sets a CSC without looking at the tuning state:
     * the ioctl decides */
    fake_ready = 1;
    memset(csc, 0, sizeof(*csc));
    csc->ColorGamut = IMP_ISP_CG_BT709_FULL;
    fake_block_bytes = 92;
    assert(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, csc) == 0);
    assert(last.calls == before + 1 && seen_block[0] == 2);
    fake_ready = 2;

    /* gamma: curve type >= 5 is refused before the ioctl */
    before = last.calls;
    gamma->Curve_type = IMP_ISP_GAMMA_CURVE_BUTT;
    assert(IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, gamma) == -4092);
    assert(last.calls == before);
    gamma->Curve_type = IMP_ISP_GAMMA_CURVE_USER;
    EXPECT(IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, gamma), 0, 0x08000025, gamma);
    EXPECT(IMP_ISP_Tuning_GetGammaAttr(IMPVI_MAIN, gamma), 1, 0x08000025, gamma);
    EXPECT(IMP_ISP_Tuning_SetAutoZoom(IMPVI_MAIN, zoom), 0, 0x08000077, zoom);
    EXPECT(IMP_ISP_Tuning_GetAutoZoom(IMPVI_MAIN, zoom), 1, 0x08000077, zoom);
    EXPECT(IMP_ISP_Tuning_SetWdrOutputMode(IMPVI_MAIN, wdr), 0, 0x08000054, wdr);
    EXPECT(IMP_ISP_Tuning_GetWdrOutputMode(IMPVI_MAIN, wdr), 1, 0x08000054, wdr);
    /* ioctl failure: the vendor code, not -1 */
    last.result = -1;
    assert(IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, gamma) == -4090);
    assert(IMP_ISP_Tuning_GetWdrOutputMode(IMPVI_MAIN, wdr) == -4090);
    last.result = 0;

    /* CCM: 13-bit words, negative values as 13-bit two's complement with
     * bit 13 set; byte 0/1 the enables; the caller's matrix is untouched */
    fake_block_bytes = 40;
    ccm->ManualEn = IMPISP_TUNING_OPS_MODE_ENABLE;
    ccm->SatEn = IMPISP_TUNING_OPS_MODE_DISABLE;
    {
        static const float m[9] = { 1.0f, -0.5f, 0.0f, 0.25f, 1.5f, -1.0f,
                                    -0.000005f, 7.999f, -3.0f };
        static const uint32_t want[9] = {
            1024, 0x2000 | ((0x2000 - 512) & 0x1fff), 0, 256, 1536,
            0x2000 | ((0x2000 - 1024) & 0x1fff), 0, 8190,
            0x2000 | ((0x2000 - 3072) & 0x1fff) };
        float copy[9];

        memcpy(ccm->ColorMatrix, m, sizeof(m));
        memcpy(copy, m, sizeof(m));
        before = last.calls;
        assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, ccm) == 0);
        assert(last.calls == before + 1 && last.command == 0xc0105435U);
        assert(last.direction == 0 && last.control == 0x08000080);
        assert(last.payload != (uintptr_t)ccm);
        assert(((uint8_t *)seen_block)[0] == 1 && ((uint8_t *)seen_block)[1] == 0);
        assert(((uint8_t *)seen_block)[2] == 0 && ((uint8_t *)seen_block)[3] == 0);
        for (i = 0; i < 9; i++)
            assert(seen_block[1 + i] == want[i]);
        assert(!memcmp(ccm->ColorMatrix, copy, sizeof(copy)));
        /* the kernel hands the same block back: matrix round trip */
        memcpy(fake_block, seen_block, 40);
        memset(ccm->ColorMatrix, 0, sizeof(ccm->ColorMatrix));
        ccm->ManualEn = ccm->SatEn = 7;
        assert(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, ccm) == 0);
        assert(ccm->ManualEn == 1 && ccm->SatEn == 0);
        assert(ccm->ColorMatrix[0] == 1.0f && ccm->ColorMatrix[1] == -0.5f);
        assert(ccm->ColorMatrix[2] == 0.0f && ccm->ColorMatrix[3] == 0.25f);
        assert(ccm->ColorMatrix[5] == -1.0f && ccm->ColorMatrix[8] == -3.0f);
        assert(ccm->ColorMatrix[7] == 8190 * 0.0009765625f);
        /* a stock block that is only zeros (nothing set yet) */
        memset(fake_block, 0, sizeof(fake_block));
        assert(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, ccm) == 0);
        assert(ccm->ManualEn == 0 && ccm->ColorMatrix[4] == 0.0f);
        /* NaN / huge saturate like trunc.w.s (2^31-1 & 0x1fff) */
        ccm->ColorMatrix[0] = 1e30f;
        ccm->ColorMatrix[1] = -1e30f;
        assert(IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, ccm) == 0);
        assert(seen_block[1] == 0x1fff);
        assert(seen_block[2] == (0x2000 | ((0U - 0x7fffffffU) & 0x1fff)));
    }

    /* CSC: only the user gamut carries a matrix: Q10, +0.5, 10 bits */
    fake_block_bytes = 92;
    memset(csc, 0, sizeof(*csc));
    csc->ColorGamut = IMP_ISP_CG_USER;
    {
        static const float c[9] = { 0.299f, 0.587f, 0.114f, -0.169f, -0.331f,
                                    0.5f, 0.5f, -0.419f, -0.081f };
        for (i = 0; i < 9; i++)
            csc->Matrix.CscCoef[i] = c[i];
    }
    csc->Matrix.CscOffset[0] = 128;
    csc->Matrix.CscOffset[1] = 16;
    csc->Matrix.CscClip[0] = 235;
    csc->Matrix.CscClip[1] = 16;
    csc->Matrix.CscClip[2] = 240;
    csc->Matrix.CscClip[3] = 17;
    assert(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, csc) == 0);
    assert(seen_block[0] == 4);
    assert(seen_block[1] == 306 && seen_block[2] == 601 && seen_block[3] == 117);
    assert(seen_block[4] == (((int)(-0.169f * 1024.0f + 0.5)) & 0x3ff));
    assert(seen_block[6] == 512 && seen_block[7] == 512);
    assert(((uint8_t *)seen_block)[40] == 128 && ((uint8_t *)seen_block)[41] == 16);
    assert(((uint8_t *)seen_block)[42] == 235 && ((uint8_t *)seen_block)[45] == 17);
    assert(((uint8_t *)seen_block)[46] == 0 && seen_block[12] == 0);
    /* a preset sends the mode only */
    csc->ColorGamut = IMP_ISP_CG_BT601_LIMITED;
    assert(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, csc) == 0);
    assert(seen_block[0] == 1 && seen_block[1] == 0 && seen_block[5] == 0);
    csc->ColorGamut = IMP_ISP_CG_BUTT;
    before = last.calls;
    assert(IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_MAIN, csc) == -4092);
    assert(last.calls == before);
    /* get: mode 4 converts back, rows two/three negate their first two */
    memset(fake_block, 0, sizeof(fake_block));
    fake_block[0] = 4;
    for (i = 1; i <= 9; i++)
        fake_block[i] = (uint32_t)i * 100;
    ((uint8_t *)fake_block)[40] = 128;
    ((uint8_t *)fake_block)[42] = 235;
    memset(csc, 0x77, sizeof(*csc));
    assert(IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_MAIN, csc) == 0);
    assert(csc->ColorGamut == IMP_ISP_CG_USER);
    assert(csc->Matrix.CscCoef[0] == 100 * 0.0009765625f);
    assert(csc->Matrix.CscCoef[2] == 300 * 0.0009765625f);
    assert(csc->Matrix.CscCoef[3] == -(400 * 0.0009765625f));
    assert(csc->Matrix.CscCoef[4] == -(500 * 0.0009765625f));
    assert(csc->Matrix.CscCoef[5] == 600 * 0.0009765625f);
    assert(csc->Matrix.CscCoef[7] == -(800 * 0.0009765625f));
    assert(csc->Matrix.CscCoef[8] == -(900 * 0.0009765625f));
    assert(csc->Matrix.CscOffset[0] == 128 && csc->Matrix.CscClip[0] == 235);
    /* any other mode zeroes the matrix */
    fake_block[0] = 2;
    assert(IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_MAIN, csc) == 0);
    assert(csc->ColorGamut == IMP_ISP_CG_BT709_FULL);
    assert(csc->Matrix.CscCoef[3] == 0 && csc->Matrix.CscClip[3] == 0);
    fake_block_bytes = 0;

    /* sensor register: forwarded with the vendor ladder done by P1 */
    reg.addr = 0x3103;
    reg.value = 0x1234;
    assert(IMP_ISP_SetSensorRegister(IMPVI_MAIN, &reg) == 0);
    assert(sreg.set == 1 && sreg.num == 0 && sreg.addr == 0x3103 &&
           sreg.value == 0x1234);
    assert(IMP_ISP_GetSensorRegister(IMPVI_MAIN, &reg) == 0);
    assert(sreg.set == 0 && reg.addr == 0x3103 && reg.value == 0x5a);
    assert(IMP_ISP_GetSensorRegister(IMPVI_MAIN, NULL) == -4087);
    sreg.result = -4095;
    reg.value = 1;
    assert(IMP_ISP_GetSensorRegister(IMPVI_MAIN, &reg) == -4095 && reg.value == 1);
    sreg.result = 0;
}

/* Guard test: the getters write exactly sizeof(struct) bytes (the stock
 * library builds the 40/92-byte kernel blocks locally and converts into
 * the caller's 44/48-byte structures). */
struct guarded {
    unsigned char before[64];
    union {
        IMPISPCCMAttr ccm;
        IMPISPCSCAttr csc;
        IMPISPGammaAttr gamma;
        IMPISPAutoZoom zoom;
    } u;
    unsigned char after[64];
};

static void guard_fill(struct guarded *g)
{
    memset(g->before, 0xa5, sizeof(g->before));
    memset(&g->u, 0x3c, sizeof(g->u));
    memset(g->after, 0xa5, sizeof(g->after));
}

static void guard_check(const struct guarded *g, size_t used)
{
    size_t i;

    for (i = 0; i < sizeof(g->before); i++)
        assert(g->before[i] == 0xa5);
    for (i = 0; i < sizeof(g->after); i++)
        assert(g->after[i] == 0xa5);
    /* bytes behind the structure inside the union stay untouched */
    for (i = used; i < sizeof(g->u); i++)
        assert(((const unsigned char *)&g->u)[i] == 0x3c);
}

static void guards(void)
{
    static struct guarded g;

    fake_ready = 2;
    last.result = 0;
    memset(fake_block, 0, sizeof(fake_block));
    fake_block_bytes = 40;
    guard_fill(&g);
    assert(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, &g.u.ccm) == 0);
    guard_check(&g, sizeof(IMPISPCCMAttr));
    fake_block_bytes = 92;
    fake_block[0] = 4;
    guard_fill(&g);
    assert(IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_MAIN, &g.u.csc) == 0);
    guard_check(&g, sizeof(IMPISPCSCAttr));
    fake_block[0] = 1;
    guard_fill(&g);
    assert(IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_MAIN, &g.u.csc) == 0);
    guard_check(&g, sizeof(IMPISPCSCAttr));
    /* a failed get leaves the structure alone */
    fake_block_bytes = 40;
    guard_fill(&g);
    last.result = -1;
    assert(IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, &g.u.ccm) == -4090);
    guard_check(&g, 0);
    last.result = 0;
    fake_block_bytes = 0;
}

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
    EXPECT(IMP_ISP_Tuning_SetModuleControl(IMPVI_MAIN, &ctl), 0, 0x08000072, &ctl);

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
    assert(IMP_ISP_Tuning_SetModuleControl(IMPVI_MAIN, &ctl) == -1);
    assert(IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, &scaler) == -1);
    /* A refused rate is not cached: GetSensorFPS keeps 15/1. */
    fps.num = 10;
    fps.den = 1;
    assert(IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &fps) == -1);
    fps.num = fps.den = 0;
    assert(IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &fps) == -1);
    assert(fps.num == 15 && fps.den == 1);
    last.result = 0;
    vendor_exact(&ccm, &gamma, &csc, &zoom, &wdr);
    guards();
    puts("t41 p3 controls tests passed");
    return 0;
}
