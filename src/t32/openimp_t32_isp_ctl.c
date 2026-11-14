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
 * and registers /dev/offline_ldc as the vendor module (ioctls 0 / 1 LUT memory, 2 one job), so
 * OfflineInit(1) opens the node.  Not as the vendor yet: the online state is kept but the requests
 * are not issued at AddSensor (LUT fill / generation not ported), and the job protocol behind the
 * open node is not implemented (f001). */

static struct {
    int init, func, size, angle;
    int type;                       /* 0 none, 1 LUT file, 2 strength */
    double strength;
    char lut[128];
} ldc_on[VI_MAX];

static struct {
    int fd;
    int jobs;
} ldc_off = { -1, 0 };

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

int32_t IMP_ISP_LDC_OfflineInit(int mode)
{
    int32_t rc = 0;

    pthread_mutex_lock(&ctl_lock);
    if (ldc_mode_rec != 1) {
        rc = E_STATE;
    } else if (mode == 1) {
        if (ldc_off.fd < 0) {
            ldc_off.fd = open("/dev/offline_ldc", O_RDWR);
            if (ldc_off.fd < 0) {
                IMP_LOG_ERR(TAG, "/dev/offline_ldc cannot open: %s", strerror(errno));
                rc = E_OPEN;
            }
        }
    } else if (mode == 0) {
        if (ldc_off.jobs) {
            rc = E_STATE;
        } else if (ldc_off.fd >= 0) {
            close(ldc_off.fd);
            ldc_off.fd = -1;
        }
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

/* job calls: the node is never open with the open module (see above) */
static int32_t ldc_off_job(const void *attr, int check_mode, const char *name)
{
    int32_t rc;

    pthread_mutex_lock(&ctl_lock);
    if (check_mode && ldc_mode_rec != 1)
        rc = E_STATE;
    else if (ldc_off.fd < 0)
        rc = E_OPEN;
    else if (!attr)
        rc = E_FIELD;
    else {
        IMP_LOG_ERR(TAG, "%s: offline LDC job protocol not implemented", name);
        rc = E_IOCTL;
    }
    pthread_mutex_unlock(&ctl_lock);
    return rc;
}

int32_t IMP_ISP_LDC_OfflineCreateJob(void *attr)
{
    return ldc_off_job(attr, 1, "LDC_OfflineCreateJob");
}

int32_t IMP_ISP_LDC_OfflineDestroyJob(uint32_t job)
{
    int32_t rc;

    pthread_mutex_lock(&ctl_lock);
    rc = ldc_off.fd < 0 ? E_OPEN : E_FIELD;     /* no job can exist */
    pthread_mutex_unlock(&ctl_lock);
    (void)job;
    return rc;
}

int32_t IMP_ISP_LDC_OfflineSubmitTask(void *task)
{
    return ldc_off_job(task, 0, "LDC_OfflineSubmitTask");
}

int32_t IMP_ISP_LDC_OfflineUpdateLutToJob(void *attr)
{
    return ldc_off_job(attr, 0, "LDC_OfflineUpdateLutToJob");
}
