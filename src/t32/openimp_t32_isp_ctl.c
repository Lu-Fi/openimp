/*
 * OpenIMP T32/T33 ISP control calls of the 2.x API (#54 #55 #58 T33, #39 #40 T32; batch Q1 of
 * audit t33backlog/PLAN.md): driver mode, one-shot, power-management mode, LDC mode record,
 * camera input mode, WDR_OPEN, custom AE/AWB records, GPIO switch, AddSensor replay helpers,
 * internal channel attribute, ISP-OSD allocation.
 *
 * Behaviour as vendor libimp 2.0.2.1 (T33) shows it in QEMU -M ingenic-t33 with the same apitest
 * (case t33pass; the vendor library on the vendor module is the reference): return codes, argument
 * order, and the /dev/tx-isp commands listed below (open-tx-isp driver/t32 serves all of them).
 * No vendor code copied.
 *
 *   GET_DriverMode   0xc004543d  value != 0 -> one-shot
 *   OneShot*         0xc004543c  the VALUE 1 enable / 0 disable / 2 tick as argument
 *   LDC_SetMode      0xc00454b8  &mode
 *   PM_Set/GetMode   0xc0045431 / 0xc0045432  {mode, vinum}  (T33: the "t32v" chip path)
 *   PM_WaitAllDone   0xc0045433  {1, 0}
 *   SetCameraInputMode 0xc0045415  the struct, cached for GetCameraInputMode
 *   GPIOSwitchConfig 0xc00454cb  {vinum, cfg[0], cfg[1], 0};  GPIOSwitchTrigger 0xc00454cc {1, vinum}
 *   Get/SetInternalChnAttr 0xc0045430 / 0xc004542f  the attribute block
 *   ISP_OSD_Alloc    reserved memory of len bytes, filled from src, *out = physical address
 *
 * The chip-class rules of the vendor library (get_cpu_id): the T32/T33 ISP has no WDR, so
 * WDR_OPEN after IMP_ISP_Open only logs and answers 0; the T33 takes the "t32v" PM path (ioctl),
 * the T32 the one-shot path (PM mode 0 = one-shot off, 1 = one-shot on + tick).
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <imp/imp_common.h>
#include <imp/imp_isp.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_isp_ctl.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ISPCTL"

#define E_IOCTL  ((int32_t)0xfffff001)
#define E_FIELD  ((int32_t)0xfffff004)
#define E_OPEN   ((int32_t)0xfffff008)
#define E_STATE  ((int32_t)0xfffff009)
#define E_VI     ((int32_t)0xfffff00c)

#define IOC_INTERNAL_SET 0xc004542fu
#define IOC_INTERNAL_GET 0xc0045430u
#define IOC_PM_SET       0xc0045431u
#define IOC_PM_GET       0xc0045432u
#define IOC_PM_WAIT      0xc0045433u
#define IOC_ONESHOT      0xc004543cu
#define IOC_DRIVER_MODE  0xc004543du
#define IOC_CAMERA_INPUT 0xc0045415u
#define IOC_LDC_MODE     0xc00454b8u
#define IOC_GPIO_CONFIG  0xc00454cbu
#define IOC_GPIO_TRIGGER 0xc00454ccu

#define VI_MAX 4

static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static int ldc_mode_rec = 2;                    /* no mode set yet: GetMode refuses */
static IMPISPCameraInputMode cam_input;         /* last mode the driver accepted */
static int cam_input_set;
static struct {
    int ae_on, awb_on;
    IMPISPAeAlgoFunc ae;
    IMPISPAwbAlgoFunc awb;
} algo[VI_MAX];

static int isp_ioctl(uint32_t req, void *arg, const char *name)
{
    int fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return -1;
    if (ioctl(fd, req, arg) != 0) {
        IMP_LOG_ERR(TAG, "%s (0x%08x): %s", name, req, strerror(errno));
        return -1;
    }
    return 0;
}

/* vendor "sensor state >= 3": the sensor of this VI is enabled */
static int sensor_running(int vinum)
{
    return OpenIMP_T32_SensorState(vinum) >= 2;
}

/* ---- driver mode / one-shot ------------------------------------------- */
int32_t IMP_ISP_GET_DriverMode(IMPISPDriverMode *mode)
{
    int32_t v = 0;

    if (OpenIMP_T32_IspFd() < 0)
        return E_STATE;
    if (!mode)
        return E_IOCTL;
    if (isp_ioctl(IOC_DRIVER_MODE, &v, "GET_DRIVER_MODE") != 0)
        return E_IOCTL;
    *mode = v ? IMPISP_DRIVER_ONESHOT_MODE : IMPISP_DRIVER_NORMAL_MODE;
    return 0;
}

static int32_t oneshot(intptr_t what, const char *name)
{
    int fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return E_OPEN;
    /* the value itself is the argument (no pointer), as the vendor passes it */
    if (ioctl(fd, IOC_ONESHOT, (void *)what) != 0) {
        IMP_LOG_ERR(TAG, "%s: %s", name, strerror(errno));
        return E_IOCTL;
    }
    return 0;
}

int32_t IMP_ISP_OneShotEnable(void)  { return oneshot(1, "OneShotEnable"); }
int32_t IMP_ISP_OneShotDisable(void) { return oneshot(0, "OneShotDisable"); }
int32_t IMP_ISP_OneShotTick(void)    { return oneshot(2, "OneShotTick"); }

/* ---- power-management mode -------------------------------------------- */
#if defined(PLATFORM_T33)
static int32_t pm_ioctl(unsigned vinum, uint32_t req, int32_t *mode, const char *name)
{
    int32_t a[2];

    if (vinum >= VI_MAX)
        return E_VI;
    if (!sensor_running((int)vinum))
        return E_STATE;
    a[0] = *mode;
    a[1] = (int32_t)vinum;
    if (isp_ioctl(req, a, name) != 0)
        return E_IOCTL;
    *mode = a[0];
    return 0;
}
#endif

int32_t IMP_ISP_PM_SetMode(IMPVI_NUM num, IMPISPPMAttr *attr)
{
#if defined(PLATFORM_T33)
    int32_t m;

    if (!attr)
        return E_STATE;
    m = (int32_t)*attr;
    return pm_ioctl((unsigned)num, IOC_PM_SET, &m, "PM_SET_MODE");
#else
    IMPISPDriverMode cur;
    int32_t rc;

    (void)num;
    if (!attr)
        return E_STATE;
    rc = IMP_ISP_GET_DriverMode(&cur);
    if (rc)
        return rc;
    if (*attr == TISP_NORMAL_MODE)
        return cur == IMPISP_DRIVER_NORMAL_MODE ? 0 : IMP_ISP_OneShotDisable();
    if (*attr == TISP_AOV_MODE)
        return cur == IMPISP_DRIVER_ONESHOT_MODE ? 0 : IMP_ISP_OneShotEnable() + IMP_ISP_OneShotTick();
    return -0xfff;
#endif
}

int32_t IMP_ISP_PM_GetMode(IMPVI_NUM num, IMPISPPMAttr *attr)
{
#if defined(PLATFORM_T33)
    int32_t m, rc;

    if (!attr)
        return E_STATE;
    m = (int32_t)*attr;
    rc = pm_ioctl((unsigned)num, IOC_PM_GET, &m, "PM_GET_MODE");
    if (rc == 0)
        *attr = (IMPISPPMAttr)m;
    return rc;
#else
    IMPISPDriverMode cur;
    int32_t rc;

    (void)num;
    if (!attr)
        return E_STATE;
    rc = IMP_ISP_GET_DriverMode(&cur);
    if (rc)
        return rc;
    *attr = cur == IMPISP_DRIVER_ONESHOT_MODE ? TISP_AOV_MODE : TISP_NORMAL_MODE;
    return 0;
#endif
}

int32_t IMP_ISP_PM_WaitAllDone(IMPISPOpsMode *enable)
{
    int32_t a[2] = { 1, 0 };

    (void)enable;                       /* the vendor ignores the argument */
    if (!sensor_running(0))
        return E_STATE;
    return isp_ioctl(IOC_PM_WAIT, a, "PM_WAIT_ALL_DONE") ? E_IOCTL : 0;
}

/* ---- LDC mode record (the LDC attribute calls are vendor stubs) -------- */
int32_t IMP_ISP_LDC_SetMode(IMPISPLDCMode mode)
{
    int32_t m = (int32_t)mode;

    if (OpenIMP_T32_IspFd() < 0 || (unsigned)mode >= IMPISP_LDC_MODE_BUTT)
        return E_OPEN;
    pthread_mutex_lock(&ctl_lock);
    ldc_mode_rec = m;
    pthread_mutex_unlock(&ctl_lock);
    return isp_ioctl(IOC_LDC_MODE, &m, "LDC_MODE") ? E_IOCTL : 0;
}

int32_t IMP_ISP_LDC_GetMode(IMPISPLDCMode *mode)
{
    int rec;

    pthread_mutex_lock(&ctl_lock);
    rec = ldc_mode_rec;
    pthread_mutex_unlock(&ctl_lock);
    if (OpenIMP_T32_IspFd() < 0 || !mode || rec >= IMPISP_LDC_MODE_BUTT)
        return E_OPEN;
    *mode = (IMPISPLDCMode)rec;
    return 0;
}

int32_t IMP_ISP_LDC_INIT(IMPVI_NUM num, IMPISPLDCInitAttr *attr)
{
    (void)num; (void)attr;
    IMP_LOG_INFO(TAG, "IMP_ISP_LDC_INIT: deprecated interface (no-op, as the vendor)");
    return 0;
}

int32_t IMP_ISP_LDC_GetAttr(IMPVI_NUM num, IMPISPLDCAttr *attr)
{
    (void)num; (void)attr;
    IMP_LOG_INFO(TAG, "IMP_ISP_LDC_GetAttr: not supported (no-op, as the vendor)");
    return 0;
}

int32_t IMP_ISP_LDC_SetAttr(IMPVI_NUM num, IMPISPLDCAttr *attr)
{
    (void)num; (void)attr;
    IMP_LOG_INFO(TAG, "IMP_ISP_LDC_SetAttr: not supported (no-op, as the vendor)");
    return 0;
}

/* ---- camera input mode / WDR_OPEN -------------------------------------- */
int32_t IMP_ISP_SetCameraInputMode(IMPISPCameraInputMode *mode)
{
    if (OpenIMP_T32_IspFd() < 0 || !mode)
        return E_OPEN;
    if ((unsigned)mode->sensor_num > 4 || (unsigned)mode->input.mode > 1)
        return E_FIELD;
    if (isp_ioctl(IOC_CAMERA_INPUT, mode, "SET_CAMERA_INPUT_MODE") != 0)
        return E_IOCTL;
    pthread_mutex_lock(&ctl_lock);
    cam_input = *mode;
    cam_input_set = 1;
    pthread_mutex_unlock(&ctl_lock);
    return 0;
}

/* GetCameraInputMode (openimp_t32_tuning.c): the cached mode, zeroes before any set */
int OpenIMP_T32_CameraInputMode(IMPISPCameraInputMode *mode)
{
    int set;

    pthread_mutex_lock(&ctl_lock);
    set = cam_input_set;
    if (set)
        *mode = cam_input;
    pthread_mutex_unlock(&ctl_lock);
    return set;
}

int32_t IMP_ISP_WDR_OPEN(IMPVI_NUM num, IMPISPWdrOpenAttr *attr)
{
    (void)num; (void)attr;
    /* vendor: chip class without WDR (T32/T33): log and 0 before any argument check */
    IMP_LOG_INFO(TAG, "IMP_ISP_WDR_OPEN: no WDR on this chip (no-op, as the vendor)");
    return 0;
}

/* ---- custom AE / AWB records ------------------------------------------- */
/* The vendor stores the record and sets the flag; its algorithm thread and the driver side
 * (AE_ALGO 0xc004541d) start at the next EnableSensor.  Stored here the same way; the
 * T32/T33 algorithm thread is not ported yet (custom AE/AWB effect: openimp #55). */
int32_t IMP_ISP_SetAeAlgoFunc(IMPVI_NUM num, IMPISPAeAlgoFunc *ae_func)
{
    if (OpenIMP_T32_IspFd() < 0 || (unsigned)num >= VI_MAX)
        return E_STATE;
    pthread_mutex_lock(&ctl_lock);
    if (!ae_func)
        algo[num].ae_on = 0;
    else {
        algo[num].ae = *ae_func;
        algo[num].ae_on = 1;
    }
    pthread_mutex_unlock(&ctl_lock);
    return 0;
}

int32_t IMP_ISP_SetAwbAlgoFunc(IMPVI_NUM num, IMPISPAwbAlgoFunc *awb_func)
{
    if (OpenIMP_T32_IspFd() < 0 || (unsigned)num >= VI_MAX)
        return E_STATE;
    pthread_mutex_lock(&ctl_lock);
    if (!awb_func)
        algo[num].awb_on = 0;
    else {
        algo[num].awb = *awb_func;
        algo[num].awb_on = 1;
    }
    pthread_mutex_unlock(&ctl_lock);
    return 0;
}

/* ---- GPIO switch -------------------------------------------------------- */
/* (T32 2.2.0 header name; the T33 2.0.2.1 header calls them IMP_ISP_SetGPIOSwitch*, its library
 * exports these names) */
int32_t IMP_ISP_GPIOSwitchConfig(IMPVI_NUM num, IMPISPGPIOSwitchAttr *attr)
{
    uint32_t a[4], vinum = (uint32_t)num;

    if (vinum >= VI_MAX)
        return E_VI;
    if (!sensor_running((int)vinum) || !attr)
        return E_STATE;
    a[0] = vinum;
    a[1] = (uint32_t)attr->gpio;
    a[2] = (uint32_t)attr->state;
    a[3] = 0;
    return isp_ioctl(IOC_GPIO_CONFIG, a, "GPIO_SWITCH_CONFIG") ? E_IOCTL : 0;
}

int32_t IMP_ISP_GPIOSwitchTrigger(IMPVI_NUM num)
{
    uint32_t a[2], vinum = (uint32_t)num;

    if (vinum >= VI_MAX)
        return E_VI;
    if (!sensor_running((int)vinum))
        return E_STATE;
    a[0] = 1;
    a[1] = vinum;
    return isp_ioctl(IOC_GPIO_TRIGGER, a, "GPIO_SWITCH_TRIGGER") ? E_IOCTL : 0;
}

/* ---- internal channel attribute ---------------------------------------- */
static int32_t internal_chn(IMPVI_NUM num, IMPISPInternalChnAttr *attr, uint32_t req, const char *name)
{
    if (OpenIMP_T32_IspFd() < 0)
        return E_OPEN;
    if (!attr)
        return E_STATE;
    if ((unsigned)num >= VI_MAX)
        return E_VI;
    /* the attribute block itself is the payload (the vendor passes the caller's pointer) */
    return isp_ioctl(req, attr, name) ? E_IOCTL : 0;
}

int32_t IMP_ISP_SetInternalChnAttr(IMPVI_NUM num, IMPISPInternalChnAttr *attr)
{
    return internal_chn(num, attr, IOC_INTERNAL_SET, "SET_INTERNAL_CHN_ATTR");
}

int32_t IMP_ISP_GetInternalChnAttr(IMPVI_NUM num, IMPISPInternalChnAttr *attr)
{
    return internal_chn(num, attr, IOC_INTERNAL_GET, "GET_INTERNAL_CHN_ATTR");
}

/* ---- AddSensor helpers -------------------------------------------------- */
/* The vendor keeps per-VI requests made before AddSensor (bin paths, ISP bypass, WDR enable,
 * night mode, LDC init) and replays them here; this library applies those requests at once,
 * so nothing is pending: the replay returns 0 and the free is empty, as the vendor without
 * pending requests. */
void IMP_ISP_AddSensor_Free(int vinum)
{
    (void)vinum;
}

int IMP_ISP_AddSensor_Process(int vinum)
{
    IMP_ISP_AddSensor_Free(vinum);
    return 0;
}

/* ---- ISP-OSD buffer ----------------------------------------------------- */
/* vendor: allocate len bytes of reserved memory, copy len bytes of src into them, *out = the
 * physical address; 0 whatever the allocation did (no check) */
int IMP_ISP_OSD_Alloc(void **out, const void *src, int len)
{
    IMPDMABufferInfo info;

    memset(&info, 0, sizeof(info));
    if (len <= 0 || DMA_AllocDescriptor(&info, len, "isp_osd") != 0 || !info.virt_addr) {
        IMP_LOG_ERR(TAG, "IMP_ISP_OSD_Alloc: %d bytes: no memory", len);
        return 0;
    }
    if (src)
        memcpy((void *)(uintptr_t)info.virt_addr, src, (size_t)len);
    if (out)
        *out = (void *)(uintptr_t)info.phys_addr;
    return 0;
}
