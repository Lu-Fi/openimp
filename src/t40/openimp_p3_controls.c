/* P3 T40 control plane: ISP tuning and direct system-register access. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <imp/imp_isp.h>

#if defined(PLATFORM_T41)
#define TISP_VIDIOC_DEFAULT_TUNING 0xc0105435U
#else
#define TISP_VIDIOC_DEFAULT_TUNING 0xc0105436U
#endif

/*
 * T40's OEM userspace ABI predates the newer 0x56xx tuning-node interface.
 * The stock driver owns all sensor-specific exposure and gain translation.
 */
#define TISP_CID_AE_EXPR_INFO 0x08000023
#define TISP_CID_AE_WEIGHT 0x08000021
#define TISP_CID_AE_STATISTICS 0x08000022
#define TISP_CID_AWB_ATTR 0x08000010
#define TISP_CID_AWB_STATISTICS 0x08000011
#define TISP_CID_AWB_WEIGHT 0x08000012
#define TISP_CID_AWB_GLOBAL_STATISTICS 0x08000013
#define TISP_CID_SENSOR_FPS 0x08000070
#define TISP_CID_RUNNING_MODE 0x08000071
#define TISP_CID_HVFLIP 0x08000073
#define TISP_CID_BCSH_HUE 0x08000081
#define TISP_CID_BRIGHTNESS 0x08000092
#define TISP_CID_SHARPNESS 0x08000093
#define TISP_CID_SATURATION 0x08000094
#define TISP_CID_CONTRAST 0x08000095
#if defined(PLATFORM_T41)
#define TISP_CID_MASK_BLOCK 0x08000074
#define TISP_CID_SCALER_LV 0x080000a6
#define TISP_CID_AWB_RGB_COEFFT 0x08000098
#define TISP_CID_ANTIFLICKER 0x08000026
#endif

typedef struct {
    int32_t vinum;
    int32_t direction;
    int32_t control;
    uintptr_t payload;
} P3TuningRequest;

extern int OpenIMP_P1_TuningIOCtl(uint32_t command, void *argument);
extern int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path);
extern int OpenIMP_P1_GetDefaultBinPath(IMPVI_NUM num, char *path, size_t size);

void OpenIMP_P3_FrameStats(uint32_t luma, uint32_t u_mean, uint32_t v_mean)
{
    /*
     * The stock ISP firmware owns AE/AWB statistics and policy.  Keep these
     * encoder-side samples diagnostic-only; they must never program a sensor.
     */
    (void)luma;
    (void)u_mean;
    (void)v_mean;
}

static int p3_tuning_pointer(IMPVI_NUM num, int32_t direction,
                             int32_t control, void *payload)
{
    P3TuningRequest request;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT || !payload)
        return -1;
    request.vinum = num;
    request.direction = direction;
    request.control = control;
    request.payload = (uintptr_t)payload;
    return OpenIMP_P1_TuningIOCtl(TISP_VIDIOC_DEFAULT_TUNING, &request);
}

static int p3_tuning_scalar(IMPVI_NUM num, int32_t direction,
                            int32_t control, int32_t *value)
{
    P3TuningRequest request;
    int result;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT || !value)
        return -1;
    request.vinum = num;
    request.direction = direction;
    request.control = control;
    request.payload = direction ? 0U : (uintptr_t)(uint32_t)*value;
    result = OpenIMP_P1_TuningIOCtl(TISP_VIDIOC_DEFAULT_TUNING, &request);
    if (result == 0 && direction)
        *value = (int32_t)request.payload;
    return result;
}

#if defined(PLATFORM_T41)
/* Vendor T41 1.2.0: the pointer tuning ioctl, get, control 0x8000033,
 * filling {hts, vts, fps, width, height}. */
int32_t IMP_ISP_Tuning_GetSensorAttr(IMPVI_NUM num, IMPISPSENSORAttr *attr)
{
    return p3_tuning_pointer(num, 1, 0x8000033, attr);
}
#endif

int32_t IMP_ISP_Tuning_GetAeExprInfo(IMPVI_NUM num,
                                     IMPISPAEExprInfo *exprinfo)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AE_EXPR_INFO, exprinfo);
}

int32_t IMP_ISP_Tuning_SetAeExprInfo(IMPVI_NUM num,
                                     IMPISPAEExprInfo *exprinfo)
{
    return p3_tuning_pointer(num, 0, TISP_CID_AE_EXPR_INFO, exprinfo);
}

int32_t IMP_ISP_Tuning_SetAeWeight(IMPVI_NUM num,
                                    IMPISPAEWeightAttr *ae_weight)
{
    return p3_tuning_pointer(num, 0, TISP_CID_AE_WEIGHT, ae_weight);
}

int32_t IMP_ISP_Tuning_GetAeWeight(IMPVI_NUM num,
                                    IMPISPAEWeightAttr *ae_weight)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AE_WEIGHT, ae_weight);
}

int32_t IMP_ISP_Tuning_GetAeStatistics(IMPVI_NUM num,
                                        IMPISPAEStatisInfo *ae_statis)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AE_STATISTICS, ae_statis);
}

int32_t IMP_ISP_Tuning_SetAwbAttr(IMPVI_NUM num, IMPISPWBAttr *attr)
{
    return p3_tuning_pointer(num, 0, TISP_CID_AWB_ATTR, attr);
}

int32_t IMP_ISP_Tuning_GetAwbAttr(IMPVI_NUM num, IMPISPWBAttr *attr)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AWB_ATTR, attr);
}

int32_t IMP_ISP_Tuning_GetAwbStatistics(IMPVI_NUM num,
                                         IMPISPAWBStatisInfo *awb_statis)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AWB_STATISTICS, awb_statis);
}

int32_t IMP_ISP_Tuning_SetAwbWeight(IMPVI_NUM num, IMPISPWeight *awb_weight)
{
    return p3_tuning_pointer(num, 0, TISP_CID_AWB_WEIGHT, awb_weight);
}

int32_t IMP_ISP_Tuning_GetAwbWeight(IMPVI_NUM num, IMPISPWeight *awb_weight)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AWB_WEIGHT, awb_weight);
}

int32_t IMP_ISP_Tuning_GetAwbGlobalStatistics(
    IMPVI_NUM num, IMPISPAWBGlobalStatisInfo *awb_statis)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AWB_GLOBAL_STATISTICS,
                             awb_statis);
}

static struct {
    unsigned char brightness;
    unsigned char contrast;
    unsigned char saturation;
    unsigned char sharpness;
    unsigned char hue;
    uint32_t fps_num;
    uint32_t fps_den;
#if defined(PLATFORM_T41)
    IMPISPHVFLIPAttr flip;
#else
    IMPISPHVFLIP flip;
#endif
    IMPISPRunningMode running_mode;
    IMPISPAntiflickerAttr antiflicker;
    char bin_path[128];
} p3_controls = {
    .brightness = 128,
    .contrast = 128,
    .saturation = 128,
    .sharpness = 128,
    .hue = 128,
    .fps_num = 30,
    .fps_den = 1,
};

static int p3_tuning_set(int32_t id, int32_t value)
{
    return p3_tuning_scalar(IMPVI_MAIN, 0, id, &value);
}

static int p3_tuning_get(int32_t id, int32_t *value)
{
    return p3_tuning_scalar(IMPVI_MAIN, 1, id, value);
}

#define P3_BCSH_ACCESSORS(Name, field, id)                                    \
    int32_t IMP_ISP_Tuning_Set##Name(IMPVI_NUM num, unsigned char *value)     \
    {                                                                         \
        int result;                                                           \
        if (num != IMPVI_MAIN || !value)                                     \
            return -1;                                                        \
        result = p3_tuning_pointer(num, 0, (id), value);                     \
        if (result == 0)                                                      \
            p3_controls.field = *value;                                       \
        return result;                                                        \
    }                                                                         \
    int32_t IMP_ISP_Tuning_Get##Name(IMPVI_NUM num, unsigned char *value)     \
    {                                                                         \
        int result;                                                           \
        if (num != IMPVI_MAIN || !value)                                     \
            return -1;                                                        \
        result = p3_tuning_pointer(num, 1, (id), value);                     \
        if (result == 0)                                                      \
            p3_controls.field = *value;                                       \
        *value = p3_controls.field;                                           \
        return result;                                                        \
    }

P3_BCSH_ACCESSORS(Brightness, brightness, TISP_CID_BRIGHTNESS)
P3_BCSH_ACCESSORS(Contrast, contrast, TISP_CID_CONTRAST)
P3_BCSH_ACCESSORS(Saturation, saturation, TISP_CID_SATURATION)
P3_BCSH_ACCESSORS(Sharpness, sharpness, TISP_CID_SHARPNESS)

int32_t IMP_ISP_Tuning_SetBcshHue(IMPVI_NUM num, unsigned char *value)
{
    int result;

    if (num != IMPVI_MAIN || !value)
        return -1;
    result = p3_tuning_pointer(num, 0, TISP_CID_BCSH_HUE, value);
    if (result == 0)
        p3_controls.hue = *value;
    return result;
}

int32_t IMP_ISP_Tuning_GetBcshHue(IMPVI_NUM num, unsigned char *value)
{
    int result;

    if (num != IMPVI_MAIN || !value)
        return -1;
    result = p3_tuning_pointer(num, 1, TISP_CID_BCSH_HUE, value);
    if (result == 0)
        p3_controls.hue = *value;
    *value = p3_controls.hue;
    return result;
}

#if defined(PLATFORM_T41)
int32_t IMP_ISP_Tuning_SetSensorFPS(IMPVI_NUM num, IMPISPSensorFps *fps)
#else
int32_t IMP_ISP_Tuning_SetSensorFPS(IMPVI_NUM num, uint32_t *numerator,
                                    uint32_t *denominator)
#endif
{
    int result;
#if defined(PLATFORM_T41)
    uint32_t *numerator = fps ? &fps->num : NULL;
    uint32_t *denominator = fps ? &fps->den : NULL;
#endif

    if (num != IMPVI_MAIN || !numerator || !denominator || !*denominator ||
        *numerator > 0xffffU || *denominator > 0xffffU)
        return -1;
    result = p3_tuning_set(TISP_CID_SENSOR_FPS,
                           (int32_t)((*numerator << 16) | *denominator));
    if (result == 0) {
        p3_controls.fps_num = *numerator;
        p3_controls.fps_den = *denominator;
    }
    return result;
}

#if defined(PLATFORM_T41)
int32_t IMP_ISP_Tuning_GetSensorFPS(IMPVI_NUM num, IMPISPSensorFps *fps)
#else
int32_t IMP_ISP_Tuning_GetSensorFPS(IMPVI_NUM num, uint32_t *numerator,
                                    uint32_t *denominator)
#endif
{
    int32_t value = 0;
    int result;
#if defined(PLATFORM_T41)
    uint32_t *numerator = fps ? &fps->num : NULL;
    uint32_t *denominator = fps ? &fps->den : NULL;
#endif

    if (num != IMPVI_MAIN || !numerator || !denominator)
        return -1;
    result = p3_tuning_get(TISP_CID_SENSOR_FPS, &value);
    if (result == 0) {
        p3_controls.fps_num = ((uint32_t)value >> 16) & 0xffffU;
        p3_controls.fps_den = (uint32_t)value & 0xffffU;
    }
    *numerator = p3_controls.fps_num;
    *denominator = p3_controls.fps_den;
    return result;
}

#if defined(PLATFORM_T41)
int32_t IMP_ISP_Tuning_SetHVFLIP(IMPVI_NUM num, IMPISPHVFLIPAttr *flip)
#else
int32_t IMP_ISP_Tuning_SetHVFLIP(IMPVI_NUM num, IMPISPHVFLIP *flip)
#endif
{
    int result;

    if (num != IMPVI_MAIN || !flip)
        return -1;
    result = p3_tuning_pointer(num, 0, TISP_CID_HVFLIP, flip);
    if (result == 0)
        p3_controls.flip = *flip;
    return result;
}

#if defined(PLATFORM_T41)
int32_t IMP_ISP_Tuning_GetHVFLIP(IMPVI_NUM num, IMPISPHVFLIPAttr *flip)
#else
int32_t IMP_ISP_Tuning_GetHVFlip(IMPVI_NUM num, IMPISPHVFLIP *flip)
#endif
{
    int result;

    if (num != IMPVI_MAIN || !flip)
        return -1;
    result = p3_tuning_pointer(num, 1, TISP_CID_HVFLIP, flip);
    if (result == 0)
        p3_controls.flip = *flip;
    *flip = p3_controls.flip;
    return result;
}

#if defined(PLATFORM_T41)
int32_t IMP_ISP_Tuning_SetMaskBlock(IMPVI_NUM num,
                                    IMPISPMaskBlockAttr *mask)
{
    return p3_tuning_pointer(num, 0, TISP_CID_MASK_BLOCK, mask);
}

int32_t IMP_ISP_Tuning_SetScalerLv(IMPVI_NUM num, IMPISPScalerLvAttr *attr)
{
    return p3_tuning_pointer(num, 0, TISP_CID_SCALER_LV, attr);
}

int IMP_ISP_Tuning_Awb_SetRgbCoefft(IMPVI_NUM num, IMPISPCoefftWb *attr)
{
    return p3_tuning_pointer(num, 0, TISP_CID_AWB_RGB_COEFFT, attr);
}

int IMP_ISP_Tuning_Awb_GetRgbCoefft(IMPVI_NUM num, IMPISPCoefftWb *attr)
{
    return p3_tuning_pointer(num, 1, TISP_CID_AWB_RGB_COEFFT, attr);
}

/*
 * Vendor T41 libimp (1.1.0/1.2.5 control numbering, the one the driver and
 * the controls above use): each of these is a plain pointer pass-through of
 * the public structure on the tuning ioctl.  open-tx-isp implements
 *   AeScenceAttr  - AeTargetComp (0..255, 128 neutral) on the open AE,
 *   AeExprInfo    - AeMaxIntegrationTime / AeMaxAGain caps (SET),
 *   Module_Ratio  - SINTER (2D NR) and TEMPER (3D NR) strength;
 * and answers -EOPNOTSUPP for HLC/BLC, manual exposure, DRC/DPC/defog
 * ratios, CCM, gamma, CSC, module bypass, auto zoom and WDR output mode,
 * so callers see the failure instead of an acknowledged no-op.
 */
#define TISP_CID_AE_SCENCE 0x08000024
#define TISP_CID_GAMMA 0x08000025
#define TISP_CID_WDR_OUTPUT_MODE 0x08000054
#define TISP_CID_MODULE_CONTROL 0x08000072
#define TISP_CID_AUTOZOOM 0x08000077
#define TISP_CID_CCM 0x08000080
#define TISP_CID_CSC 0x08000096
#define TISP_CID_MODULE_RATIO 0x080000a4

#define P3_T41_POINTER_PAIR(Name, Type, id)                                   \
    int32_t IMP_ISP_Tuning_Set##Name(IMPVI_NUM num, Type *attr)               \
    {                                                                         \
        return p3_tuning_pointer(num, 0, (id), attr);                         \
    }                                                                         \
    int32_t IMP_ISP_Tuning_Get##Name(IMPVI_NUM num, Type *attr)               \
    {                                                                         \
        return p3_tuning_pointer(num, 1, (id), attr);                         \
    }

P3_T41_POINTER_PAIR(AeScenceAttr, IMPISPAEScenceAttr, TISP_CID_AE_SCENCE)
P3_T41_POINTER_PAIR(Module_Ratio, IMPISPModuleRatioAttr, TISP_CID_MODULE_RATIO)
P3_T41_POINTER_PAIR(GammaAttr, IMPISPGammaAttr, TISP_CID_GAMMA)
P3_T41_POINTER_PAIR(ModuleControl, IMPISPModuleCtl, TISP_CID_MODULE_CONTROL)
P3_T41_POINTER_PAIR(AutoZoom, IMPISPAutoZoom, TISP_CID_AUTOZOOM)
P3_T41_POINTER_PAIR(WdrOutputMode, IMPISPWdrOutputMode, TISP_CID_WDR_OUTPUT_MODE)

/*
 * CCM and CSC do not travel as the public structures: the kernel (stock
 * tisp_s_ccm_attr / tisp_csc_api_set) takes
 *   CCM, 40 bytes: u8 ManualEn, u8 SatEn, 2 pad, 9 x s32 Q16 (65536 = 1.0)
 *   CSC, 92 bytes: s32 version (0..3 = ColorGamut, 6 = user), 9 x s32 RGB->YUV
 *     Q16, bytes {Y offset, UV offset, Y min, Y max}, bytes {UV min, UV max,
 *     0, 0}, 9 x s32 YUV->RGB Q16 (the inverse), then the two byte words again.
 */
static int32_t p3_q16(float value)
{
    float scaled = value * 65536.0f;

    if (scaled >= 2147483520.0f)
        return 0x7fffff00;
    if (scaled <= -2147483520.0f)
        return -0x7fffff00;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void p3_put32(unsigned char *p, int32_t v)
{
    uint32_t u = (uint32_t)v;

    p[0] = (unsigned char)u;
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)(u >> 16);
    p[3] = (unsigned char)(u >> 24);
}

static int32_t p3_get32(const unsigned char *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

void OpenIMP_P3_CCMToWire(const IMPISPCCMAttr *attr, unsigned char wire[40])
{
    unsigned int i;

    memset(wire, 0, 40);
    wire[0] = (unsigned char)(attr->ManualEn ? 1 : 0);
    wire[1] = (unsigned char)(attr->SatEn ? 1 : 0);
    for (i = 0; i < 9; i++)
        p3_put32(wire + 4 + i * 4, p3_q16(attr->ColorMatrix[i]));
}

void OpenIMP_P3_CCMFromWire(const unsigned char wire[40], IMPISPCCMAttr *attr)
{
    unsigned int i;

    attr->ManualEn = wire[0] ? 1 : 0;
    attr->SatEn = wire[1] ? 1 : 0;
    for (i = 0; i < 9; i++)
        attr->ColorMatrix[i] = (float)p3_get32(wire + 4 + i * 4) / 65536.0f;
}

/* Inverse of a 3x3 matrix; 0 when singular. */
static int p3_invert3(const double m[9], double inv[9])
{
    double c0 = m[4] * m[8] - m[5] * m[7];
    double c1 = m[5] * m[6] - m[3] * m[8];
    double c2 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c0 + m[1] * c1 + m[2] * c2;

    if (det > -1e-9 && det < 1e-9)
        return 0;
    inv[0] = c0 / det;
    inv[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    inv[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    inv[3] = c1 / det;
    inv[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    inv[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    inv[6] = c2 / det;
    inv[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    inv[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return 1;
}

/* 0, or -1 when the request cannot be expressed (bad gamut, singular user
 * matrix). */
int OpenIMP_P3_CSCToWire(const IMPISPCSCAttr *attr, unsigned char wire[92])
{
    double fwd[9], inv[9];
    unsigned int i;

    memset(wire, 0, 92);
    if ((unsigned int)attr->ColorGamut > 4U)
        return -1;
    if (attr->ColorGamut != 4) {
        p3_put32(wire, (int32_t)attr->ColorGamut);
        return 0;
    }
    for (i = 0; i < 9; i++) {
        float f = attr->Matrix.CscCoef[i];

        if (!(f > -4.0f && f < 4.0f))
            return -1;
        fwd[i] = (double)p3_q16(f) / 65536.0;
    }
    if (!p3_invert3(fwd, inv))
        return -1;
    p3_put32(wire, 6);
    for (i = 0; i < 9; i++) {
        double r = inv[i];

        if (!(r > -4.0 && r < 4.0))
            return -1;
        p3_put32(wire + 4 + i * 4, p3_q16(attr->Matrix.CscCoef[i]));
        p3_put32(wire + 48 + i * 4,
                 (int32_t)(r * 65536.0 + (r >= 0.0 ? 0.5 : -0.5)));
    }
    wire[40] = attr->Matrix.CscOffset[0];	/* Y offset */
    wire[41] = attr->Matrix.CscOffset[1];	/* UV offset */
    wire[42] = attr->Matrix.CscClip[0];		/* Y min */
    wire[43] = attr->Matrix.CscClip[1];		/* Y max */
    wire[44] = attr->Matrix.CscClip[2];		/* UV min */
    wire[45] = attr->Matrix.CscClip[3];		/* UV max */
    memcpy(wire + 84, wire + 40, 4);
    memcpy(wire + 88, wire + 44, 4);
    return 0;
}

void OpenIMP_P3_CSCFromWire(const unsigned char wire[92], IMPISPCSCAttr *attr)
{
    int32_t version = p3_get32(wire);
    unsigned int i;

    memset(attr, 0, sizeof(*attr));
    attr->ColorGamut = (version >= 0 && version <= 3) ? (IMPISPCSCColorGamut)version
                                                       : IMP_ISP_CG_USER;
    for (i = 0; i < 9; i++)
        attr->Matrix.CscCoef[i] = (float)p3_get32(wire + 4 + i * 4) / 65536.0f;
    attr->Matrix.CscOffset[0] = wire[40];
    attr->Matrix.CscOffset[1] = wire[41];
    attr->Matrix.CscClip[0] = wire[42];
    attr->Matrix.CscClip[1] = wire[43];
    attr->Matrix.CscClip[2] = wire[44];
    attr->Matrix.CscClip[3] = wire[45];
}

int32_t IMP_ISP_Tuning_SetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    unsigned char wire[40];

    if (!ccm)
        return -1;
    OpenIMP_P3_CCMToWire(ccm, wire);
    return p3_tuning_pointer(num, 0, TISP_CID_CCM, wire);
}

int32_t IMP_ISP_Tuning_GetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    unsigned char wire[40];
    int result;

    if (!ccm)
        return -1;
    memset(wire, 0, sizeof(wire));
    result = p3_tuning_pointer(num, 1, TISP_CID_CCM, wire);
    if (result == 0)
        OpenIMP_P3_CCMFromWire(wire, ccm);
    return result;
}

int32_t IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    unsigned char wire[92];

    if (!csc || OpenIMP_P3_CSCToWire(csc, wire))
        return -1;
    return p3_tuning_pointer(num, 0, TISP_CID_CSC, wire);
}

int32_t IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    unsigned char wire[92];
    int result;

    if (!csc)
        return -1;
    memset(wire, 0, sizeof(wire));
    result = p3_tuning_pointer(num, 1, TISP_CID_CSC, wire);
    if (result == 0)
        OpenIMP_P3_CSCFromWire(wire, csc);
    return result;
}
#endif

int32_t IMP_ISP_Tuning_SetISPRunningMode(IMPVI_NUM num,
                                         IMPISPRunningMode *mode)
{
    int result;

    if (num != IMPVI_MAIN || !mode)
        return -1;
    result = p3_tuning_pointer(num, 0, TISP_CID_RUNNING_MODE, mode);
    if (result == 0)
        p3_controls.running_mode = *mode;
    return result;
}

int32_t IMP_ISP_Tuning_GetISPRunningMode(IMPVI_NUM num,
                                         IMPISPRunningMode *mode)
{
    int result;

    if (num != IMPVI_MAIN || !mode)
        return -1;
    result = p3_tuning_pointer(num, 1, TISP_CID_RUNNING_MODE, mode);
    if (result == 0)
        p3_controls.running_mode = *mode;
    *mode = p3_controls.running_mode;
    return result;
}

int32_t IMP_ISP_Tuning_SetISPBypass(IMPVI_NUM num,
                                    IMPISPTuningOpsMode *enable)
{
    if (num != IMPVI_MAIN || !enable)
        return -1;
    /*
     * OEM T40 stores this policy flag in libimp; it does not issue a tuning
     * ioctl until a separate custom-ISP configuration is supplied.
     */
    return 0;
}

int32_t IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_NUM num,
                                          IMPISPAntiflickerAttr *attribute)
{
#if defined(PLATFORM_T41)
    int result;
#endif

    if (num != IMPVI_MAIN || !attribute)
        return -1;
#if defined(PLATFORM_T41)
    result = p3_tuning_pointer(num, 0, TISP_CID_ANTIFLICKER, attribute);
    if (result != 0)
        return result;
#endif
    p3_controls.antiflicker = *attribute;
    return 0;
}

int32_t IMP_ISP_Tuning_GetAntiFlickerAttr(IMPVI_NUM num,
                                          IMPISPAntiflickerAttr *attribute)
{
#if defined(PLATFORM_T41)
    int result;
#endif

    if (num != IMPVI_MAIN || !attribute)
        return -1;
#if defined(PLATFORM_T41)
    result = p3_tuning_pointer(num, 1, TISP_CID_ANTIFLICKER, attribute);
    if (result != 0)
        return result;
    p3_controls.antiflicker = *attribute;
#endif
    *attribute = p3_controls.antiflicker;
    return 0;
}

int32_t IMP_ISP_SetDefaultBinPath(IMPVI_NUM num, char *path)
{
    size_t length;
    int result;

    if (num != IMPVI_MAIN || !path)
        return -1;
    length = strlen(path);
    if (length >= sizeof(p3_controls.bin_path))
        return -1;
    result = OpenIMP_P1_SetDefaultBinPath(num, path);
    if (result == 0)
        memcpy(p3_controls.bin_path, path, length + 1U);
    return result;
}

/* Vendor IMP_ISP_GetDefaultBinPath returns the absolute path of the bin file the
 * running ISP was started with (T40 1.3.1/en imp_isp.h:369-396).  The old body
 * answered 0 for every call and copied the setter cache, so a caller that never
 * set a path got a successful return plus an empty string, and a caller that
 * follows the header's own example - char path[64] - could receive the 128-byte
 * setter cache.  Report the path OpenIMP really forwarded to the kernel
 * (OpenIMP_P1_GetDefaultBinPath, bounded by the vendor's 64-byte example) and
 * fail when libimp was never given one. */
int32_t IMP_ISP_GetDefaultBinPath(IMPVI_NUM num, char *path)
{
    if (num != IMPVI_MAIN || path == NULL)
        return -1;
    if (OpenIMP_P1_GetDefaultBinPath(num, path, 64U) == 0)
        return 0;

    /* No path was ever accepted by IMP_ISP_SetDefaultBinPath.  The default the
     * kernel then uses is a driver property (T40 module parameter
     * t40_tuning_bin_path, T41 "/etc/sensor/<sensor>-t41.bin") and unknown to
     * libimp, so an empty string must not be reported as success. */
    path[0] = '\0';
    return -1;
}

int IMP_ISP_Tuning_SetOsdPoolSize(int size)
{
    return size >= 0 ? 0 : -1;
}

#if !defined(PLATFORM_T41)   /* T41: src/t23/openimp_t23_osd.c */
int IMP_OSD_SetPoolSize(int size)
{
    return size >= 0 ? 0 : -1;
}
#endif

static uint32_t p3_register_access(uint32_t address, const uint32_t *write_value,
                                   int *ok)
{
    long page_size = sysconf(_SC_PAGESIZE);
    uint32_t page_mask;
    uint32_t page_address;
    uint32_t page_offset;
    volatile uint32_t *reg;
    void *mapping;
    uint32_t result = 0;
    int fd;

    *ok = 0;
    if (page_size <= 0 || (address & 3U))
        return 0;
    page_mask = (uint32_t)page_size - 1U;
    page_address = address & ~page_mask;
    page_offset = address & page_mask;
    fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0)
        return 0;
    mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, (off_t)page_address);
    if (mapping == MAP_FAILED) {
        close(fd);
        return 0;
    }
    reg = (volatile uint32_t *)((unsigned char *)mapping + page_offset);
    if (write_value) {
        *reg = *write_value;
        __sync_synchronize();
    }
    result = *reg;
    *ok = 1;
    munmap(mapping, (size_t)page_size);
    close(fd);
    return result;
}

uint32_t IMP_System_ReadReg32(uint32_t address)
{
    int ok;
    return p3_register_access(address, NULL, &ok);
}

int32_t IMP_System_WriteReg32(uint32_t address, uint32_t value)
{
    int ok;
    (void)p3_register_access(address, &value, &ok);
    return ok ? 0 : -1;
}
