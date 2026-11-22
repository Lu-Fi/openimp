/* T40 + T41 custom AE / AWB algorithm callbacks: IMP_ISP_SetAeAlgoFunc / IMP_ISP_SetAwbAlgoFunc.
 *
 * Follows the vendor libimp 1.3.1 (disassembly, 2026-10-31):
 *   SetAe/AwbAlgoFunc(num, func)   0x86630 / 0x872c8: copy the 5-pointer callback record,
 *                                  mark it pending; nothing else (no ioctl, returns 0).
 *   IMP_ISP_EnableSensor           calls the *_internal registration for every pending
 *                                  record before the sensor ioctls (0x866b8 / 0x87350):
 *                                  ioctl 0xc004541d (AE) / 0xc0045425 (AWB) with
 *                                  {cookie 0x336ac, num, init attr[]}, the kernel fills the
 *                                  init attribute, open(priv, init) is called, one worker
 *                                  thread per sensor (0 and 1) is started.
 *   worker (impisp_algo_thread1/3) loop: ioctl get (0xc004541b / 0xc0045423) hands the
 *                                  statistics {cookie, num, Info}, handle(priv, Info, Attr)
 *                                  is called, ioctl set (0xc004541c / 0xc0045424) returns
 *                                  {cookie, num, Attr}.  AE: when the callback answers an
 *                                  integration time in microseconds (unit 1) the value is
 *                                  converted to lines with the sensor attribute of the info
 *                                  record, as the vendor does.  The AE worker of sensor 1
 *                                  only loops on ioctl 0xc0045420 (vendor behaviour).
 *   IMP_ISP_DisableSensor          close (0x964a0 / 0x96b0c): cancel + join the worker,
 *                                  ioctl 0xc004541e / 0xc0045426 {.., num}, close(priv), the
 *                                  record is gone (SetAlgoFunc has to be called again).
 *   IMP_ISP_Tuning_SetSensorFPS    notify(priv, 0 = FPS_CHANGE, NULL) of an active AE record.
 *
 * Switch: on by default (the callback results reach the sensor and the white balance, shown on the T40 test camera); OPENIMP_ISP_ALGO_FUNC=0
 * makes the calls answer -1 / ENOTSUP again (see algo_enabled()).  Needs an open-tx-isp T40 / T41 module with the #65 / #76 fixes (T41 AWB library: #195, an extra beyond the vendor, whose T41 module has no AWB library path).
 *
 * Differences (documented): NULL pointers and sensors above 3 are refused with -1 (the vendor
 * dereferences them); a missing callback is skipped instead of called; the close waits at most
 * two seconds for the cancelled worker before it issues the close ioctl itself.
 * The statistics, the attribute write-back and the effect on the picture are done by the ISP
 * driver; this file only carries the data (driver side: open-tx-isp T40 tx_isp_*_algo_*).
 *
 * T33 (PLATFORM_T33, vendor libimp 2.0.2.1, objdump addresses, #784 group 4): the same protocol with
 * these deltas, all taken from the vendor binary:
 *   cookie 0x013501f4 (0x1019cc lui 0x135 / addiu 500; the open-tx-isp driver/t32 checks it as
 *   T32_AE_ALGO_VERSION); open requests 136 / 28 bytes (+18260 / +5824 of the 0x3974 sensor slot),
 *   statistics buffers 2004 / 12324 bytes, attribute buffers 64 / 24 bytes.
 *   SetAe/AwbAlgoFunc (0x10b818 / 0x10ba94) stay in src/t32/openimp_t32_isp_ctl.c (vendor state
 *   checks, NULL clears the flag) and hand the record over with OpenIMP_P1_AlgoRegister().
 *   EnableSensor 0x1052cc: after G_INPUT (0x80085403) and before STREAMON (0x80085407): AE open
 *   ioctl 0xc004541d (0x1056b4), open(priv, init) (0x1056dc, result ignored), worker thread
 *   (0x10570c); then AWB 0xc0045425 (0x1055bc) the same way.  A failed open ioctl is logged and
 *   the sensor enable goes on.  A worker thread for every sensor 0..3 (no sensor 1 special case).
 *   AE worker 0x10192c: ioctl 0xc004541b, handle(priv, info, attr), only when attr.change != 0
 *   (0x101a40) the us -> lines conversion and ioctl 0xc004541c; AWB worker 0x101c44: 0xc0045423,
 *   handle, set 0xc0045424 only when attr.change != 0 (0x101d3c).
 *   DisableSensor 0x105e88: after G_INPUT and before LINKS_STREAMOFF: cancel + join, close ioctl
 *   0xc004541e / 0xc0045426 {.., num}, close(priv) only when that ioctl succeeded; the flag is
 *   NOT cleared (the next EnableSensor opens the record again).
 *   SetSensorFPS 0x11129c: notify(priv, 0, NULL) when the AE flag is set and notify != NULL. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_isp.h>

#define ALGO_SENSORS 4
#ifdef PLATFORM_T33
#define ALGO_COOKIE 0x013501f4U
#define ALGO_GATE_CHANGE 1          /* set ioctl only when the callback answered change != 0 */
#define ALGO_KEEP_FLAG 1            /* DisableSensor keeps the record registered */
#define ALGO_INFO_CONST
#else
#define ALGO_COOKIE 0x336acU
#define ALGO_GATE_CHANGE 0
#define ALGO_KEEP_FLAG 0
#define ALGO_INFO_CONST const
#endif

#define TISP_AE_OPEN 0xc004541dU
#define TISP_AE_CLOSE 0xc004541eU
#define TISP_AE_GET 0xc004541bU
#define TISP_AE_SET 0xc004541cU
#define TISP_AE_GET_SEC 0xc0045420U
#define TISP_AWB_OPEN 0xc0045425U
#define TISP_AWB_CLOSE 0xc0045426U
#define TISP_AWB_GET 0xc0045423U
#define TISP_AWB_SET 0xc0045424U

#define AE_REQ_WORDS 2
#define AE_INFO_OFF 8
#define AE_SET_WORDS (AE_REQ_WORDS + sizeof(IMPISPAeAttr) / 4)    /* cookie, num, IMPISPAeAttr (T40 48, T33 56 bytes) */
#define AWB_SET_WORDS (AE_REQ_WORDS + sizeof(IMPISPAwbAttr) / 4)  /* cookie, num, IMPISPAwbAttr (16 bytes) */
#ifdef PLATFORM_T33
#define SENSOR_VTS_OFF 1980         /* IMPISPAeInfo.sensor_attr.vts (vendor 0x101b08: info + 1980) */
#define SENSOR_FPS_OFF 1984         /* IMPISPAeInfo.sensor_attr.fps (vendor 0x101b04: info + 1984) */
_Static_assert(sizeof(IMPISPAeInfo) == 1996 && sizeof(IMPISPAeAttr) == 56 && sizeof(IMPISPAeInitAttr) == 128,
               "T33 AE algo layout (vendor buffers 2004 / 64 / 136 bytes)");
_Static_assert(sizeof(IMPISPAwbInfo) == 12316 && sizeof(IMPISPAwbAttr) == 16 && sizeof(IMPISPAwbInitAttr) == 20,
               "T33 AWB algo layout (vendor buffers 12324 / 24 / 28 bytes)");
_Static_assert(__builtin_offsetof(IMPISPAeInfo, sensor_attr.vts) == SENSOR_VTS_OFF &&
               __builtin_offsetof(IMPISPAeInfo, sensor_attr.fps) == SENSOR_FPS_OFF, "T33 AE info sensor attr");
#else
#define SENSOR_VTS_OFF 1978         /* IMPISPAeInfo.sensor_attr.vts (packed) */
#define SENSOR_FPS_OFF 1982         /* IMPISPAeInfo.sensor_attr.fps: (num << 16) | den */
#endif

struct algo_rec {
    IMPISPAeAlgoFunc ae;            /* copy of the registered record (live: the open one) */
    IMPISPAwbAlgoFunc awb;
    int ae_flag, awb_flag;          /* 1 = registered (pending or running) */
    int ae_running, awb_running;
    pthread_t ae_thread, awb_thread;
    volatile int ae_done, awb_done; /* worker has left its loop */
    int fd;
    uint32_t ae_req[AE_REQ_WORDS + 32];     /* open request: cookie, num, init attr (up to 128 bytes) */
    uint32_t awb_req[AE_REQ_WORDS + 8];
};

static struct algo_rec algo[ALGO_SENSORS];
static pthread_mutex_t algo_lock = PTHREAD_MUTEX_INITIALIZER;

static void algo_name(const char *name)
{
    prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
}

/* On by default.  The registration, the statistics in handle() and the callback results (exposure time / analog gain / ISP gain for the AE,
 * R/B gains for the AWB) were shown to change the sensor exposure and the picture colour on the T40 test camera (#65);
 * OPENIMP_ISP_ALGO_FUNC=0 switches the feature off (-1 / ENOTSUP, nothing is registered). */
static int algo_enabled(void)
{
    const char *e = getenv("OPENIMP_ISP_ALGO_FUNC");

    return !e || atoi(e) != 0;
}

#ifdef PLATFORM_T33
/* src/t32/openimp_t32_isp_ctl.c SetAe/AwbAlgoFunc: kind 0 = AE, 1 = AWB; rec NULL = clear the flag.
 * Returns -1 / ENOTSUP when OPENIMP_ISP_ALGO_FUNC=0 (nothing is registered). */
int OpenIMP_P1_AlgoRegister(int num, int kind, const void *rec)
{
    if (!algo_enabled()) {
        errno = ENOTSUP;
        return -1;
    }
    if ((unsigned)num >= ALGO_SENSORS)
        return -1;
    pthread_mutex_lock(&algo_lock);
    if (kind == 0) {
        if (rec)
            algo[num].ae = *(const IMPISPAeAlgoFunc *)rec;
        algo[num].ae_flag = rec != NULL;
    } else {
        if (rec)
            algo[num].awb = *(const IMPISPAwbAlgoFunc *)rec;
        algo[num].awb_flag = rec != NULL;
    }
    pthread_mutex_unlock(&algo_lock);
    return 0;
}
#else
int32_t IMP_ISP_SetAeAlgoFunc(IMPVI_NUM num, IMPISPAeAlgoFunc *ae_func)
{
    if (!algo_enabled()) {
        errno = ENOTSUP;
        return -1;
    }
    if ((unsigned)num >= ALGO_SENSORS || !ae_func)
        return -1;
    pthread_mutex_lock(&algo_lock);
    algo[num].ae = *ae_func;
    algo[num].ae_flag = 1;
    pthread_mutex_unlock(&algo_lock);
    return 0;
}

int32_t IMP_ISP_SetAwbAlgoFunc(IMPVI_NUM num, IMPISPAwbAlgoFunc *awb_func)
{
    if (!algo_enabled()) {
        errno = ENOTSUP;
        return -1;
    }
    if ((unsigned)num >= ALGO_SENSORS || !awb_func)
        return -1;
    pthread_mutex_lock(&algo_lock);
    algo[num].awb = *awb_func;
    algo[num].awb_flag = 1;
    pthread_mutex_unlock(&algo_lock);
    return 0;
}
#endif

static uint32_t rd32(const unsigned char *p)
{
    uint32_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

/* integration time microseconds -> lines, divisor as the vendor worker computes it */
static uint32_t ae_us_divisor(const unsigned char *info)
{
    uint32_t vts = rd32(info + SENSOR_VTS_OFF);
    uint32_t fps = rd32(info + SENSOR_FPS_OFF);
    uint32_t t;

    if (!vts)
        return 0;
    t = ((fps & 0xffffU) * 1000000U) / vts;
    return t * (fps >> 16);
}

/* cancel / exit of a worker: tell algo_join at once (without it every DisableSensor waited the full 2 s, #784) */
static void algo_left(void *done)
{
    *(volatile int *)done = 1;
}

static void *ae_worker(void *arg)
{
    struct algo_rec *a = arg;
    int num = (int)(a - algo);
    uint32_t buf[(AE_INFO_OFF + sizeof(IMPISPAeInfo) + 3) / 4 + 1];
    uint32_t set[AE_SET_WORDS];
    IMPISPAeAttr *attr = (IMPISPAeAttr *)(set + AE_REQ_WORDS);

    algo_name("impisp_algo");
    pthread_cleanup_push(algo_left, (void *)&a->ae_done);
    memset(buf, 0, sizeof(buf));
    memset(set, 0, sizeof(set));
    buf[0] = ALGO_COOKIE;
    buf[1] = (uint32_t)num;
    set[0] = ALGO_COOKIE;
    set[1] = (uint32_t)num;
    for (;;) {
        pthread_testcancel();
        if (!ALGO_GATE_CHANGE && num == 1) {                     /* vendor: sensor 1 only waits on the second handle ioctl */
            if (ioctl(a->fd, TISP_AE_GET_SEC, buf) != 0)
                break;
            continue;
        }
        if (ioctl(a->fd, TISP_AE_GET, buf) != 0)
            break;
        if (a->ae.handle)
            a->ae.handle(a->ae.priv_data,
                         (ALGO_INFO_CONST IMPISPAeInfo *)((unsigned char *)buf + AE_INFO_OFF), attr);
#if ALGO_GATE_CHANGE
        if (!attr->change)
            continue;
#endif
        if (attr->AeIntegrationTimeUnit == ISP_CORE_EXPR_UNIT_US) {
            uint32_t d = ae_us_divisor((unsigned char *)buf + AE_INFO_OFF);

            if (d) {
                attr->AeIntegrationTime /= d;
                attr->AeShortIntegrationTime /= d;
            }
        }
        if (ioctl(a->fd, TISP_AE_SET, set) != 0)
            break;
    }
    pthread_cleanup_pop(1);         /* sets ae_done, also when the cancel ends the thread */
    return NULL;
}

static void *awb_worker(void *arg)
{
    struct algo_rec *a = arg;
    int num = (int)(a - algo);
    uint32_t buf[(AE_INFO_OFF + sizeof(IMPISPAwbInfo) + 3) / 4 + 1];
    uint32_t set[AWB_SET_WORDS];
    IMPISPAwbAttr *attr = (IMPISPAwbAttr *)(set + AE_REQ_WORDS);

    algo_name("impisp_algo");
    pthread_cleanup_push(algo_left, (void *)&a->awb_done);
    memset(buf, 0, sizeof(buf));
    memset(set, 0, sizeof(set));
    buf[0] = ALGO_COOKIE;
    buf[1] = (uint32_t)num;
    set[0] = ALGO_COOKIE;
    set[1] = (uint32_t)num;
    for (;;) {
        pthread_testcancel();
        if (ioctl(a->fd, TISP_AWB_GET, buf) != 0)
            break;
        if (a->awb.handle)
            a->awb.handle(a->awb.priv_data,
                          (ALGO_INFO_CONST IMPISPAwbInfo *)((unsigned char *)buf + AE_INFO_OFF), attr);
#if ALGO_GATE_CHANGE
        if (!attr->change)
            continue;
#endif
        if (ioctl(a->fd, TISP_AWB_SET, set) != 0)
            break;
    }
    pthread_cleanup_pop(1);         /* sets awb_done, also when the cancel ends the thread */
    return NULL;
}

/* IMP_ISP_EnableSensor: register the pending records with the driver and start the workers */
void OpenIMP_P1_AlgoStart(int fd, int num)
{
    struct algo_rec *a;

    if ((unsigned)num >= ALGO_SENSORS || fd < 0)
        return;
    a = &algo[num];
    pthread_mutex_lock(&algo_lock);
    a->fd = fd;
    if (a->ae_flag == 1 && !a->ae_running) {
        memset(a->ae_req, 0, sizeof(a->ae_req));
        a->ae_req[0] = ALGO_COOKIE;
        a->ae_req[1] = (uint32_t)num;
        if (ioctl(fd, TISP_AE_OPEN, a->ae_req) == 0) {
            if (a->ae.open)
                a->ae.open(a->ae.priv_data, (IMPISPAeInitAttr *)(a->ae_req + AE_REQ_WORDS));
            a->ae_done = 0;
            if ((ALGO_GATE_CHANGE || num <= 1) && pthread_create(&a->ae_thread, NULL, ae_worker, a) == 0)
                a->ae_running = 1;
            else if (!ALGO_GATE_CHANGE && num > 1)
                a->ae_running = 1;      /* vendor: no worker above sensor 1, the record stays open */
        } else {
            fprintf(stderr, "openimp-algo: AE open ioctl failed: %s\n", strerror(errno));
        }
    }
    if (a->awb_flag == 1 && !a->awb_running) {
        memset(a->awb_req, 0, sizeof(a->awb_req));
        a->awb_req[0] = ALGO_COOKIE;
        a->awb_req[1] = (uint32_t)num;
        if (ioctl(fd, TISP_AWB_OPEN, a->awb_req) == 0) {
            if (a->awb.open)
                a->awb.open(a->awb.priv_data, (IMPISPAwbInitAttr *)(a->awb_req + AE_REQ_WORDS));
            a->awb_done = 0;
            if ((ALGO_GATE_CHANGE || num <= 1) && pthread_create(&a->awb_thread, NULL, awb_worker, a) == 0)
                a->awb_running = 1;
            else if (!ALGO_GATE_CHANGE && num > 1)
                a->awb_running = 1;
        } else {
            fprintf(stderr, "openimp-algo: AWB open ioctl failed: %s\n", strerror(errno));
        }
    }
    pthread_mutex_unlock(&algo_lock);
}

/* cancel the worker; if it does not leave on the signal, the close ioctl wakes the driver wait */
static int algo_join(pthread_t th, volatile int *done, int fd, unsigned int close_cmd, int num)
{
    uint32_t req[2] = {ALGO_COOKIE, (uint32_t)num};
    int i, closed = 0;

    pthread_cancel(th);
    for (i = 0; i < 200 && !*done; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};

        nanosleep(&ts, NULL);
    }
    if (!*done)
        closed = ioctl(fd, close_cmd, req) == 0;
    pthread_join(th, NULL);
    return closed;
}

/* IMP_ISP_DisableSensor: stop the workers, tell the driver, close the user libraries */
void OpenIMP_P1_AlgoStop(int fd, int num)
{
    struct algo_rec *a;
    uint32_t req[2];

    if ((unsigned)num >= ALGO_SENSORS)
        return;
    a = &algo[num];
    req[0] = ALGO_COOKIE;
    req[1] = (uint32_t)num;
    pthread_mutex_lock(&algo_lock);
    if (a->ae_flag == 1 && a->ae_running) {
        int done_close = 0;

        if (ALGO_GATE_CHANGE || num <= 1)
            done_close = algo_join(a->ae_thread, &a->ae_done, fd, TISP_AE_CLOSE, num);
        /* the join already sent the close ioctl when the worker did not leave on the cancel: once is enough (a second
         * close would free the driver's info block twice) */
        if (done_close || ioctl(fd, TISP_AE_CLOSE, req) == 0) {
            a->ae_flag = ALGO_KEEP_FLAG;
            a->ae_running = 0;
            if (a->ae.close)
                a->ae.close(a->ae.priv_data);
        } else {
            fprintf(stderr, "openimp-algo: AE close ioctl failed: %s\n", strerror(errno));
            if (ALGO_KEEP_FLAG)
                a->ae_running = 0;     /* T33: worker joined, flag kept, no close callback (vendor 0x106528) */
        }
    }
    if (a->awb_flag == 1 && a->awb_running) {
        int done_close = 0;

        if (ALGO_GATE_CHANGE || num <= 1)
            done_close = algo_join(a->awb_thread, &a->awb_done, fd, TISP_AWB_CLOSE, num);
        if (done_close || ioctl(fd, TISP_AWB_CLOSE, req) == 0) {
            a->awb_flag = ALGO_KEEP_FLAG;
            a->awb_running = 0;
            if (a->awb.close)
                a->awb.close(a->awb.priv_data);
        } else {
            fprintf(stderr, "openimp-algo: AWB close ioctl failed: %s\n", strerror(errno));
            if (ALGO_KEEP_FLAG)
                a->awb_running = 0;     /* T33: worker joined, flag kept, no close callback (vendor 0x106528) */
        }
    }
    pthread_mutex_unlock(&algo_lock);
}

/* IMP_ISP_Tuning_SetSensorFPS succeeded: tell an active AE library (vendor 0x77f40) */
void OpenIMP_P1_AlgoNotifyFps(int num)
{
    struct algo_rec *a;
    int (*notify)(void *, IMPISPAeNotify, void *) = NULL;
    void *priv = NULL;

    if ((unsigned)num >= ALGO_SENSORS)
        return;
    a = &algo[num];
    pthread_mutex_lock(&algo_lock);
    if (a->ae_flag == 1 && (ALGO_KEEP_FLAG || a->ae_running)) {   /* T33: the flag alone (0x1110d0) */
        notify = a->ae.notify;
        priv = a->ae.priv_data;
    }
    pthread_mutex_unlock(&algo_lock);
    if (notify)
        notify(priv, IMPISP_AE_NOTIFY_FPS_CHANGE, NULL);
}
