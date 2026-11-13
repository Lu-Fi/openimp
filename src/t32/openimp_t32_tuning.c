/*
 * OpenIMP T32 ISP tuning (#296, planned; not a release gate).
 *
 * IMP_ISP_EnableTuning / DisableTuning and the IMP_ISP_Tuning_* calls of the
 * vendor 2.x API on /dev/isp-m0 (DEFAULT_CMD_ISP_TUNING 0xc0105467, request
 * {vinum, dir, id, value or pointer}).  Built against the vendor T32 2.2.0
 * public headers (T32_HEADERS, ingenic-headers T32/2.2.0, as the T41 build
 * uses its vendor headers), so the prototypes and structure layouts are the
 * vendor's; OpenIMP's own include/imp is not used in this file.
 *
 * Ids, directions and payloads: tests/t32/vendor-2.2.0-tuning.txt, recorded
 * with vendor libimp 2.2.0 in QEMU (tools/t32/t32_tuning_sample.c), and the
 * table in docs/T32_TUNING.md.  The checks and their return codes follow the
 * vendor library: no tuning 0xfffff008, NULL 0xfffff009, VI >= 4 0xfffff00c,
 * ioctl failure 0xfffff006 (sensor attribute 0xfffff001), bad field
 * 0xfffff004.  Most calls hand the caller's structure to the driver as it is;
 * the ones with a conversion (CCM, CSC, anti-flicker, wait-frame, mask/draw
 * block colours, BCSH cache, sensor fps) are commented where they are.
 * No vendor code copied.
 *
 * Not here: SwitchBin and the running mode (openimp_t32_bin.c, #289), the
 * ISP OSD regions (IMP_ISP_Tuning_*OsdRgn*, OSD issue), the vendor's
 * multi-process server mode.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>

#include "imp_log_int.h"
#include "isp/isp_mask_rgb2yuv.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_tuning.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-TUN"

/*
 * The vendor public structures whose size differs between the T32 2.2.0 and
 * the T33 2.0.2.1 headers (the other 279 common typedefs are equal; audit of
 * #390).  IMPISPBinInfo is a header type only: the DEFAULT_BIN request of
 * both libimp lines is 880 bytes (version at 863, SoC at 871,
 * openimp_t32_bin.c), the T33 header's 856 is not what the T33 library
 * passes.  None of the three crosses an ioctl this library issues.
 */
#ifdef PLATFORM_T33
_Static_assert(sizeof(IMPISPBinInfo) == 856, "T33 2.0.2.1 IMPISPBinInfo");
_Static_assert(sizeof(tisp_bin_attr_t) == 828, "T33 2.0.2.1 tisp_bin_attr_t (no paddr[6])");
_Static_assert(sizeof(IMPEncoderKernEncOut) == 48, "T33 2.0.2.1 IMPEncoderKernEncOut");
#else
_Static_assert(sizeof(IMPISPBinInfo) == 880, "T32 2.2.0 IMPISPBinInfo");
_Static_assert(sizeof(tisp_bin_attr_t) == 852, "T32 2.2.0 tisp_bin_attr_t");
_Static_assert(sizeof(IMPEncoderKernEncOut) == 56, "T32 2.2.0 IMPEncoderKernEncOut");
#endif

#define T32_TUN_IOC      0xc0105467u    /* /dev/isp-m0 DEFAULT_CMD_ISP_TUNING */
#define T32_AE_START_SET 0xc0045436u    /* /dev/tx-isp SET/GET_AE_START_EV */
#define T32_AE_START_GET 0xc0045437u
#define T32_SET 0
#define T32_GET 1
#define T32_VI_MAX 4

/* vendor return codes */
#define E_SENSOR  ((int32_t)0xfffff001)
#define E_FIELD   ((int32_t)0xfffff004)
#define E_STATE   ((int32_t)0xfffff005)
#define E_IOCTL   ((int32_t)0xfffff006)
#define E_TUNING  ((int32_t)0xfffff008)
#define E_NULL    ((int32_t)0xfffff009)
#define E_VI      ((int32_t)0xfffff00c)

/* tuning ids (vendor 2.2.0, recorded: tests/t32/vendor-2.2.0-tuning.txt) */
enum {
    ID_STATIS_CONFIG = 0x08000000, ID_AWB_ATTR = 0x08000010, ID_AWB_STATIS = 0x08000011,
    ID_AWB_WEIGHT = 0x08000012, ID_AWB_GLOBAL_STATIS = 0x08000013, ID_FACE_AWB = 0x08000014,
    ID_AWB_ONLY_READ = 0x08000015, ID_AWB_CT_TREND = 0x08000017, ID_AE_WEIGHT = 0x08000021,
    ID_AE_STATIS = 0x08000022, ID_AE_EXPR_INFO = 0x08000023, ID_GAMMA = 0x08000025,
    ID_ANTIFLICKER = 0x08000026, ID_AE_EXP_LIST = 0x08000027, ID_AE_FLICKER_FLAG = 0x08000028,
    ID_FACE_AE_LUMA = 0x08000029, ID_FACE_AE_WEIGHT = 0x0800002a, ID_AE_ONLY_READ = 0x0800002b,
    ID_AE_TARGET = 0x0800002d, ID_AE_LIGHT_COMP = 0x0800002e, ID_AF_STATIS = 0x08000031,
    ID_AF_WEIGHT = 0x08000032, ID_SENSOR_ATTR = 0x08000033, ID_AF_METRICS = 0x08000034,
    ID_FACE_AE = 0x08000035, ID_WDR_OUTPUT = 0x08000054, ID_TMO_FACEAE = 0x08000062,
    ID_SENSOR_FPS = 0x08000070, ID_MODULE_CTL = 0x08000072, ID_HVFLIP = 0x08000073,
    ID_MASK_BLOCK = 0x08000074, ID_AUTO_ZOOM = 0x08000077, ID_CCM = 0x08000080,
    ID_BCSH_HUE = 0x08000081, ID_WAIT_FRAME = 0x08000091, ID_BRIGHTNESS = 0x08000092,
    ID_SHARPNESS = 0x08000093, ID_SATURATION = 0x08000094, ID_CONTRAST = 0x08000095,
    ID_CSC = 0x08000096, ID_CSCCR = 0x08000097, ID_MS_AE = 0x08000098, ID_MS_AWB = 0x08000099,
    ID_MS_EFFECT = 0x0800009a, ID_BCSH_RGB_OFFSET = 0x0800009b, ID_DRAW_BLOCK = 0x080000a0,
    ID_OSD_ATTR = 0x080000a1, ID_OSD_BLOCK = 0x080000a2, ID_MODULE_RATIO = 0x080000a4,
    ID_SCALER_LV = 0x080000a6, ID_HB = 0x080000a7,
};

/* vendor 2.2.0 layouts this file reads or converts (sizes measured, #296) */
_Static_assert(sizeof(IMPISPSensorFps) == 8, "T32 IMPISPSensorFps");
_Static_assert(sizeof(IMPISPAntiflickerAttr) == 8, "T32 IMPISPAntiflickerAttr");
_Static_assert(sizeof(IMPISPHVFLIPAttr) == 16, "T32 IMPISPHVFLIPAttr");
_Static_assert(sizeof(IMPISPGammaAttr) == 264, "T32 IMPISPGammaAttr");
_Static_assert(sizeof(IMPISPCCMAttr) == 44, "T32 IMPISPCCMAttr");
_Static_assert(sizeof(IMPISPCSCAttr) == 92, "T32 IMPISPCSCAttr");
_Static_assert(sizeof(IMPISPAeStartAttr) == 8, "T32 IMPISPAeStartAttr");
_Static_assert(sizeof(IMPISPWaitFrameAttr) == 24, "T32 IMPISPWaitFrameAttr");
_Static_assert(sizeof(IMPISPMaskBlockAttr) == 24, "T32 IMPISPMaskBlockAttr");
_Static_assert(sizeof(IMPISPDrawBlockAttr) == 36, "T32 IMPISPDrawBlockAttr");
_Static_assert(sizeof(IMPISPAFStatisInfo) == 5400, "T32 IMPISPAFStatisInfo");

typedef struct {
    int32_t vinum;
    int32_t dir;
    uint32_t id;
    uint32_t value;             /* value or user pointer */
} T32TunReq;

/* BCSH cache (vendor: bytes 4.. of the tuning block, 0x80 at enable) */
enum { BCSH_BRIGHT, BCSH_CONTRAST, BCSH_SHARP, BCSH_SAT, BCSH_HUE, BCSH_N };

static struct {
    pthread_mutex_t lock;
    int fd;                     /* /dev/isp-m0, -1 = tuning off */
    uint8_t bcsh[BCSH_N][T32_VI_MAX];
    void (*video_drop)(void);
} tun = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
};

static int32_t tun_ioctl(int vinum, int dir, uint32_t id, uint32_t value, uint32_t *out)
{
    T32TunReq r = { vinum, dir, id, value };
    int fd = tun.fd;

    if (fd < 0)
        return E_TUNING;
    if (ioctl(fd, T32_TUN_IOC, &r) != 0) {
        IMP_LOG_ERR(TAG, "tuning %s id 0x%08x: %s", dir ? "get" : "set", id, strerror(errno));
        return E_IOCTL;
    }
    if (out)
        *out = r.value;
    return 0;
}

static int32_t tun_ptr(int vinum, int dir, uint32_t id, const void *p)
{
    return tun_ioctl(vinum, dir, id, (uint32_t)(uintptr_t)p, NULL);
}

/* the vendor's common checks: tuning on, pointer, VI number */
static int32_t tun_check(int num, const void *p)
{
    if (tun.fd < 0)
        return E_TUNING;
    if (!p)
        return E_NULL;
    if ((unsigned)num >= T32_VI_MAX)
        return E_VI;
    return 0;
}

static int32_t tun_pass(int num, int dir, uint32_t id, void *p)
{
    int32_t rc = tun_check(num, p);

    return rc ? rc : tun_ptr(num, dir, id, p);
}

/* ---- enable / disable ---------------------------------------------------- */
int32_t IMP_ISP_EnableTuning(void)
{
    uint32_t fps;
    int fd, i;

    if (OpenIMP_T32_IspFd() < 0)
        return E_STATE;         /* ISP not open */
    pthread_mutex_lock(&tun.lock);
    if (tun.fd >= 0) {
        pthread_mutex_unlock(&tun.lock);
        return 0;
    }
    fd = open("/dev/isp-m0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "open /dev/isp-m0: %s", strerror(errno));
        pthread_mutex_unlock(&tun.lock);
        return E_TUNING;
    }
    memset(tun.bcsh, 0x80, sizeof(tun.bcsh));
    tun.fd = fd;
    pthread_mutex_unlock(&tun.lock);
    /* the sensor rate of every streaming sensor (vendor: for FrameSource;
     * the T32 FrameSource reads it per frame from the driver) */
    for (i = 0; i < T32_VI_MAX; i++)
        if (OpenIMP_T32_SensorState(i) == 2 && tun_ioctl(i, T32_GET, ID_SENSOR_FPS, 0, &fps) == 0)
            IMP_LOG_DBG(TAG, "VI %d sensor fps %u/%u", i, fps >> 16, fps & 0xffff);
    return 0;
}

int32_t IMP_ISP_DisableTuning(void)
{
    pthread_mutex_lock(&tun.lock);
    if (tun.fd >= 0) {
        close(tun.fd);
        tun.fd = -1;
    }
    pthread_mutex_unlock(&tun.lock);
    return 0;
}

/* ---- sensor ---------------------------------------------------------------- */
/* value word num << 16 | den; the vendor checks no tuning state here */
int32_t IMP_ISP_Tuning_SetSensorFPS(IMPVI_NUM num, IMPISPSensorFps *fps)
{
    int32_t rc = tun_check(num, fps);

    if (rc)
        return rc;
    return tun_ioctl(num, T32_SET, ID_SENSOR_FPS, fps->num << 16 | fps->den, NULL);
}

/* vendor: the ioctl's own result (-1) on failure */
int32_t IMP_ISP_Tuning_GetSensorFPS(IMPVI_NUM num, IMPISPSensorFps *fps)
{
    uint32_t v;
    int32_t rc = tun_check(num, fps);

    if (rc)
        return rc;
    if (tun_ioctl(num, T32_GET, ID_SENSOR_FPS, 0, &v))
        return -1;
    fps->num = v >> 16;
    fps->den = v & 0xffff;
    return 0;
}

int32_t IMP_ISP_Tuning_GetSensorAttr(IMPVI_NUM num, IMPISPSENSORAttr *attr)
{
    int32_t rc = tun_check(num, attr);

    if (rc)
        return rc;
    return tun_ptr(num, T32_GET, ID_SENSOR_ATTR, attr) ? E_SENSOR : 0;
}

/* the callback is kept; the T32 FrameSource drops no frames for it yet */
int32_t IMP_ISP_Tuning_SetVideoDrop(void (*cb)(void))
{
    pthread_mutex_lock(&tun.lock);
    tun.video_drop = cb;
    pthread_mutex_unlock(&tun.lock);
    return 0;
}

/* ---- BCSH ------------------------------------------------------------------- */
static int32_t bcsh_set(int num, unsigned char *v, uint32_t id, int slot)
{
    int32_t rc = tun_pass(num, T32_SET, id, v);

    if (!rc)
        tun.bcsh[slot][num] = *v;
    return rc;
}

/* answered from the last value set (0x80 after EnableTuning), as the vendor */
static int32_t bcsh_get(int num, unsigned char *v, int slot)
{
    int32_t rc = tun_check(num, v);

    if (!rc)
        *v = tun.bcsh[slot][num];
    return rc;
}

#define BCSH(name, id, slot) \
    int32_t IMP_ISP_Tuning_Set##name(IMPVI_NUM num, unsigned char *v) { return bcsh_set(num, v, id, slot); } \
    int32_t IMP_ISP_Tuning_Get##name(IMPVI_NUM num, unsigned char *v) { return bcsh_get(num, v, slot); }
BCSH(Brightness, ID_BRIGHTNESS, BCSH_BRIGHT)
BCSH(Contrast, ID_CONTRAST, BCSH_CONTRAST)
BCSH(Sharpness, ID_SHARPNESS, BCSH_SHARP)
BCSH(Saturation, ID_SATURATION, BCSH_SAT)
BCSH(BcshHue, ID_BCSH_HUE, BCSH_HUE)

/* ---- plain pass-through (the driver reads / fills the caller's structure) ---- */
#define PASS_SET(fn, type, id) \
    int32_t fn(IMPVI_NUM num, type *p) { return tun_pass(num, T32_SET, id, p); }
#define PASS_GET(fn, type, id) \
    int32_t fn(IMPVI_NUM num, type *p) { return tun_pass(num, T32_GET, id, p); }
#define PASS(name, type, id) \
    PASS_SET(IMP_ISP_Tuning_Set##name, type, id) PASS_GET(IMP_ISP_Tuning_Get##name, type, id)

#ifndef PLATFORM_T33	/* the T33 2.0.2.1 API has no BCSH RGB offset (#390) */
PASS(BcshRGBOffsetAttr, IMPISPBCSHRGBOffsetAttr, ID_BCSH_RGB_OFFSET)
#endif
PASS(ModuleControl, IMPISPModuleCtl, ID_MODULE_CTL)
PASS(Module_Ratio, IMPISPModuleRatioAttr, ID_MODULE_RATIO)
PASS(StatisConfig, IMPISPStatisConfig, ID_STATIS_CONFIG)
PASS(AeExprInfo, IMPISPAEExprInfo, ID_AE_EXPR_INFO)
PASS(AeTargetAttr, IMPISPAeTargetAttr, ID_AE_TARGET)
PASS(AeLightCompensation, IMPISPAeLightCompensationAttr, ID_AE_LIGHT_COMP)
PASS(AwbAttr, IMPISPWBAttr, ID_AWB_ATTR)
PASS(AfWeight, IMPISPWeight, ID_AF_WEIGHT)
PASS(AwbWeight, IMPISPAwbWeight, ID_AWB_WEIGHT)
PASS(AeWeight, IMPISPAEWeightAttr, ID_AE_WEIGHT)
PASS(AeExpList, IMPISPAeExpListAttr, ID_AE_EXP_LIST)
PASS(AutoZoom, IMPISPAutoZoom, ID_AUTO_ZOOM)
PASS(OSDAttr, IMPISPOSDAttr, ID_OSD_ATTR)
PASS(OSDBlock, IMPISPOSDBlockAttr, ID_OSD_BLOCK)
PASS(WdrOutputMode, IMPISPWdrOutputMode, ID_WDR_OUTPUT)
PASS(AwbCtTrendOffset, IMPISPAwbCtTrendOffset, ID_AWB_CT_TREND)
PASS(FaceAe, IMPISPFaceAttr, ID_FACE_AE)
PASS(FaceAeWeiget, IMPISPFaceAeWeiget, ID_FACE_AE_WEIGHT)
PASS(FaceAwb, IMPISPFaceAttr, ID_FACE_AWB)
PASS_GET(IMP_ISP_Tuning_GetAeOnlyReadAttr, IMPISPAeOnlyReadAttr, ID_AE_ONLY_READ)
PASS_GET(IMP_ISP_Tuning_GetAwbStatistics, IMPISPAWBStatisInfo, ID_AWB_STATIS)
PASS_GET(IMP_ISP_Tuning_GetAwbGlobalStatistics, IMPISPAWBGlobalStatisInfo, ID_AWB_GLOBAL_STATIS)
PASS_GET(IMP_ISP_Tuning_GetAeStatistics, IMPISPAEStatisInfo, ID_AE_STATIS)
PASS_GET(IMP_ISP_Tuning_GetAFMetricesInfo, IMPISPAFMetricsInfo, ID_AF_METRICS)
PASS_GET(IMP_ISP_Tuning_GetAEFlickerFlag, IMPISPFlickerFlag, ID_AE_FLICKER_FLAG)
PASS_GET(IMP_ISP_Tuning_GetAwbOnlyReadAttr, IMPISPAwbOnlyReadAttr, ID_AWB_ONLY_READ)
PASS_GET(IMP_ISP_Tuning_GetFaceAeLuma, IMPISPFaceAeLuma, ID_FACE_AE_LUMA)
PASS_GET(IMP_ISP_Tuning_GetHVFLIP, IMPISPHVFLIPAttr, ID_HVFLIP)
PASS_GET(IMP_ISP_Tuning_GetGammaAttr, IMPISPGammaAttr, ID_GAMMA)
/* the vendor's other statistics layout (state word 0 == 1, a 0x1518-byte
 * driver block re-sorted) is not reached on PRJ007: pass-through only */
PASS_GET(IMP_ISP_Tuning_GetAfStatistics, IMPISPAFStatisInfo, ID_AF_STATIS)
PASS_SET(IMP_ISP_Tuning_SetScalerLv, IMPISPScalerLvAttr, ID_SCALER_LV)
PASS_SET(IMP_ISP_Tuning_SetTmoFaceae, IMPISPTmoFaceae, ID_TMO_FACEAE)
PASS_SET(IMP_ISP_SetCsccrMode, IMPISPCsccrModeAttr, ID_CSCCR)
PASS_GET(IMP_ISP_GetCsccrMode, IMPISPCsccrModeAttr, ID_CSCCR)
PASS_SET(IMP_ISP_HB_SetAttr, uint16_t, ID_HB)
PASS_GET(IMP_ISP_HB_GetAttr, uint16_t, ID_HB)

#ifndef PLATFORM_T33	/* no IMPISPSensorSyncEffect / multi-sensor effect API in T33 2.0.2.1 (#390) */
/* multi-sensor sync: Set always goes to VI 0, Get takes any VI (no range check) */
#define MULTI(name, id) \
    int32_t IMP_ISP_Tuning_Set##name(IMPISPSensorSyncEffect *p) \
    { return tun.fd < 0 ? E_TUNING : !p ? E_NULL : tun_ptr(0, T32_SET, id, p); } \
    int32_t IMP_ISP_Tuning_Get##name(IMPVI_NUM num, IMPISPSensorSyncEffect *p) \
    { return tun.fd < 0 ? E_TUNING : !p ? E_NULL : tun_ptr(num, T32_GET, id, p); }
MULTI(MultiSensorAeEffect, ID_MS_AE)
MULTI(MultiSensorAWBEffect, ID_MS_AWB)
MULTI(MultiSensorEffect, ID_MS_EFFECT)
#endif

/* ---- with field checks ------------------------------------------------------- */
int32_t IMP_ISP_Tuning_SetHVFLIP(IMPVI_NUM num, IMPISPHVFLIPAttr *attr)
{
    uint32_t w[4];
    int32_t rc = tun_check(num, attr);

    if (rc)
        return rc;
    memcpy(w, attr, sizeof(w));
    if (w[1] >= 4 || w[2] >= 4 || w[3] >= 4 || w[0] >= 4)
        return E_FIELD;
    return tun_ptr(num, T32_SET, ID_HVFLIP, attr);
}

/* gamma mode word < 5 (the vendor's WDR-only mode 3 skip needs a WDR ISP) */
int32_t IMP_ISP_Tuning_SetGammaAttr(IMPVI_NUM num, IMPISPGammaAttr *gamma)
{
    uint32_t mode;
    int32_t rc = tun_check(num, gamma);

    if (rc)
        return rc;
    memcpy(&mode, gamma, sizeof(mode));
    if (mode >= 5)
        return E_FIELD;
    return tun_ptr(num, T32_SET, ID_GAMMA, gamma);
}

/* ---- converted payloads ---------------------------------------------------- */
/* anti-flicker: driver payload {u32 mode; u8 freq} (mode < 3, freq < 256) */
typedef struct {
    uint32_t mode;
    uint8_t freq;
    uint8_t pad[3];
} T32Antiflicker;

int32_t IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_NUM num, IMPISPAntiflickerAttr *attr)
{
    uint32_t w[2];
    T32Antiflicker a;
    int32_t rc = tun_check(num, attr);

    if (rc)
        return rc;
    memcpy(w, attr, sizeof(w));
    if (w[0] >= 3 || (w[0] != 0 && w[1] >= 256))
        return E_FIELD;
    memset(&a, 0, sizeof(a));
    a.mode = w[0];
    a.freq = (uint8_t)w[1];
    return tun_ptr(num, T32_SET, ID_ANTIFLICKER, &a);
}

int32_t IMP_ISP_Tuning_GetAntiFlickerAttr(IMPVI_NUM num, IMPISPAntiflickerAttr *attr)
{
    uint32_t w[2];
    T32Antiflicker a;
    int32_t rc = tun_check(num, attr);

    if (rc)
        return rc;
    memset(&a, 0, sizeof(a));
    rc = tun_ptr(num, T32_GET, ID_ANTIFLICKER, &a);
    if (!rc) {
        w[0] = a.mode;
        w[1] = a.freq;
        memcpy(attr, w, sizeof(w));
    }
    return rc;
}

/*
 * CCM: user {u32 manual, u32 sat, float ccm[9]} -> driver {u8 manual, u8 sat,
 * 2 pad, u32 ccm[9]}, coefficients sign-magnitude 1.10.. : |c| * 1024 in 13
 * bits, bit 13 = negative (values >= -1e-5 count as positive).  The vendor
 * also negates the caller's negative coefficients in place; not done here.
 */
#define CCM_SCALE 1024.0f
#define CCM_NEG   0x2000u
#define CCM_MAG   0x1fffu

typedef struct {
    uint8_t manual, sat, pad[2];
    uint32_t c[9];
} T32CcmWire;

int32_t IMP_ISP_Tuning_SetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    uint32_t w[2];
    float f[9];
    T32CcmWire x;
    int32_t rc = tun_check(num, ccm);
    int i;

    if (rc)
        return rc;
    memcpy(w, ccm, sizeof(w));
    memcpy(f, (const uint8_t *)ccm + 8, sizeof(f));
    memset(&x, 0, sizeof(x));
    x.manual = (uint8_t)w[0];
    x.sat = (uint8_t)w[1];
    for (i = 0; i < 9; i++) {
        if ((double)f[i] >= -0.00001)
            x.c[i] = (uint32_t)(int32_t)(f[i] * CCM_SCALE) & CCM_MAG;
        else
            x.c[i] = ((uint32_t)-(int32_t)(-f[i] * CCM_SCALE) & CCM_MAG) | CCM_NEG;
    }
    return tun_ptr(num, T32_SET, ID_CCM, &x);
}

int32_t IMP_ISP_Tuning_GetCCMAttr(IMPVI_NUM num, IMPISPCCMAttr *ccm)
{
    int32_t w[2];
    float f[9];
    T32CcmWire x;
    int32_t rc = tun_check(num, ccm);
    int i;

    if (rc)
        return rc;
    memset(&x, 0, sizeof(x));
    rc = tun_ptr(num, T32_GET, ID_CCM, &x);
    if (rc)
        return rc;
    for (i = 0; i < 9; i++) {
        if (x.c[i] & CCM_NEG)
            f[i] = -(float)(-x.c[i] & CCM_MAG) * (1.0f / CCM_SCALE);
        else
            f[i] = (float)(int32_t)x.c[i] * (1.0f / CCM_SCALE);
    }
    w[0] = (int8_t)x.manual;    /* sign-extended bytes, as the vendor */
    w[1] = (int8_t)x.sat;
    memcpy(ccm, w, sizeof(w));
    memcpy((uint8_t *)ccm + 8, f, sizeof(f));
    return 0;
}

/*
 * CSC (92 B both sides): mode word, two 9-coefficient float blocks at +4 and
 * +48, bytes +40..+45 and +84..+89.  Mode 6 (user matrix): the driver block
 * carries the coefficients as int(c * 65536 + 0.5); Get divides by 65535
 * (the vendor's asymmetric scale); other modes carry only the mode word
 * (the vendor leaves the rest of its stack block uninitialised: zero here).
 */
#define CSC_MODE_USER 6
#define CSC_MODES     7

int32_t IMP_ISP_Tuning_SetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    uint8_t in[92], x[92];
    uint32_t mode;
    int32_t rc = tun_check(num, csc);
    int i;

    if (rc)
        return rc;
    memcpy(in, csc, sizeof(in));
    memcpy(&mode, in, 4);
    if (mode >= CSC_MODES)
        return E_FIELD;
    memset(x, 0, sizeof(x));
    memcpy(x, &mode, 4);
    if (mode == CSC_MODE_USER) {
        for (i = 0; i < 9; i++) {
            float a, b;
            int32_t ia, ib;

            memcpy(&a, in + 4 + 4 * i, 4);
            memcpy(&b, in + 48 + 4 * i, 4);
            ia = (int32_t)((double)(a * 65536.0f) + 0.5);
            ib = (int32_t)((double)(b * 65536.0f) + 0.5);
            memcpy(x + 4 + 4 * i, &ia, 4);
            memcpy(x + 48 + 4 * i, &ib, 4);
        }
        memcpy(x + 40, in + 40, 6);
        memcpy(x + 84, in + 84, 6);
    }
    return tun_ptr(num, T32_SET, ID_CSC, x);
}

int32_t IMP_ISP_Tuning_GetISPCSCAttr(IMPVI_NUM num, IMPISPCSCAttr *csc)
{
    uint8_t x[92], out[92];
    uint32_t mode;
    int32_t rc = tun_check(num, csc);
    int i;

    if (rc)
        return rc;
    memset(x, 0, sizeof(x));
    rc = tun_ptr(num, T32_GET, ID_CSC, x);
    if (rc)
        return rc;
    memcpy(&mode, x, 4);
    if (mode == CSC_MODE_USER) {
        memcpy(out, csc, sizeof(out));
        for (i = 0; i < 9; i++) {
            int32_t ia, ib;
            float a, b;

            memcpy(&ia, x + 4 + 4 * i, 4);
            memcpy(&ib, x + 48 + 4 * i, 4);
            a = (float)ia / 65535.0f;
            b = (float)ib / 65535.0f;
            memcpy(out + 4 + 4 * i, &a, 4);
            memcpy(out + 48 + 4 * i, &b, 4);
        }
        memcpy(out + 40, x + 40, 6);
        memcpy(out + 84, x + 84, 6);
    } else {
        memset(out, 0, sizeof(out));
    }
    memcpy(out, &mode, 4);
    memcpy(csc, out, sizeof(out));
    return 0;
}

/* wait for a frame-done: driver block = the caller's timeout word, words 2..4
 * come back (frame count and time) */
int32_t IMP_ISP_Tuning_WaitFrameDone(IMPVI_NUM num, IMPISPWaitFrameAttr *attr)
{
    uint32_t x[6], w[6];
    int32_t rc = tun_check(num, attr);

    if (rc)
        return rc;
    memcpy(w, attr, sizeof(w));
    memset(x, 0, sizeof(x));
    x[0] = w[0];
    rc = tun_ptr(num, T32_GET, ID_WAIT_FRAME, x);
    if (rc)
        return rc;
    memcpy(&w[2], &x[2], 3 * sizeof(uint32_t));
    memcpy(attr, w, sizeof(w));
    return 0;
}

/* mask / draw block: an RGB colour gets its YUV next to it (bytes +3..+5 of
 * the colour) before the driver sees it, BT.601 as the T31 mask path */
static void tun_rgb_yuv(uint8_t *c)
{
    uint8_t t[3] = { c[0], c[1], c[2] };

    isp_mask_rgb_to_yuv(t);
    c[3] = t[0];
    c[4] = t[1];
    c[5] = t[2];
}

/*
 * The vendor keeps the last Set mask / draw block per VI and puts the caller's
 * colour form back on Get (the driver answers YUV only), #336:
 *  - mask: entry chx * pinum (the vendor's index; 12 entries of 24 bytes per
 *    VI), Get takes mask_type (+12) and the RGB bytes (+16..+18) of the entry
 *    the driver's chx / pinum bytes select;
 *  - draw: one 36-byte slot per VI (Set always writes it), Get takes
 *    color_type (+8) and the RGB bytes (+22..+24) of slot "pinum"; the vendor
 *    reads past its slot for pinum > 0 (other data), here only pinum 0 is
 *    answered from the cache, others keep what the driver returned.
 */
#define T32_MASK_CACHE 12
static uint8_t tun_mask_cache[T32_VI_MAX][T32_MASK_CACHE][24];
static uint8_t tun_draw_cache[T32_VI_MAX][36];

int32_t IMP_ISP_Tuning_SetMaskBlock(IMPVI_NUM num, IMPISPMaskBlockAttr *mask)
{
    uint8_t *b = (uint8_t *)mask;
    int32_t type;
    unsigned e;
    int32_t rc = tun_check(num, mask);

    if (rc)
        return rc;
    memcpy(&type, b + 12, 4);
    if (b[2] && type == 0)      /* enabled, colour given as RGB */
        tun_rgb_yuv(b + 16);
    e = (unsigned)b[0] * b[1];
    if (e < T32_MASK_CACHE)
        memcpy(tun_mask_cache[num][e], b, 24);
    return tun_ptr(num, T32_SET, ID_MASK_BLOCK, mask);
}

int32_t IMP_ISP_Tuning_GetMaskBlock(IMPVI_NUM num, IMPISPMaskBlockAttr *mask)
{
    uint8_t *b = (uint8_t *)mask;
    unsigned e;
    int32_t rc = tun_pass(num, T32_GET, ID_MASK_BLOCK, mask);

    if (rc)
        return rc;
    e = (unsigned)b[0] * b[1];
    if (e < T32_MASK_CACHE) {
        memcpy(b + 12, tun_mask_cache[num][e] + 12, 4);
        memcpy(b + 16, tun_mask_cache[num][e] + 16, 3);
    }
    return 0;
}

int32_t IMP_ISP_Tuning_SetDrawBlock(IMPVI_NUM num, IMPISPDrawBlockAttr *draw)
{
    uint8_t *b = (uint8_t *)draw;
    int32_t type, rgb;
    int32_t rc = tun_check(num, draw);

    if (rc)
        return rc;
    memcpy(&type, b + 4, 4);
    /* window, range, line and (T32) cover get the RGB -> YUV conversion; any
     * other type goes to the driver as it is (vendor 2.2.0) */
    memcpy(&rgb, b + 8, 4);
    if (type >= 0 && type <= 3 && b[12] && rgb == 0)
        tun_rgb_yuv(b + 22);
    memcpy(tun_draw_cache[num], b, 36);
    return tun_ptr(num, T32_SET, ID_DRAW_BLOCK, draw);
}

int32_t IMP_ISP_Tuning_GetDrawBlock(IMPVI_NUM num, IMPISPDrawBlockAttr *draw)
{
    uint8_t *b = (uint8_t *)draw;
    int32_t rc = tun_pass(num, T32_GET, ID_DRAW_BLOCK, draw);

    if (rc)
        return rc;
    if (b[0] == 0) {
        memcpy(b + 8, tun_draw_cache[num] + 8, 4);
        memcpy(b + 22, tun_draw_cache[num] + 22, 3);
    }
    return 0;
}

/* ---- AE start (on /dev/tx-isp, needs no tuning) ---------------------------- */
int32_t IMP_ISP_Tuning_SetAeStartAttr(IMPVI_NUM num, IMPISPAeStartAttr *attr)
{
    uint32_t r[4] = { 0 };
    int fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return E_TUNING;
    if ((unsigned)num >= T32_VI_MAX || !OpenIMP_T32_SensorState(num))
        return E_STATE;
    if (!attr)
        return E_TUNING;
    r[0] = num;
    memcpy(&r[1], attr, 8);
    return ioctl(fd, T32_AE_START_SET, r) ? E_SENSOR : 0;
}

int32_t IMP_ISP_Tuning_GetAeStartAttr(IMPVI_NUM num, IMPISPAeStartAttr *attr)
{
    uint32_t r[4] = { 0 };
    int fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return E_TUNING;
    if ((unsigned)num >= T32_VI_MAX || !OpenIMP_T32_SensorState(num))
        return E_STATE;
    if (!attr)
        return E_TUNING;
    r[0] = num;
    if (ioctl(fd, T32_AE_START_GET, r))
        return E_SENSOR;
    /* the vendor writes a third word past the 8-byte structure; not here */
    memcpy(attr, &r[1], 8);
    return 0;
}

/* ---- streamer-used ISP calls (#446) ---------------------------------------
 * Checks and return codes of vendor libimp T32 2.2.0 / T33 2.0.2.1
 * (disassembly; no vendor code copied):
 *   GetDefaultBinPath  ISP open (0xfffff008), attr, VI < 4; the default bin
 *                      of attr->mode (vendor: asked from its driver, which
 *                      keeps what the library set; on the open stack the
 *                      library owns the bins, openimp_t32_bin.c)
 *   Set/GetFrameDrop   ISP open, attr, VI; SET/GET_FRAME_DROP with {vi, 3 x
 *                      {enable, lsize, fmark}} (40 B), driver error 0xfffff001
 *   Set/GetISPBypass   Set: attr, VI; BYPASS_SENSOR {enable, vi}, the value
 *                      kept; Get: ISP open, attr, VI, tuning enabled
 *                      (0xfffff005), the kept value
 *   Set/GetSensorRegister  ISP open, attr, VI, sensor enabled and on I2C
 *                      (0xfffff009 otherwise); S/G_REGISTER with the 64 B
 *                      block {name[32], vi, bus type, size, pad, reg u64,
 *                      val u64}; driver error 0xfffff001
 *   GetCameraInputMode attr; zeroes the 108 B structure, 0 (as the vendor)
 *   WDR_ENABLE         ISP open (vendor: no check, dereferences), attr, VI,
 *                      value 0/1 (0xfffff004); the WDR bit of the bin mode:
 *                      kept before AddSensor, else the WDR / non-WDR bin is
 *                      loaded (0xfffff006 when that fails)
 *   WDR_ENABLE_GET     ISP open, attr, VI, tuning enabled; DEFAULT_BIN
 *                      (880 B, VI at byte 862), WDR word at byte 852
 * The vendor's SetFrameDrop also recomputes its FrameSource drop table; the
 * open FrameSource takes the rate from the driver per frame instead. */
#define T32X_IOC_SET_SENSOR_REGISTER 0xc040540du
#define T32X_IOC_GET_SENSOR_REGISTER 0x8040540eu
#define T32X_IOC_BYPASS_SENSOR       0x80085418u
#define T32X_IOC_SET_FRAME_DROP      0xc004542cu
#define T32X_IOC_GET_FRAME_DROP      0xc004542du
#define T32X_IOC_DEFAULT_BIN         0xc00454c9u
#define T32X_DBIN_SIZE 880
#define T32X_DBIN_WDR  852
#define T32X_DBIN_VI   862

_Static_assert(sizeof(IMPISPFrameDropAttr) == 36, "IMPISPFrameDropAttr");
_Static_assert(sizeof(IMPISPCameraInputMode) == 108, "IMPISPCameraInputMode");
_Static_assert(sizeof(IMPISPSensorRegister) == 8, "IMPISPSensorRegister");

static int32_t isp_bypass[T32_VI_MAX];

static int32_t ispx_check(int num, const void *p)
{
    if (OpenIMP_T32_IspFd() < 0)
        return E_TUNING;
    if (!p)
        return E_NULL;
    if ((unsigned)num >= T32_VI_MAX)
        return E_VI;
    return 0;
}

int32_t IMP_ISP_GetDefaultBinPath(IMPVI_NUM num, IMPISPDefaultBinAttr *attr)
{
    int32_t rc = ispx_check(num, attr);

    if (rc)
        return rc;
    if (OpenIMP_T32_BinDefaultPath(num, (int)attr->mode, attr->bname) < 0)
        return E_NULL;
    return 0;
}

struct ispx_fdrop {
    uint32_t vi;
    IMPISPFrameDropAttr attr;
};

int32_t IMP_ISP_SetFrameDrop(IMPVI_NUM num, IMPISPFrameDropAttr *attr)
{
    struct ispx_fdrop b;
    int32_t rc = ispx_check(num, attr);

    if (rc)
        return rc;
    b.vi = (uint32_t)num;
    b.attr = *attr;
    if (ioctl(OpenIMP_T32_IspFd(), T32X_IOC_SET_FRAME_DROP, &b) != 0) {
        IMP_LOG_ERR(TAG, "SET_FRAME_DROP: %s", strerror(errno));
        return E_SENSOR;
    }
    return 0;
}

int32_t IMP_ISP_GetFrameDrop(IMPVI_NUM num, IMPISPFrameDropAttr *attr)
{
    struct ispx_fdrop b;
    int32_t rc = ispx_check(num, attr);

    if (rc)
        return rc;
    memset(&b, 0, sizeof(b));
    b.vi = (uint32_t)num;
    if (ioctl(OpenIMP_T32_IspFd(), T32X_IOC_GET_FRAME_DROP, &b) != 0) {
        IMP_LOG_ERR(TAG, "GET_FRAME_DROP: %s", strerror(errno));
        return E_SENSOR;
    }
    *attr = b.attr;
    return 0;
}

int32_t IMP_ISP_SetISPBypass(IMPVI_NUM num, IMPISPOpsMode *enable)
{
    int32_t b[2];
    int fd = OpenIMP_T32_IspFd();

    if (!enable)
        return E_NULL;
    if ((unsigned)num >= T32_VI_MAX)
        return E_VI;
    if (fd >= 0) {
        b[0] = (int32_t)*enable;
        b[1] = (int32_t)num;
        if (ioctl(fd, T32X_IOC_BYPASS_SENSOR, b) != 0) {
            IMP_LOG_ERR(TAG, "BYPASS_SENSOR: %s", strerror(errno));
            return E_SENSOR;
        }
    }
    isp_bypass[num] = (int32_t)*enable;
    return 0;
}

int32_t IMP_ISP_GetISPBypass(IMPVI_NUM num, IMPISPOpsMode *enable)
{
    int32_t rc = ispx_check(num, enable);

    if (rc)
        return rc;
    if (tun.fd < 0)
        return E_STATE;
    *enable = (IMPISPOpsMode)isp_bypass[num];
    return 0;
}

/* vendor tx_isp_dbg_register (T32/T33 SDK include/tx-isp-device.h) */
struct ispx_reg {
    char name[32];
    int32_t sensor_id;
    int32_t type;
    uint32_t size;
    uint32_t pad;
    uint64_t reg;
    uint64_t val;
};
_Static_assert(sizeof(struct ispx_reg) == 64, "tx_isp_dbg_register");

static int32_t ispx_reg(IMPVI_NUM num, IMPISPSensorRegister *reg, int set)
{
    struct ispx_reg b;
    int32_t rc = ispx_check(num, reg);

    if (rc)
        return rc;
    if (OpenIMP_T32_SensorState(num) != 2)
        return E_NULL;          /* vendor: sensor not enabled */
    memset(&b, 0, sizeof(b));
    if (OpenIMP_T32_SensorName(num, b.name) < 0)
        return E_NULL;
    b.sensor_id = (int32_t)num;
    b.type = 1;                 /* TX_SENSOR_CONTROL_INTERFACE_I2C: the only bus the vendor accepts */
    b.reg = reg->addr;
    b.val = set ? reg->value : 0;
    if (ioctl(OpenIMP_T32_IspFd(), set ? T32X_IOC_SET_SENSOR_REGISTER : T32X_IOC_GET_SENSOR_REGISTER,
              &b) != 0) {
        IMP_LOG_ERR(TAG, "%s_REGISTER 0x%x: %s", set ? "SET" : "GET", reg->addr, strerror(errno));
        return E_SENSOR;
    }
    if (!set)
        reg->value = (uint32_t)b.val;
    return 0;
}

int32_t IMP_ISP_SetSensorRegister(IMPVI_NUM num, IMPISPSensorRegister *reg)
{
    return ispx_reg(num, reg, 1);
}

int32_t IMP_ISP_GetSensorRegister(IMPVI_NUM num, IMPISPSensorRegister *reg)
{
    return ispx_reg(num, reg, 0);
}

int32_t IMP_ISP_GetCameraInputMode(IMPISPCameraInputMode *mode)
{
    if (!mode)
        return E_NULL;
    memset(mode, 0, sizeof(*mode));
    return 0;
}

int32_t IMP_ISP_WDR_ENABLE(IMPVI_NUM num, IMPISPTuningOpsMode *mode)
{
    int32_t rc = ispx_check(num, mode);

    if (rc)
        return rc;
    if ((unsigned)*mode > 1)
        return E_FIELD;
    return OpenIMP_T32_BinSetWdr(num, *mode == IMPISP_TUNING_OPS_MODE_ENABLE) < 0 ? E_IOCTL : 0;
}

int32_t IMP_ISP_WDR_ENABLE_GET(IMPVI_NUM num, IMPISPTuningOpsMode *mode)
{
    uint8_t b[T32X_DBIN_SIZE];
    uint32_t w;
    int32_t rc = ispx_check(num, mode);

    if (rc)
        return rc;
    if (tun.fd < 0)
        return E_STATE;
    memset(b, 0, sizeof(b));
    b[T32X_DBIN_VI] = (uint8_t)num;
    if (ioctl(OpenIMP_T32_IspFd(), T32X_IOC_DEFAULT_BIN, b) != 0) {
        IMP_LOG_ERR(TAG, "DEFAULT_BIN (WDR): %s", strerror(errno));
        return E_SENSOR;
    }
    memcpy(&w, b + T32X_DBIN_WDR, sizeof(w));
    *mode = (IMPISPTuningOpsMode)w;
    return 0;
}
