/*
 * T41 IMP_ISP_SetVicDoneCbFunc and IMP_ISP_RAW_RwControl (openimp #320),
 * rebuilt from the vendor libimp T41 1.2.5 (isp_tseries.c.o):
 *
 * SetVicDoneCbFunc(cb)  0x4fcc: tuning not enabled or already on -4091;
 *     ISP-device ioctl 0xc0045431 (int 1) fails -4090; cb stored; worker
 *     thread start fails -4087; 0.
 *   worker (impisp_vic_done_cb_thread 0x59c): loop { cancel point;
 *     v = -1; ioctl 0xc0045432 (&v) blocks until the next VIC frame done;
 *     ioctl failure ends the thread; v != 0 -> cb() }.  One call per VIC
 *     frame, always from this thread (never from interrupt context).
 *   close (IMP_ISP_SetVicDoneCbFunc_close 0x5404, from DisableSensor):
 *     cancel + join, ioctl 0xc0045431 (int 0), off.
 *
 * RAW_RwControl(num, attr)  0x1dbc4: ISP not open -4088, attr NULL -4087,
 *     num >= 2 -4084, mtype != START -4091, tuning not enabled -4091,
 *     nrvbs > 4 -4092.  Enable: sensor size from GetSensorAttr, one rmem
 *     block of w*h*(dtype+1)*nrvbs (fails -4094), the kernel gets
 *     {mode, dtype, mtype, paddr[4]} (paddr[i] = start + i*frame) by the
 *     tuning control 0x80000a7, a per-sensor thread runs the ring.  Disable:
 *     thread stopped, rmem freed.  Failed control ioctl -4090.
 *   worker (impisp_raw_thread 0xb38): loop { tuning get 0x80000a8 -> the
 *     address of the frame the ISP wrote (blocks one frame); frame_done(
 *     {vinum, w, h, dtype, vaddr}); tuning set 0x80000a8 (the same address:
 *     the frame, maybe edited by the callback, is fed back into the ISP) };
 *     a failed ioctl ends the thread.
 *
 * Only T41: on T40 0xc0045431 is TISP_VIDIOC_BYPASS_BIND
 * (openimp_p3_controls.c).  Needs open-tx-isp T41 with #320 (VIC done
 * completion offset, RAW controls routed).
 *
 * Differences (documented, not visible in return codes): the thread
 * arguments live in a static per-sensor slot (the vendor frees them right
 * after pthread_create, a race with the thread's copy); a NULL callback is
 * skipped instead of called; the blocking ioctls run with asynchronous
 * cancellation (as libc does at its own cancellation points), so the
 * cancel + join never waits for a frame that does not come; disable stops
 * the thread and the ring before the rmem block is freed (the vendor frees
 * first and leaves the ISP writing into it for up to one frame); a second
 * enable of a running sensor is a no-op returning 0 (the vendor allocates
 * and starts a second thread; its kernel ignores the repeated control).
 * A process that dies without a disable: the open-tx-isp T41 module stops
 * the ring when the tuning fd that enabled it is released (beyond vendor,
 * user OK 2026-11-11; the vendor module keeps the ISP writing into rmem).
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#include <imp/imp_isp.h>

#include "dma_alloc.h"

extern int OpenIMP_P1_TuningEnabled(void);
extern int OpenIMP_P1_IspIOCtlOpened(uint32_t command, void *argument);
extern int OpenIMP_P1_TuningWaitIOCtl(uint32_t command, void *argument);
extern int OpenIMP_P1_DupFd(int tuning);
extern int OpenIMP_P1_IspIsOpen(void);
extern int IMP_Free(uintptr_t address);

void OpenIMP_P1_VicDoneStop(void);

#define RV_E_GENERIC (-4095)
#define RV_E_IOCTL   (-4090)
#define RV_E_NOTRUN  (-4091)
#define RV_E_NOTOPEN (-4088)
#define RV_E_PARAM   (-4087)
#define RV_E_NUM     (-4084)
#define RV_E_NRVBS   (-4092)
#define RV_E_NOMEM   (-4094)

#define TISP_SET_VIC_DONE_CB 0xc0045431U
#define TISP_GET_VIC_DONE_CB 0xc0045432U
#define TISP_TUNING_IOCTL    0xc0105435U
#define TISP_RAW_RW_CONTROL  0x080000a7U
#define TISP_RAW_RW_HANDLER  0x080000a8U

#define RV_SENSORS 2
#define RV_BUFS    4

typedef struct {
    int32_t vinum;
    int32_t is_get;
    uint32_t id;
    uintptr_t payload;
} RvTuning;

/* kernel payload of 0x80000a7 (28 bytes, vendor stack layout) */
typedef struct {
    uint32_t mode, dtype, mtype;
    uint32_t paddr[RV_BUFS];
} RvRawCtl;
_Static_assert(sizeof(RvRawCtl) == 28, "RAW control payload");
_Static_assert(sizeof(IMPISPRawRwInfo) == 20, "IMPISPRawRwInfo");
#if UINTPTR_MAX == 0xffffffffU          /* target ABI (host tests are 64-bit) */
_Static_assert(sizeof(IMPISPRawRwAttr) == 20, "IMPISPRawRwAttr");
#endif

static pthread_mutex_t rv_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- VIC frame done ------------------------------------------------------ */

static struct {
    int enabled;
    pthread_t thread;
    void (*cb)(void);
} vic;

static void rv_close_fd(void *fd)
{
    close((int)(intptr_t)fd);
}

/* Blocking ioctl with asynchronous cancellation: pthread_cancel interrupts
 * the sleep in the driver instead of waiting for the next event. */
static int rv_ioctl_cancellable(int fd, uint32_t command, void *argument)
{
    int old, result;

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old);
    result = ioctl(fd, command, argument);
    pthread_setcanceltype(old, NULL);
    return result;
}

static void *vic_done_thread(void *arg)
{
    void (*cb)(void) = (void (*)(void))arg;
    int fd = OpenIMP_P1_DupFd(0);

    if (fd < 0)
        return NULL;
    pthread_cleanup_push(rv_close_fd, (void *)(intptr_t)fd);
    for (;;) {
        int32_t v = -1;

        pthread_testcancel();
        if (rv_ioctl_cancellable(fd, TISP_GET_VIC_DONE_CB, &v) != 0) {
            syslog(LOG_ERR, "libimp: VIC done wait failed: %d", errno);
            break;
        }
        if (v != 0 && cb)
            cb();
    }
    pthread_cleanup_pop(1);
    return NULL;
}

int32_t IMP_ISP_SetVicDoneCbFunc(void (*cb)(void))
{
    int32_t on = 1;
    int result = 0;

    pthread_mutex_lock(&rv_lock);
    if (!OpenIMP_P1_TuningEnabled() || vic.enabled) {
        result = RV_E_NOTRUN;
        goto out;
    }
    if (OpenIMP_P1_IspIOCtlOpened(TISP_SET_VIC_DONE_CB, &on) != 0) {
        result = RV_E_IOCTL;
        goto out;
    }
    vic.cb = cb;
    if (pthread_create(&vic.thread, NULL, vic_done_thread, (void *)cb) != 0) {
        on = 0;
        (void)OpenIMP_P1_IspIOCtlOpened(TISP_SET_VIC_DONE_CB, &on);
        result = RV_E_PARAM;            /* vendor -4087 */
        goto out;
    }
    vic.enabled = 1;
out:
    pthread_mutex_unlock(&rv_lock);
    return result;
}

/* IMP_ISP_DisableSensor (vendor IMP_ISP_SetVicDoneCbFunc_close) */
void OpenIMP_P1_VicDoneStop(void)
{
    int32_t off = 0;

    pthread_mutex_lock(&rv_lock);
    if (vic.enabled) {
        pthread_cancel(vic.thread);
        pthread_join(vic.thread, NULL);
        if (OpenIMP_P1_IspIOCtlOpened(TISP_SET_VIC_DONE_CB, &off) != 0)
            syslog(LOG_ERR, "libimp: VIC done close ioctl failed: %d", errno);
        vic.enabled = 0;
        vic.cb = NULL;
    }
    pthread_mutex_unlock(&rv_lock);
}

/* ---- RAW ring ------------------------------------------------------------ */

static struct {
    int running;
    pthread_t thread;
    IMPISPRawRwInfo info;               /* vinum, width, height, dtype */
    int32_t (*frame_done)(IMPISPRawRwInfo *info);
} raw[RV_SENSORS];
static IMPDMABufferInfo raw_buf;         /* one block, as the vendor (+68 global) */

static void *raw_thread(void *arg)
{
    int num = (int)(intptr_t)arg;
    int fd = OpenIMP_P1_DupFd(1);

    if (fd < 0)
        return NULL;
    pthread_cleanup_push(rv_close_fd, (void *)(intptr_t)fd);
    for (;;) {
        uint32_t paddr = 0;
        RvTuning request = { num, 1, TISP_RAW_RW_HANDLER, (uintptr_t)&paddr };

        pthread_testcancel();
        if (rv_ioctl_cancellable(fd, TISP_TUNING_IOCTL, &request) != 0) {
            syslog(LOG_ERR, "libimp: RAW get frame failed: %d", errno);
            break;
        }
        pthread_testcancel();
        if (raw[num].frame_done && raw_buf.virt_addr) {
            IMPISPRawRwInfo info = raw[num].info;

            info.vaddr = raw_buf.virt_addr + paddr - raw_buf.phys_addr;
            raw[num].frame_done(&info);
        }
        request.is_get = 0;
        if (ioctl(fd, TISP_TUNING_IOCTL, &request) != 0) {
            syslog(LOG_ERR, "libimp: RAW put frame failed: %d", errno);
            break;
        }
    }
    pthread_cleanup_pop(1);
    return NULL;
}

int32_t IMP_ISP_RAW_RwControl(IMPVI_NUM num, IMPISPRawRwAttr *attr)
{
    RvRawCtl ctl;
    RvTuning request;
    IMPISPSENSORAttr sensor;
    uint32_t frame, i;
    int result = 0;

    if (!OpenIMP_P1_IspIsOpen())
        return RV_E_NOTOPEN;
    if (!attr)
        return RV_E_PARAM;
    if ((unsigned)num >= RV_SENSORS)
        return RV_E_NUM;
    if (attr->mtype != TISP_RAW_MODULE_START)
        return RV_E_NOTRUN;
    if (!OpenIMP_P1_TuningEnabled())
        return RV_E_NOTRUN;
    if (attr->nrvbs > RV_BUFS)
        return RV_E_NRVBS;

    pthread_mutex_lock(&rv_lock);
    memset(&ctl, 0, sizeof(ctl));
    memset(&sensor, 0, sizeof(sensor));
    (void)IMP_ISP_Tuning_GetSensorAttr(num, &sensor);
    if (attr->mode != IMPISP_OPS_MODE_DISABLE) {
        if (raw[num].running)
            goto out;                   /* see the header: no second ring */
        frame = sensor.width * sensor.height * ((uint32_t)attr->dtype + 1U);
        memset(&raw_buf, 0, sizeof(raw_buf));
        if (DMA_AllocDescriptor(&raw_buf, (int)(frame * attr->nrvbs), "raw") != 0 ||
            !raw_buf.phys_addr) {
            memset(&raw_buf, 0, sizeof(raw_buf));
            result = RV_E_NOMEM;
            goto out;
        }
        for (i = 0; i < attr->nrvbs; i++)
            ctl.paddr[i] = raw_buf.phys_addr + i * frame;
        raw[num].info.vinum = num;
        raw[num].info.width = sensor.width;
        raw[num].info.height = sensor.height;
        raw[num].info.dtype = attr->dtype;
        raw[num].info.vaddr = 0;
        raw[num].frame_done = attr->frame_done;
        if (pthread_create(&raw[num].thread, NULL, raw_thread,
                           (void *)(intptr_t)num) == 0)
            raw[num].running = 1;
    } else if (raw[num].running) {
        pthread_cancel(raw[num].thread);
        pthread_join(raw[num].thread, NULL);
        raw[num].running = 0;
    }

    ctl.mode = attr->mode;
    ctl.dtype = attr->dtype;
    ctl.mtype = attr->mtype;
    request.vinum = num;
    request.is_get = 0;
    request.id = TISP_RAW_RW_CONTROL;
    request.payload = (uintptr_t)&ctl;
    /* enable sleeps about one second in the driver: not under the P1 lock */
    if (OpenIMP_P1_TuningWaitIOCtl(TISP_TUNING_IOCTL, &request) != 0)
        result = RV_E_IOCTL;

    if (attr->mode == IMPISP_OPS_MODE_DISABLE && raw_buf.virt_addr) {
        IMP_Free(raw_buf.virt_addr);
        memset(&raw_buf, 0, sizeof(raw_buf));
    }
out:
    pthread_mutex_unlock(&rv_lock);
    return result;
}
