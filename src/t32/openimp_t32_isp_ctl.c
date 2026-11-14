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
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* ---- SnapRaw (batch F1; exported by the vendor 2.x libimp, not in its headers) ----------------
 * IMP_FrameSource_SnapRaw_Start(attr) / _End(): RAW frames of the sensor into reserved memory.
 * The vendor flow (T33 2.0.2.1 disassembly, no code copied): ISP not open f008, NULL -1;
 * sensor attribute (GetSensorAttr; -1, picture > 4096: -1); line length per RAW type rounded up
 * to 8 bytes (0: w, 1 and 3: 2w, 2: 3w/2, other: 2w), frame size = line x h (x2 for mode 2),
 * count frames must fit the buffer (-1); every registered encoder channel stops receiving
 * (restarted by _End); 2 frame times; per sensor the module-control bit 11 is set (cleared again
 * by _End where Start set it); /dev/tx-isp 0xc004548f {vinum, 0}, then 0xc004548e with
 * {vinum, count, type, mode, frame size, paddr, vaddr}; on success attr gets count, frame size,
 * paddr and vaddr.  _End: 0xc004548f {vinum, 1}, module bits back, encoders restart.
 * Not as the vendor: the vendor takes the buffer from its reuse block (IMP_Alloc_get_reuse_block);
 * OpenIMP allocates reserved memory of the requested size (kept until the next Start). */
typedef struct {
    uint32_t vinum, count, type, mode, size, paddr, vaddr;
} OpenIMPSnapRawAttr;

#define IOC_SNAPRAW      0xc004548eu
#define IOC_SNAPRAW_CTL  0xc004548fu
#define SNAP_MAX_SENSORS 4
#define SNAP_ENC_CHNS    16

extern int IMP_Encoder_Query(int encChn, void *stat) __attribute__((weak));
extern int IMP_Encoder_StopRecvPic(int encChn) __attribute__((weak));
extern int IMP_Encoder_StartRecvPic(int encChn) __attribute__((weak));

static struct {
    uint32_t vinum;
    int enc_stopped[SNAP_ENC_CHNS];
    int mod_set[SNAP_MAX_SENSORS];
    int nsensors;
    IMPDMABufferInfo buf;
} snap;

static int snap_sensors(void)
{
    int n = cam_input_set ? (int)cam_input.sensor_num : 0;

    if (n <= 0)
        n = 1;
    return n > SNAP_MAX_SENSORS ? SNAP_MAX_SENSORS : n;
}

int IMP_FrameSource_SnapRaw_Start(OpenIMPSnapRawAttr *attr)
{
    IMPISPSENSORAttr sa;
    uint32_t line, fsize, total, fnum = 25, fden = 1, wait_us;
    uint32_t arg[2], req[7];
    struct { uint8_t registered; uint8_t pad[23]; } st;
    int i, fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return E_OPEN;
    if (!attr)
        return -1;
    memset(&snap.enc_stopped, 0, sizeof(snap.enc_stopped));
    memset(&snap.mod_set, 0, sizeof(snap.mod_set));
    snap.vinum = attr->vinum;
    memset(&sa, 0, sizeof(sa));
    if (IMP_ISP_Tuning_GetSensorAttr((IMPVI_NUM)attr->vinum, &sa) < 0)
        return -1;
    if (sa.width > 0x1000 || sa.height > 0x1000) {
        IMP_LOG_ERR(TAG, "SnapRaw: sensor %ux%u too large", (unsigned)sa.width, (unsigned)sa.height);
        return -1;
    }
    switch (attr->type) {
    case 0:  line = sa.width; break;
    case 2:  line = sa.width * 3 / 2; break;
    default: line = sa.width * 2; break;
    }
    line = (line + 7) & ~7u;
    fsize = line * sa.height * (attr->mode == 2 ? 2 : 1);
    total = fsize * attr->count;
    if (!total)
        return -1;
    if (snap.buf.phys_addr) {
        DMA_FreePhys(snap.buf.phys_addr);
        memset(&snap.buf, 0, sizeof(snap.buf));
    }
    if (DMA_AllocDescriptor(&snap.buf, (int)total, "snapraw") != 0 || !snap.buf.phys_addr) {
        IMP_LOG_ERR(TAG, "SnapRaw: %u bytes of reserved memory not available", total);
        memset(&snap.buf, 0, sizeof(snap.buf));
        return -1;
    }
    /* encoders stop receiving while the ISP delivers RAW */
    for (i = 0; IMP_Encoder_Query && i < SNAP_ENC_CHNS; i++) {
        memset(&st, 0, sizeof(st));
        if (IMP_Encoder_Query(i, &st) == 0 && st.registered) {
            int r;

            snap.enc_stopped[i] = 1;
            r = IMP_Encoder_StopRecvPic ? IMP_Encoder_StopRecvPic(i) : 0;
            if (r < 0)
                return r;
        }
    }
    {
        IMPISPSensorFps f = { 25, 1 };

        if (IMP_ISP_Tuning_GetSensorFPS((IMPVI_NUM)attr->vinum, &f) == 0 && f.num && f.den) {
            fnum = f.num;
            fden = f.den;
        }
    }
    wait_us = (uint32_t)(2000000ull * fden / fnum);
    usleep(wait_us);
    snap.nsensors = snap_sensors();
    for (i = 0; i < snap.nsensors; i++) {
        IMPISPModuleCtl m;

        memset(&m, 0, sizeof(m));
        if (IMP_ISP_Tuning_GetModuleControl((IMPVI_NUM)i, &m) != 0)
            return -1;
        if (!(m.key & 0x800)) {
            m.key |= 0x800;
            snap.mod_set[i] = 1;
            if (IMP_ISP_Tuning_SetModuleControl((IMPVI_NUM)i, &m) != 0)
                return -1;
        }
    }
    usleep(wait_us);
    arg[0] = attr->vinum;
    arg[1] = 0;
    if (ioctl(fd, IOC_SNAPRAW_CTL, arg) < 0) {
        IMP_LOG_ERR(TAG, "SnapRaw: 0x%08x: %s", IOC_SNAPRAW_CTL, strerror(errno));
        return -1;
    }
    req[0] = attr->vinum;
    req[1] = attr->count;
    req[2] = attr->type;
    req[3] = attr->mode;
    req[4] = fsize;
    req[5] = snap.buf.phys_addr;
    req[6] = snap.buf.virt_addr;
    if (ioctl(fd, IOC_SNAPRAW, req) < 0) {
        IMP_LOG_ERR(TAG, "SnapRaw: 0x%08x: %s", IOC_SNAPRAW, strerror(errno));
        return -1;
    }
    attr->count = req[1];
    attr->size = req[4];
    attr->paddr = req[5];
    attr->vaddr = req[6];
    return 0;
}

int IMP_FrameSource_SnapRaw_End(void)
{
    uint32_t arg[2];
    int i, r = 0, fd = OpenIMP_T32_IspFd();

    if (fd < 0)
        return E_OPEN;
    arg[0] = snap.vinum;
    arg[1] = 1;
    if (ioctl(fd, IOC_SNAPRAW_CTL, arg) < 0) {
        IMP_LOG_ERR(TAG, "SnapRaw_End: 0x%08x: %s", IOC_SNAPRAW_CTL, strerror(errno));
        return -1;
    }
    for (i = 0; i < snap.nsensors && i < SNAP_MAX_SENSORS; i++) {
        IMPISPModuleCtl m;

        if (!snap.mod_set[i])
            continue;
        memset(&m, 0, sizeof(m));
        if (IMP_ISP_Tuning_GetModuleControl((IMPVI_NUM)i, &m) != 0)
            return -1;
        m.key &= ~0x800u;
        if (IMP_ISP_Tuning_SetModuleControl((IMPVI_NUM)i, &m) != 0)
            return -1;
        snap.mod_set[i] = 0;
    }
    for (i = 0; i < SNAP_ENC_CHNS; i++)
        if (snap.enc_stopped[i]) {
            r = IMP_Encoder_StartRecvPic ? IMP_Encoder_StartRecvPic(i) : 0;
            if (r < 0)
                return -1;
            snap.enc_stopped[i] = 0;
        }
    return r;
}

/* ---- IMP_DQbuffer (exported debug helper of the vendor 2.x libimp, not in its headers) ---------
 * (path, request, dump): /dev/tisp (256 tries, 10 ms apart, else -1), the request word written
 * to it, then the request itself on a 96-byte block; when the answer names a buffer (word 13
 * physical address, word 14 length) and dump != 0, that memory (/dev/mem) is written to path.
 * 0, or -1 on any failure (as the vendor). */
int IMP_DQbuffer(const char *path, uint32_t req, int dump)
{
    uint32_t blk[24];
    int fd = -1, i, mem, out, rc = -1;
    void *p;

    for (i = 0; i < 256 && fd < 0; i++) {
        fd = open("/dev/tisp", O_RDWR);
        if (fd < 0)
            usleep(10000);
    }
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "IMP_DQbuffer: Can not open /dev/tisp: %s", strerror(errno));
        return -1;
    }
    if (write(fd, &req, 4) != 4)
        goto done;
    memset(blk, 0, sizeof(blk));
    if (ioctl(fd, req, blk) < 0)
        goto done;
    rc = 0;
    if (!blk[14] || !dump || !path)
        goto done;
    rc = -1;
    mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0)
        goto done;
    p = mmap(NULL, blk[14], PROT_READ | PROT_WRITE, MAP_SHARED, mem, (off_t)blk[13]);
    if (p != MAP_FAILED) {
        out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
        if (out >= 0) {
            if (write(out, p, blk[14]) == (ssize_t)blk[14])
                rc = 0;
            close(out);
        }
        munmap(p, blk[14]);
    }
    close(mem);
done:
    close(fd);
    return rc;
}

/* ---- LDC online / offline (batch L1, #54 T33 / #39 T32) ----------------------------------------
 * Vendor 2.0.2.1 (disassembly, no code copied).  Online: library state per sensor, consumed by
 * IMP_ISP_AddSensor (vendor: /dev/tx-isp 0xc00454b9 with the state block, the LUT memory mapped
 * and filled from the LUT file or generated (ldc_param_to_lut / gen_undistort_lut), 0xc00454bb,
 * 0xc00454ba).  Checks in vendor order:
 *   OnlineInit(vinum, attr)     ISP not open f008; sensor already added f009; NULL f004; LDC mode
 *                               record not 0 (online, IMP_ISP_LDC_SetMode) f009; func 1 (I2D) or
 *                               2 (HLDC) or 4 (LDC), else f004; HLDC / LDC keep the block size,
 *                               I2D the angle
 *   OnlineSetStrength(vinum, d) f008; sensor added or mode record not 0 f009; a LUT file already
 *                               set f004; keeps the strength
 *   OnlineGetStrength(vinum, *d) f008; mode record not 0 f009; the strength when one was set; 0
 *   OnlineSetLut(vinum, name)   f008; sensor added / mode f009; a strength already set f004; keeps
 *                               the 128-byte file name
 *   OnlineGetLut(vinum, name)   f008; mode f009; the name when one was set; 0
 * Offline (/dev/offline_ldc of the vendor tx-isp-t33.ko): OfflineInit(1) needs mode record 1
 * (f009), opens the node (f008 when it cannot); OfflineInit(0) closes it when no job is left;
 * CreateJob f009 (mode) / f008 (not initialised) / f004 (NULL); DestroyJob, SubmitTask,
 * UpdateLutToJob f008 (not initialised) / f004 (NULL or unknown job).
 * Open module (open-tx-isp, #491): the T33 module answers the online LUT requests 0xc00454b9..bb
 * and registers /dev/offline_ldc as the vendor module.  IMP_ISP_AddSensor sends the online state
 * (OpenIMP_T32_LdcAddSensor below) and the offline job protocol follows the vendor (below). */

static struct {
    int init, func, size, angle;
    int type;                       /* 0 none, 1 LUT file, 2 strength */
    double strength;
    char lut[128];
} ldc_on[VI_MAX];

static int ldc_on_check(int vinum, int need_unadded)
{
    if (OpenIMP_T32_IspFd() < 0)
        return E_OPEN;
    if (vinum < 0 || vinum >= VI_MAX)
        return E_STATE;
    if (need_unadded && OpenIMP_T32_SensorState(vinum) >= 1)
        return E_STATE;
    return 0;
}

int32_t IMP_ISP_LDC_OnlineInit(IMPVI_NUM num, IMPISPLDCInitOnlineAttr *attr)
{
    int32_t rc = ldc_on_check((int)num, 1);

    if (rc)
        return rc;
    if (!attr)
        return E_FIELD;
    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 0) {
        pthread_mutex_unlock(&ctl_lock);
        IMP_LOG_ERR(TAG, "LDC_OnlineInit: LDC mode is not online (IMP_ISP_LDC_SetMode)");
        return E_STATE;
    }
    if (attr->func != IMPISP_I2D_MODE && attr->func != IMPISP_HLDC_MODE && attr->func != IMPISP_LDC_MODE) {
        pthread_mutex_unlock(&ctl_lock);
        IMP_LOG_ERR(TAG, "LDC_OnlineInit: func %d (I2D 1, HLDC 2, LDC 4)", (int)attr->func);
        return E_FIELD;
    }
    ldc_on[num].init = 1;
    ldc_on[num].func = (int)attr->func;
    if (attr->func != IMPISP_I2D_MODE)
        ldc_on[num].size = (int)attr->size;
    else
        ldc_on[num].angle = (int)attr->angle;
    pthread_mutex_unlock(&ctl_lock);
    return 0;
}

int32_t IMP_ISP_LDC_OnlineSetStrength(IMPVI_NUM num, double strength)
{
    int32_t rc = ldc_on_check((int)num, 1);

    if (rc)
        return rc;
    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 0)
        rc = E_STATE;
    else if (ldc_on[num].type == 1)
        rc = E_FIELD;               /* a LUT file is set: the two exclude each other */
    else {
        ldc_on[num].strength = strength;
        ldc_on[num].type = 2;
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OnlineGetStrength(IMPVI_NUM num, double *strength)
{
    int32_t rc = ldc_on_check((int)num, 0);

    if (rc)
        return rc;
    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 0)
        rc = E_STATE;
    else if (ldc_on[num].type == 2 && strength)
        *strength = ldc_on[num].strength;
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OnlineSetLut(IMPVI_NUM num, char lut_name[128])
{
    int32_t rc = ldc_on_check((int)num, 1);

    if (rc)
        return rc;
    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 0)
        rc = E_STATE;
    else if (ldc_on[num].type == 2)
        rc = E_FIELD;               /* a strength is set */
    else if (!lut_name)
        rc = E_FIELD;
    else {
        memcpy(ldc_on[num].lut, lut_name, sizeof(ldc_on[num].lut));
        ldc_on[num].type = 1;
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OnlineGetLut(IMPVI_NUM num, char lut_name[128])
{
    int32_t rc = ldc_on_check((int)num, 0);

    if (rc)
        return rc;
    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 0)
        rc = E_STATE;
    else if (ldc_on[num].type == 1 && lut_name)
        memcpy(lut_name, ldc_on[num].lut, sizeof(ldc_on[num].lut));
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

/* ---- online LDC at IMP_ISP_AddSensor (#491) ------------------------------------------------------
 * Vendor 2.0.2.1 IMP_ISP_AddSensor after S_INPUT (disassembly, no code copied): for a VI with an
 * online LDC state (OnlineInit) the 48-byte state block goes to /dev/tx-isp 0xc00454b9 (the driver
 * answers the LUT memory: word 9 size, word 10 physical address); for HLDC / LDC the LUT is filled
 * through /dev/mem: from the LUT file (OnlineSetLut) or, for a strength (OnlineSetStrength), by
 * gen_undistort_lut - which in 2.0.2.1 writes the identity mesh (every node its own position, Q4;
 * the strength is not used) after a 0x80-byte header {size, crc, "T33", 0, .., "20251202"}; the
 * vendor first asks 0xc00454bb with a NULL block (EFAULT online -> gen_undistort_lut).  Then
 * 0xc00454ba with the block (word 7 + word 8 = the LUT past its header).  Errors are logged; the
 * sensor is added anyway (vendor).  The header CRC is a plain CRC-32 here (the vendor's ldc_crc32
 * runs over the buffer before it is filled, so its value carries no information). */
#define LDC_LUT_HDR 0x80u

static uint32_t ldc_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu, k;

    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xedb88320u & -(c & 1u));
    }
    return ~c;
}

static void *ldc_map(uint32_t pa, uint32_t len, int *fd, void **base, uint32_t *mlen)
{
    uint32_t off = pa & 0xfffu;

    *fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (*fd < 0)
        return NULL;
    *mlen = (len + off + 0xfffu) & ~0xfffu;
    *base = mmap(NULL, *mlen, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, (off_t)(pa - off));
    if (*base == MAP_FAILED) {
        close(*fd);
        return NULL;
    }
    return (uint8_t *)*base + off;
}

static void ldc_unmap(int fd, void *base, uint32_t mlen)
{
    munmap(base, mlen);
    close(fd);
}

/* FUN_0010e750 / the AddSensor file branch: the LUT file into the LUT memory */
static int32_t ldc_lut_from_file(const char *name, uint32_t pa, uint32_t size)
{
    void *base;
    uint32_t mlen;
    uint8_t *lut;
    FILE *f;
    int fd;

    if (!(lut = ldc_map(pa, size, &fd, &base, &mlen)))
        return E_OPEN;
    f = fopen(name, "rb");
    if (!f) {
        IMP_LOG_ERR(TAG, "LDC LUT %s: %s", name, strerror(errno));
        ldc_unmap(fd, base, mlen);
        return E_OPEN;
    }
    if (fread(lut, 1, size, f) == 0)
        IMP_LOG_WARN(TAG, "LDC LUT %s: empty", name);
    fclose(f);
    ldc_unmap(fd, base, mlen);
    return 0;
}

/* gen_undistort_lut (2.0.2.1): identity mesh of a w x h picture, block bs */
static void ldc_identity_lut(uint8_t *lut, uint32_t w, uint32_t h, uint32_t bs)
{
    uint32_t cols = ((w + bs - 1) / bs + 2) & 0xfffeu, rows = (h + bs - 1) / bs + 1;
    uint32_t total = cols * rows * 4 + LDC_LUT_HDR, x, y;
    uint32_t *hdr = (uint32_t *)lut, *node = (uint32_t *)(lut + LDC_LUT_HDR);

    memset(lut, 0, total);
    for (y = 0; y < h; y++) {
        int yb = y % bs == 0, ylast = (h - 1) % bs && y == h - 1;

        for (x = 0; x < w; x++) {
            int xb = x % bs == 0, xlast = (w - 1) % bs && x == w - 1;
            uint32_t v = (y * 16u) << 16 | (x * 16u);

            if (xb && yb)
                node[(y / bs) * cols + x / bs] = v;
            if (ylast && xb)
                node[(rows - 1) * cols + x / bs] = v;
            if (xlast && yb)
                node[(y / bs) * cols + cols - 1] = v;
            if (xlast && ylast)
                node[(rows - 1) * cols + cols - 1] = v;
        }
    }
    hdr[0] = total;
    hdr[2] = 0x333354;          /* "T33" */
    memcpy(lut + 0x6c, "20251202", 8);
    hdr[1] = ldc_crc32(lut + 0x10, total - 0x10);
}

int OpenIMP_T32_LdcAddSensor(int fd, int vinum, uint32_t w, uint32_t h)
{
    uint32_t b[12];
    void *base;
    uint32_t mlen;
    uint8_t *lut;
    int mfd, rc = 0;

    if (vinum < 0 || vinum >= VI_MAX)
        return 0;
    pthread_mutex_lock(&ctl_lock);
    if (!ldc_on[vinum].init || ldc_mode_rec != 0) {
        pthread_mutex_unlock(&ctl_lock);
        return 0;
    }
    memset(b, 0, sizeof(b));
    b[0] = (uint32_t)vinum;
    b[1] = (uint32_t)ldc_on[vinum].func;
    b[2] = ldc_on[vinum].func == IMPISP_I2D_MODE ? 0 : (uint32_t)ldc_on[vinum].size;
    b[3] = 1;
    b[4] = ldc_on[vinum].func == IMPISP_I2D_MODE ? (uint32_t)ldc_on[vinum].angle : 0;
    b[5] = h << 16 | w;
    b[6] = (uint32_t)ldc_on[vinum].type;
    b[8] = ldc_on[vinum].func == IMPISP_I2D_MODE ? 0 : LDC_LUT_HDR;
    if (ioctl(fd, 0xc00454b9u, b) != 0) {
        IMP_LOG_ERR(TAG, "LDC online VI%d: 0xc00454b9: %s", vinum, strerror(errno));
        pthread_mutex_unlock(&ctl_lock);
        return -1;
    }
    b[7] = b[10];
    if (ldc_on[vinum].func != IMPISP_I2D_MODE) {
        if (!b[10] || !b[9]) {
            IMP_LOG_ERR(TAG, "LDC online VI%d: no LUT memory from the driver", vinum);
            rc = -1;
        } else if (ldc_on[vinum].type == 1) {
            rc = ldc_lut_from_file(ldc_on[vinum].lut, b[10], b[9]);
        } else if ((lut = ldc_map(b[10], b[9], &mfd, &base, &mlen))) {
            ioctl(fd, 0xc00454bbu, NULL);       /* vendor: IQ-bin parameters; online: EFAULT */
            ldc_identity_lut(lut, w, h, b[2] ? b[2] : 16);
            ldc_unmap(mfd, base, mlen);
        } else {
            rc = -1;
        }
    }
    if (!rc && ioctl(fd, 0xc00454bau, b) != 0) {
        IMP_LOG_ERR(TAG, "LDC online VI%d: 0xc00454ba: %s", vinum, strerror(errno));
        rc = -1;
    }
    pthread_mutex_unlock(&ctl_lock);
    IMP_LOG_INFO(TAG, "LDC online VI%d func %d %ux%u: LUT %u B at 0x%x (%s)", vinum, ldc_on[vinum].func, w, h,
                 b[9], b[10], ldc_on[vinum].type == 1 ? "file" : "identity mesh");
    return rc;
}

/* ---- offline LDC jobs (#491) ---------------------------------------------------------------------
 * Vendor 2.0.2.1 (disassembly, no code copied).  OfflineInit(1): mode record 1 (f009), the device
 * block and /dev/offline_ldc (f008); OfflineInit(0): closes it when no job was ever created (f009 when
 * one was; the vendor then destroys the mutex of the freed block and crashes, here not).
 * CreateJob(attr {func mask, u16 w, u16 h, block size, .., job handle out at word 36}): f009 mode,
 * f008 not initialised, f004 NULL; the creation count goes up, the first free of 12 job slots takes
 * w, h, the func mask and the block size and its handle goes to attr word 36 and to every free entry
 * of the handle list.  The vendor then still returns f004 (also on success; kept: callers of the
 * vendor library see the same code and use the handle).  No LUT memory is allocated (as the vendor).
 * DestroyJob(handle): f008 / f004 unknown; the job slot is freed and its LUT memory block given to
 * ioctl 1 (f001 when that fails).  SubmitTask(task {handle, Y in, UV in, Y out, UV out, strides
 * in | out << 16, angle, func}): f008 / f004; func = task func & job mask: 1 (I2D) with the angle,
 * 4 (LDC) with the job's LUT and block size, else f00a; ioctl 2 runs the job and waits for it (f001).
 * UpdateLutToJob({handle, 0 params / 1 file, ..}): f008 / f004; the job must have func bit 2 (f004).
 * Not as the vendor: a job without LUT memory (every job: CreateJob allocates none) is refused with
 * f001 instead of mapping physical address 0 through /dev/mem; params mode (ldc_param_to_lut) is not
 * ported (f001). */
#define E_FUNC   ((int32_t)0xfffff00a)
#define LDC_JOBS 12

struct ldc_job {
    uint16_t used, busy;
    uint32_t self;
    uint32_t lut_off;
    uint32_t rsvd;
    uint16_t w, h;              /* +0x10 */
    uint32_t lut_pa;            /* +0x14 */
    uint32_t lut_size;          /* +0x18 */
    uint32_t func;              /* +0x1c */
    uint32_t block;             /* +0x20 */
};

static struct {
    int open;
    uint8_t created;
    int fd;
    pthread_mutex_t lock;
    struct ldc_job job[LDC_JOBS];
    uint32_t list[LDC_JOBS];
} ldc_off = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

static struct ldc_job *ldc_off_find(uint32_t h)
{
    int i;

    for (i = 0; h && i < LDC_JOBS; i++)
        if (ldc_off.list[i] == h)
            return (struct ldc_job *)(uintptr_t)h;
    return NULL;
}

int32_t IMP_ISP_LDC_OfflineInit(int mode)
{
    int32_t rc = 0;

    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 1) {
        rc = E_STATE;
    } else if (mode == 1) {
        if (!ldc_off.open) {
            memset(ldc_off.job, 0, sizeof(ldc_off.job));
            memset(ldc_off.list, 0, sizeof(ldc_off.list));
            ldc_off.created = 0;
            ldc_off.fd = open("/dev/offline_ldc", O_RDWR | O_CLOEXEC);
            if (ldc_off.fd < 0) {
                IMP_LOG_ERR(TAG, "/dev/offline_ldc cannot open: %s", strerror(errno));
                rc = E_OPEN;
            } else {
                ldc_off.open = 1;
            }
        }
    } else if (mode == 0 && ldc_off.open) {
        if (ldc_off.created) {
            IMP_LOG_ERR(TAG, "LDC_OfflineInit(0): %u job(s) created", ldc_off.created);
            rc = E_STATE;
        } else {
            close(ldc_off.fd);
            ldc_off.fd = -1;
            ldc_off.open = 0;
        }
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OfflineCreateJob(void *attr)
{
    uint32_t *a = attr;
    struct ldc_job *j;
    int i;

    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 1) {
        pthread_mutex_unlock(&ctl_lock);
        return E_STATE;
    }
    if (!ldc_off.open) {
        pthread_mutex_unlock(&ctl_lock);
        return E_OPEN;
    }
    if (!a) {
        pthread_mutex_unlock(&ctl_lock);
        return E_FIELD;
    }
    ldc_off.created++;
    for (i = 0; i < LDC_JOBS && ldc_off.job[i].used; i++)
        ;
    if (i == LDC_JOBS) {
        pthread_mutex_unlock(&ctl_lock);
        IMP_LOG_ERR(TAG, "LDC_OfflineCreateJob: no free job");
        return E_FIELD;
    }
    j = &ldc_off.job[i];
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->self = (uint32_t)(uintptr_t)j;
    j->w = (uint16_t)a[1];
    j->h = (uint16_t)(a[1] >> 16);
    j->func = a[0];
    j->block = a[2];
    a[36] = j->self;
    for (i = 0; i < LDC_JOBS; i++)
        if (!ldc_off.list[i])
            ldc_off.list[i] = j->self;
    pthread_mutex_unlock(&ctl_lock);
    IMP_LOG_INFO(TAG, "LDC_OfflineCreateJob: job 0x%x %ux%u func %u", j->self, j->w, j->h, j->func);
    return E_FIELD;             /* vendor 2.0.2.1 returns 0xfffff004 after creating the job */
}

int32_t IMP_ISP_LDC_OfflineDestroyJob(uint32_t job)
{
    struct ldc_job *j;
    uint32_t m[5];
    int32_t rc = 0;

    pthread_mutex_lock(&ctl_lock);
    if (!ldc_off.open) {
        rc = E_OPEN;
    } else if (!(j = ldc_off_find(job))) {
        IMP_LOG_ERR(TAG, "LDC_OfflineDestroyJob: unknown job 0x%x", job);
        rc = E_FIELD;
    } else {
        j->used = 0;
        m[0] = (uint32_t)j->h << 16 | j->w;
        m[1] = j->lut_pa;
        m[2] = j->lut_size;
        m[3] = j->func;
        m[4] = j->block;
        if (ioctl(ldc_off.fd, 1, m) == -1)
            rc = E_IOCTL;
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OfflineSubmitTask(void *task)
{
    const uint32_t *t = task;
    struct ldc_job *j;
    uint32_t b[10];
    int fd;

    pthread_mutex_lock(&ctl_lock);
    if (!ldc_off.open) {
        pthread_mutex_unlock(&ctl_lock);
        return E_OPEN;
    }
    if (!t || !(j = ldc_off_find(t[0]))) {
        pthread_mutex_unlock(&ctl_lock);
        return E_FIELD;
    }
    memset(b, 0, sizeof(b));
    b[0] = (uint32_t)j->h << 16 | j->w;
    b[1] = t[1];
    b[2] = t[2];
    b[3] = t[3];
    b[4] = t[4];
    b[5] = t[5];
    b[7] = t[7] & j->func;
    if (b[7] == 1) {
        b[9] = t[6];
    } else if (b[7] == 4) {
        b[8] = j->block;
        b[6] = j->lut_pa + j->lut_off;
    } else {
        pthread_mutex_unlock(&ctl_lock);
        IMP_LOG_ERR(TAG, "LDC_OfflineSubmitTask: func %u of job mask %u", t[7], j->func);
        return E_FUNC;
    }
    j->busy++;
    fd = ldc_off.fd;
    pthread_mutex_unlock(&ctl_lock);
    pthread_mutex_lock(&ldc_off.lock);
    if (ioctl(fd, 2, b) == -1) {
        pthread_mutex_unlock(&ldc_off.lock);
        IMP_LOG_ERR(TAG, "LDC_OfflineSubmitTask: %s", strerror(errno));
        return E_IOCTL;
    }
    pthread_mutex_unlock(&ldc_off.lock);
    pthread_mutex_lock(&ctl_lock);
    j->busy--;
    pthread_mutex_unlock(&ctl_lock);
    return 0;
}

int32_t IMP_ISP_LDC_OfflineUpdateLutToJob(void *attr)
{
    const uint32_t *p = attr;
    struct ldc_job *j;
    int32_t rc;

    pthread_mutex_lock(&ctl_lock);
    if (!ldc_off.open)
        rc = E_OPEN;
    else if (!p || !(j = ldc_off_find(p[0])) || !(j->func & 4) || p[1] > 1)
        rc = E_FIELD;
    else if (!j->lut_pa || p[1] == 0) {
        IMP_LOG_ERR(TAG, "LDC_OfflineUpdateLutToJob: %s", p[1] == 0 ? "LUT from parameters not ported"
                    : "the job has no LUT memory (vendor 2.0.2.1 CreateJob allocates none)");
        rc = E_IOCTL;
    } else {
        rc = ldc_lut_from_file((const char *)&p[2], j->lut_pa, j->lut_size);
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}
