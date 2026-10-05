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
#if defined(PLATFORM_T41)
extern int OpenIMP_P1_TuningReady(void);
extern int OpenIMP_P1_SensorRegister(int32_t num, uint32_t *reg, int set);
#endif
extern int OpenIMP_P1_SetDefaultBinPath(IMPVI_NUM num, const char *path);

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
P3_T41_POINTER_PAIR(ModuleControl, IMPISPModuleCtl, TISP_CID_MODULE_CONTROL)

/*
 * Vendor T41 libimp 1.2.6 (disassembled) for CCM, gamma, CSC, auto zoom and
 * the WDR output mode: the same ladder in front of every tuning ioctl, with
 * the vendor's own error codes (callers log them):
 *   -4088  no ISP device (IMP_ISP_Open not called)
 *   -4087  NULL attribute pointer
 *   -4084  num >= 2
 *   -4091  tuning not enabled (IMP_ISP_EnableTuning)
 *   -4092  invalid attribute value (gamma curve type, CSC gamut)
 *   -4090  the tuning ioctl failed (the driver refused the control)
 * The request is the usual envelope with direction 0 (set) / 1 (get).
 */
#define P3V_NO_DEVICE   (-4088)
#define P3V_NULL_ARG    (-4087)
#define P3V_BAD_NUM     (-4084)
#define P3V_NOT_READY   (-4091)
#define P3V_BAD_VALUE   (-4092)
#define P3V_IOCTL       (-4090)

/* The part every function has: device, pointer, vinum (and, unless the
 * vendor function omits it, tuning state). */
static int p3v_check(IMPVI_NUM num, const void *attr, int need_tuning)
{
    int ready = OpenIMP_P1_TuningReady();

    if (ready == 0)
        return P3V_NO_DEVICE;
    if (!attr)
        return P3V_NULL_ARG;
    if ((uint32_t)num >= 2U)
        return P3V_BAD_NUM;
    if (need_tuning && ready < 2)
        return P3V_NOT_READY;
    return 0;
}

static int p3v_ioctl(IMPVI_NUM num, int32_t direction, int32_t control,
                     void *payload)
{
    P3TuningRequest request;

    request.vinum = num;
    request.direction = direction;
    request.control = control;
    request.payload = (uintptr_t)payload;
    return OpenIMP_P1_TuningIOCtl(TISP_VIDIOC_DEFAULT_TUNING, &request) ?
        P3V_IOCTL : 0;
}

/* MIPS trunc.w.s / trunc.w.d: out of range and NaN give 2^31 - 1. */
static int32_t p3v_trunc(double value)
{
    if (!(value > -2147483649.0 && value < 2147483648.0))
        return 0x7fffffff;
    return (int32_t)value;
}

/*
 * CCM: the 40-byte kernel block is { u8 ManualEn, u8 SatEn, pad[2],
 * word[9] }.  Each ColorMatrix float becomes a 13-bit word: value * 1024
 * truncated, a negative value (below -1e-5) as the 13-bit two's complement
 * of its magnitude with bit 13 set.  (The vendor library also stores the
 * negated magnitude back into the caller's matrix; this one leaves the
 * caller's attribute alone.)
 */
#define P3V_CCM_BYTES 40U

int32_t IMP_ISP_Tuning_SetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    uint32_t block[P3V_CCM_BYTES / 4U];
    int result = p3v_check(num, ccm, 1);
    unsigned int i;

    if (result)
        return result;
    memset(block, 0, sizeof(block));
    ((uint8_t *)block)[0] = (uint8_t)ccm->ManualEn;
    ((uint8_t *)block)[1] = (uint8_t)ccm->SatEn;
    for (i = 0; i < 9; i++) {
        float value = ccm->ColorMatrix[i];
        int32_t word;

        if ((double)value < -1e-5) {
            word = p3v_trunc((double)(-value * 1024.0f));
            block[1 + i] = ((uint32_t)(-word) & 0x1fffU) | 0x2000U;
        } else {
            word = p3v_trunc((double)(value * 1024.0f));
            block[1 + i] = (uint32_t)word & 0x1fffU;
        }
    }
    return p3v_ioctl(num, 0, TISP_CID_CCM, block);
}

int32_t IMP_ISP_Tuning_GetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    uint32_t block[P3V_CCM_BYTES / 4U];
    int result = p3v_check(num, ccm, 1);
    unsigned int i;

    if (result)
        return result;
    memset(block, 0, sizeof(block));
    result = p3v_ioctl(num, 1, TISP_CID_CCM, block);
    if (result)
        return result;
    ccm->ManualEn = (IMPISPTuningOpsMode)(int8_t)((uint8_t *)block)[0];
    ccm->SatEn = (IMPISPTuningOpsMode)(int8_t)((uint8_t *)block)[1];
    for (i = 0; i < 9; i++) {
        uint32_t word = block[1 + i];

        if (word & 0x2000U)
            ccm->ColorMatrix[i] =
                -(float)(int32_t)((0U - word) & 0x1fffU) * 0.0009765625f;
        else
            ccm->ColorMatrix[i] = (float)(int32_t)word * 0.0009765625f;
    }
    return 0;
}

/* Gamma: the 264-byte public attribute goes through unchanged; the vendor
 * library only rejects a curve type >= 5. */
int32_t IMP_ISP_Tuning_SetGammaAttr(IMPVI_NUM num, IMPISPGammaAttr *gamma)
{
    int result = p3v_check(num, gamma, 1);

    if (result)
        return result;
    if ((uint32_t)gamma->Curve_type >= 5U)
        return P3V_BAD_VALUE;
    return p3v_ioctl(num, 0, TISP_CID_GAMMA, gamma);
}

int32_t IMP_ISP_Tuning_GetGammaAttr(IMPVI_NUM num, IMPISPGammaAttr *gamma)
{
    int result = p3v_check(num, gamma, 1);

    if (result)
        return result;
    return p3v_ioctl(num, 1, TISP_CID_GAMMA, gamma);
}

/*
 * CSC: the kernel block is 92 bytes { u32 mode, i32 word[9], u8 offset[2],
 * u8 clip[4], ... }.  Only mode 4 (IMP_ISP_CG_USER) carries a matrix: each
 * CscCoef becomes (int)(coef * 1024 + 0.5) & 0x3ff.  The vendor library
 * checks the gamut (< 5) but not the tuning state on Set.  (The stock
 * driver numbers its modes BT601 full/limited, BT709 full/limited, BT2020
 * full/limited, user = 6, so mode 4 selects the BT2020 full preset there.)
 */
#define P3V_CSC_BYTES 92U

int32_t IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    uint32_t block[P3V_CSC_BYTES / 4U];
    int result = p3v_check(num, csc, 0);
    unsigned int i;

    if (result)
        return result;
    if ((uint32_t)csc->ColorGamut >= 5U)
        return P3V_BAD_VALUE;
    memset(block, 0, sizeof(block));
    block[0] = (uint32_t)csc->ColorGamut;
    if (csc->ColorGamut == 4) {
        for (i = 0; i < 9; i++) {
            double scaled = (double)(csc->Matrix.CscCoef[i] * 1024.0f);

            block[1 + i] = (uint32_t)p3v_trunc(scaled + 0.5) & 0x3ffU;
        }
        memcpy((uint8_t *)block + 40, csc->Matrix.CscOffset, 2);
        memcpy((uint8_t *)block + 42, csc->Matrix.CscClip, 4);
    }
    return p3v_ioctl(num, 0, TISP_CID_CSC, block);
}

int32_t IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    uint32_t block[P3V_CSC_BYTES / 4U];
    int result = p3v_check(num, csc, 1);
    unsigned int i;
    uint32_t mode;

    if (result)
        return result;
    memset(block, 0, sizeof(block));
    result = p3v_ioctl(num, 1, TISP_CID_CSC, block);
    if (result)
        return result;
    mode = block[0];
    if (mode == 4U) {
        for (i = 0; i < 9; i++)
            csc->Matrix.CscCoef[i] =
                (float)(int32_t)block[1 + i] * 0.0009765625f;
        /* rows two and three carry the magnitude of their first two
         * (negative) coefficients */
        csc->Matrix.CscCoef[3] = -csc->Matrix.CscCoef[3];
        csc->Matrix.CscCoef[4] = -csc->Matrix.CscCoef[4];
        csc->Matrix.CscCoef[7] = -csc->Matrix.CscCoef[7];
        csc->Matrix.CscCoef[8] = -csc->Matrix.CscCoef[8];
        memcpy(csc->Matrix.CscOffset, (uint8_t *)block + 40, 2);
        memcpy(csc->Matrix.CscClip, (uint8_t *)block + 42, 4);
    } else {
        memset(&csc->Matrix, 0, sizeof(csc->Matrix));
    }
    csc->ColorGamut = (IMPISPCSCColorGamut)mode;
    return 0;
}

/* Auto zoom (60 bytes) and the WDR output mode (the stock library's
 * pointer pass-through, the stock driver has no handler for 0x54). */
int32_t IMP_ISP_Tuning_SetAutoZoom(IMPVI_NUM num, IMPISPAutoZoom *zoom)
{
    int result = p3v_check(num, zoom, 1);

    return result ? result : p3v_ioctl(num, 0, TISP_CID_AUTOZOOM, zoom);
}

int32_t IMP_ISP_Tuning_GetAutoZoom(IMPVI_NUM num, IMPISPAutoZoom *zoom)
{
    int result = p3v_check(num, zoom, 1);

    return result ? result : p3v_ioctl(num, 1, TISP_CID_AUTOZOOM, zoom);
}

int32_t IMP_ISP_Tuning_SetWdrOutputMode(IMPVI_NUM num,
                                        IMPISPWdrOutputMode *mode)
{
    int result = p3v_check(num, mode, 1);

    return result ? result :
        p3v_ioctl(num, 0, TISP_CID_WDR_OUTPUT_MODE, mode);
}

int32_t IMP_ISP_Tuning_GetWdrOutputMode(IMPVI_NUM num,
                                        IMPISPWdrOutputMode *mode)
{
    int result = p3v_check(num, mode, 1);

    return result ? result :
        p3v_ioctl(num, 1, TISP_CID_WDR_OUTPUT_MODE, mode);
}

/* Sensor registers: 64-byte request on the /dev/tx-isp node, see
 * OpenIMP_P1_SensorRegister. */
int32_t IMP_ISP_SetSensorRegister(IMPVI_NUM num, IMPISPSensorRegister *reg)
{
    return OpenIMP_P1_SensorRegister(num, reg ? &reg->addr : NULL, 1);
}

int32_t IMP_ISP_GetSensorRegister(IMPVI_NUM num, IMPISPSensorRegister *reg)
{
    return OpenIMP_P1_SensorRegister(num, reg ? &reg->addr : NULL, 0);
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

int32_t IMP_ISP_GetDefaultBinPath(IMPVI_NUM num, char *path)
{
    if (num != IMPVI_MAIN || !path)
        return -1;
    strcpy(path, p3_controls.bin_path);
    return 0;
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
