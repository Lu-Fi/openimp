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
 * Review 2026-11-11 (F1/F5): IMP_ISP_DisableTuning and DisableSensor stop
 * a ring still on (OpenIMP_P1_RawStop) before the tuning node closes; the
 * enable sends the control before it starts the thread, so a refused
 * control frees the block and leaves nothing running, and a failed
 * pthread_create switches the ring off again and returns -4095 (the
 * vendor leaves the ring on without a consumer and returns 0).
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
void OpenIMP_P1_RawStop(void);

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
static int raw_tp[RV_SENSORS];          /* ring started by test pattern mode 6 (beyond vendor) */

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

/* tuning control 0x80000a7 (enable sleeps about one second in the driver:
 * not under the P1 lock) */
static int rv_raw_control(int num, uint32_t mode, uint32_t dtype, uint32_t mtype,
                          const uint32_t *paddr)
{
    RvRawCtl ctl;
    RvTuning request;

    memset(&ctl, 0, sizeof(ctl));
    ctl.mode = mode;
    ctl.dtype = dtype;
    ctl.mtype = mtype;
    if (paddr)
        memcpy(ctl.paddr, paddr, sizeof(ctl.paddr));
    request.vinum = num;
    request.is_get = 0;
    request.id = TISP_RAW_RW_CONTROL;
    request.payload = (uintptr_t)&ctl;
    return OpenIMP_P1_TuningWaitIOCtl(TISP_TUNING_IOCTL, &request) != 0 ? RV_E_IOCTL : 0;
}

static void rv_raw_free(void)
{
    if (raw_buf.virt_addr)
        IMP_Free(raw_buf.virt_addr);
    memset(&raw_buf, 0, sizeof(raw_buf));
}

/* rv_lock held: thread, ring and rmem block of a running sensor off */
static int rv_raw_stop(int num)
{
    int result;

    pthread_cancel(raw[num].thread);
    pthread_join(raw[num].thread, NULL);
    raw[num].running = 0;
    raw_tp[num] = 0;
    result = rv_raw_control(num, IMPISP_OPS_MODE_DISABLE, raw[num].info.dtype,
                            TISP_RAW_MODULE_START, NULL);
    rv_raw_free();
    return result;
}

/* IMP_ISP_DisableTuning / IMP_ISP_DisableSensor (beyond vendor, review
 * 2026-11-11 F1): a ring the application left on is stopped while the
 * tuning node is still open; the vendor keeps its raw thread sleeping in
 * the driver through the ISP teardown. */
void OpenIMP_P1_RawStop(void)
{
    int num;

    pthread_mutex_lock(&rv_lock);
    for (num = 0; num < RV_SENSORS; num++)
        if (raw[num].running) {
            syslog(LOG_WARNING, "libimp: RAW ring of sensor %d still on at ISP teardown: stopped", num);
            (void)rv_raw_stop(num);
        }
    pthread_mutex_unlock(&rv_lock);
}

/* rv_lock held, ring of num off: rmem block, ring control, thread (the
 * vendor enable path of RAW_RwControl) */
static int rv_raw_start(int num, const IMPISPRawRwAttr *attr)
{
    IMPISPSENSORAttr sensor;
    uint32_t paddr[RV_BUFS];
    uint32_t frame, i;
    int result;

    memset(&sensor, 0, sizeof(sensor));
    (void)IMP_ISP_Tuning_GetSensorAttr(num, &sensor);
    frame = sensor.width * sensor.height * ((uint32_t)attr->dtype + 1U);
    memset(&raw_buf, 0, sizeof(raw_buf));
    if (DMA_AllocDescriptor(&raw_buf, (int)(frame * attr->nrvbs), "raw") != 0 ||
        !raw_buf.phys_addr) {
        memset(&raw_buf, 0, sizeof(raw_buf));
        return RV_E_NOMEM;
    }
    memset(paddr, 0, sizeof(paddr));
    for (i = 0; i < attr->nrvbs; i++)
        paddr[i] = raw_buf.phys_addr + i * frame;
    raw[num].info.vinum = num;
    raw[num].info.width = sensor.width;
    raw[num].info.height = sensor.height;
    raw[num].info.dtype = attr->dtype;
    raw[num].info.vaddr = 0;
    raw[num].frame_done = attr->frame_done;
    /* ring first, then the thread: a refused control (older driver,
     * t41_raw_ring=0) leaves nothing behind (review 2026-11-11 F5) */
    result = rv_raw_control(num, attr->mode, attr->dtype, attr->mtype, paddr);
    if (result) {
        rv_raw_free();
        return result;
    }
    if (pthread_create(&raw[num].thread, NULL, raw_thread, (void *)(intptr_t)num) != 0) {
        /* no consumer: the ring would freeze the picture */
        (void)rv_raw_control(num, IMPISP_OPS_MODE_DISABLE, attr->dtype, attr->mtype, NULL);
        rv_raw_free();
        return RV_E_GENERIC;
    }
    raw[num].running = 1;
    return 0;
}

int32_t IMP_ISP_RAW_RwControl(IMPVI_NUM num, IMPISPRawRwAttr *attr)
{
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
    if (attr->mode == IMPISP_OPS_MODE_DISABLE) {
        if (raw[num].running) {
            result = rv_raw_stop(num);
        } else {
            result = rv_raw_control(num, attr->mode, attr->dtype, attr->mtype, NULL);
            rv_raw_free();
        }
        goto out;
    }
    if (raw[num].running) {
        if (raw_tp[num])
            result = RV_E_NOTRUN;       /* held by test pattern mode 6 */
        goto out;                       /* see the header: no second ring */
    }
    result = rv_raw_start(num, attr);
out:
    pthread_mutex_unlock(&rv_lock);
    return result;
}

/* ---- test pattern mode 6 (src/openimp_testpattern.c, beyond vendor) ------
 *
 * The T41 ISP has no test generator at its input (the T10/T20 Apical core
 * has one, src/isp/isp_tseries.c).  Its vendor way to put a picture of the
 * application's choice in front of the ISP is the RAW ring above: the ISP
 * writes each sensor frame to DDR, the frame_done callback may change it,
 * and the RDMA feeds the same buffer back into the ISP front end
 * (IMP_ISP_RAW_RwControl remark in the vendor header; the #320 read-back
 * proof).  Mode 6 starts that ring itself, RAW8 with one buffer (the only
 * size that fits next to a streamer in rmem=30M), and its callback
 * overwrites every frame with a Bayer picture in the sensor's colour filter
 * order, so the whole ISP (BLC, LSC, demosaic, WB, CCM, gamma, NR,
 * sharpening, scaler, encoder) runs on a sensor-independent picture:
 *   1 flat field (half scale), 2 four horizontal bands red, green, blue,
 *   white with a left-to-right ramp, 3 four vertical bands with a
 *   top-to-bottom ramp, 4 eight bars at 75 % (white, yellow, cyan, green,
 *   magenta, red, blue, black).
 * The colour filter order is read from the ISP core (0x13300088 bits 4:0,
 * written by the driver from the sensor's media bus code with the OEM map
 * {RGGB 0, BGGR 1, GRBG 2, GBRG 3}) on every frame, so a flip that moves
 * the order is followed.  Other values (RGB-IR) are not supported.
 * Limits: the ring with one buffer halves the output rate (25 -> 12.5 fps
 * on the gc5603, as with the vendor ring); AE and AWB keep running on the
 * picture, as on T10/T20.  An application ring (RAW_RwControl) has
 * priority: mode 6 returns EBUSY while one runs, and RAW_RwControl ENABLE
 * returns -4091 while mode 6 holds the ring (DISABLE stops it). */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>

#define TP6_ISP_PAGE  0x13300000u
#define TP6_CFA_REG   0x88u
#define TP6_BLACK     15u               /* RAW8 black level (gc5603 60/1023 >> 2) */

static uint32_t tp6_pattern;            /* 1..4 while the ring is held (atomic) */
static volatile uint32_t *tp6_isp;      /* ISP core page, uncached */
static uint8_t *tp6_row[2];
static uint32_t tp6_key[2], tp6_w;

/* channel (0 R, 1 G, 2 B) at [cfa][y & 1][x & 1] */
static const uint8_t tp6_chan[4][2][2] = {
    { {0, 1}, {1, 2} },                 /* RGGB */
    { {2, 1}, {1, 0} },                 /* BGGR */
    { {1, 0}, {2, 1} },                 /* GRBG */
    { {1, 2}, {0, 1} },                 /* GBRG */
};

/* rgb in 0..1000 of full scale */
static void tp6_bar(uint32_t i, uint32_t rgb[3])
{
    static const uint8_t bars[8] = { 7, 3, 6, 2, 5, 1, 4, 0 };  /* bit 0 R, 1 G, 2 B */
    uint32_t c;

    for (c = 0; c < 3; c++)
        rgb[c] = (bars[i & 7] >> c) & 1 ? 750 : 0;
}

/* row key: rows with the same key and parity are equal */
static uint32_t tp6_row_key(uint32_t pattern, uint32_t y, uint32_t h)
{
    if (pattern == 2)
        return 1 + y * 4 / h;
    if (pattern == 3)
        return 1 + (h > 1 ? y * 255 / (h - 1) : 0);
    return 1;
}

static void tp6_pixel(uint32_t pattern, uint32_t key, uint32_t x, uint32_t w, uint32_t rgb[3])
{
    uint32_t v, band;

    rgb[0] = rgb[1] = rgb[2] = 0;
    switch (pattern) {
    case 1:
        rgb[0] = rgb[1] = rgb[2] = 500;
        return;
    case 2:                              /* band from the key, ramp along x */
        band = key - 1;
        v = w > 1 ? x * 1000 / (w - 1) : 0;
        break;
    case 3:                              /* band along x, ramp level from the key */
        band = x * 4 / w;
        v = (key - 1) * 1000 / 255;
        break;
    default:
        tp6_bar(x * 8 / w, rgb);
        return;
    }
    if (band >= 3)
        rgb[0] = rgb[1] = rgb[2] = v;
    else
        rgb[band] = v;
}

static void tp6_build(uint8_t *row, uint32_t pattern, uint32_t key, uint32_t cfa,
                      uint32_t parity, uint32_t w)
{
    uint32_t x, rgb[3];

    for (x = 0; x < w; x++) {
        tp6_pixel(pattern, key, x, w, rgb);
        row[x] = (uint8_t)(TP6_BLACK + rgb[tp6_chan[cfa][parity][x & 1]] * (255u - TP6_BLACK) / 1000u);
    }
}

/* raw thread: one call per RAW frame, before the RDMA reads it back */
static int32_t tp6_frame_done(IMPISPRawRwInfo *info)
{
    uint32_t pattern = __atomic_load_n(&tp6_pattern, __ATOMIC_ACQUIRE);
    uint32_t cfa, y, w = info->width, h = info->height;
    uint8_t *dst = (uint8_t *)(uintptr_t)info->vaddr;

    if (!pattern || !dst || !w || !h || info->dtype != TISP_RAW_DTYPE_RAW8 || !tp6_isp)
        return 0;
    cfa = tp6_isp[TP6_CFA_REG / 4] & 0x1f;
    if (cfa > 3)
        return 0;                       /* order changed to one we cannot draw: pass through */
    if (tp6_w != w) {
        free(tp6_row[0]);
        free(tp6_row[1]);
        tp6_row[0] = malloc(w);
        tp6_row[1] = malloc(w);
        tp6_key[0] = tp6_key[1] = 0;
        tp6_w = (tp6_row[0] && tp6_row[1]) ? w : 0;
        if (!tp6_w)
            return 0;
    }
    for (y = 0; y < h; y++) {
        uint32_t parity = y & 1;
        uint32_t key = tp6_row_key(pattern, y, h);
        uint32_t full = pattern << 24 | cfa << 16 | key;

        if (tp6_key[parity] != full) {
            tp6_build(tp6_row[parity], pattern, key, cfa, parity, w);
            tp6_key[parity] = full;
        }
        memcpy(dst + (size_t)y * w, tp6_row[parity], w);
    }
    (void)DMA_RmemFlushCache(dst, w * h, 1);
    return 0;
}

static int tp6_map(void)
{
    int fd;
    void *m;

    if (tp6_isp)
        return 0;
#ifdef RAWVIC_HOST_TEST
    {
        extern volatile uint32_t rawvic_test_isp[];

        (void)fd;
        (void)m;
        tp6_isp = rawvic_test_isp;
        return 0;
    }
#endif
    fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, TP6_ISP_PAGE);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    tp6_isp = m;
    return 0;
}

/* pattern 0 = sensor input again, 1..4 = picture; set_lock of the test
 * pattern module held */
int openimp_tp_ispin_set(uint32_t pattern)
{
    IMPISPRawRwAttr attr;
    int result = 0, err = 0;

    if (pattern > 4) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&rv_lock);
    if (!pattern) {
        if (raw[0].running && raw_tp[0] && rv_raw_stop(0) != 0)
            err = EIO;
        __atomic_store_n(&tp6_pattern, 0, __ATOMIC_RELEASE);
        goto out;
    }
    if (raw[0].running) {
        if (raw_tp[0])
            __atomic_store_n(&tp6_pattern, pattern, __ATOMIC_RELEASE);
        else
            err = EBUSY;                /* the application's ring */
        goto out;
    }
    if (!OpenIMP_P1_IspIsOpen() || !OpenIMP_P1_TuningEnabled() || tp6_map() != 0) {
        err = ENODEV;
        goto out;
    }
    if ((tp6_isp[TP6_CFA_REG / 4] & 0x1f) > 3) {
        err = ENOTSUP;
        goto out;
    }
    __atomic_store_n(&tp6_pattern, pattern, __ATOMIC_RELEASE);
    memset(&attr, 0, sizeof(attr));
    attr.mode = IMPISP_OPS_MODE_ENABLE;
    attr.dtype = TISP_RAW_DTYPE_RAW8;
    attr.mtype = TISP_RAW_MODULE_START;
    attr.nrvbs = 1;
    attr.frame_done = tp6_frame_done;
    result = rv_raw_start(0, &attr);
    if (result == 0) {
        raw_tp[0] = 1;
    } else {
        __atomic_store_n(&tp6_pattern, 0, __ATOMIC_RELEASE);
        err = result == RV_E_NOMEM ? ENOMEM : EIO;
        syslog(LOG_WARNING, "libimp: test pattern 6: RAW ring start failed: %d", result);
    }
out:
    pthread_mutex_unlock(&rv_lock);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}
