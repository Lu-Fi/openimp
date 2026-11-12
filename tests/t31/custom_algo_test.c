/*
 * Host test of the T23 / T31 custom AE / AWB library path (IMP_ISP_SetAeAlgoFunc /
 * IMP_ISP_SetAwbAlgoFunc in src/isp/isp_tseries.c, vendor libimp T23 1.3.0 / T31 1.1.6 flow):
 * registration without ioctl, the open ioctls + open callbacks at IMP_ISP_EnableSensor, the
 * worker loops (get handle -> handle callback -> set handle, cookie 0x336ac, us -> lines like
 * the vendor), the FPS notify, the close at IMP_ISP_DisableSensor and the OPENIMP_ISP_ALGO_FUNC
 * switch.  Built twice (PLATFORM_T31 / PLATFORM_T23) against a fake ioctl() driver.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/globals.h"
#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

extern int IMP_ISP_SetAeAlgoFunc(void *func);
extern int IMP_ISP_SetAwbAlgoFunc(void *func);
extern int IMP_ISP_EnableSensor(void);
extern int IMP_ISP_DisableSensor(void);

#define COOKIE 0x336acU
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int n_ae_open, n_ae_close, n_awb_open, n_awb_close, n_ae_get, n_awb_get, n_ae_set, n_awb_set, n_other;
static uint32_t ae_open_cookie = 0xffffffffU, ae_set_cookie, awb_set_cookie, ae_set_it, ae_set_change, awb_set_r;
static void *awb_open_arg = (void *)1;
static int open_on_enabled;
static ISPDevice dev;

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    uint32_t *p;
    __builtin_va_list ap;

    (void)fd;
    __builtin_va_start(ap, req);
    p = __builtin_va_arg(ap, uint32_t *);
    __builtin_va_end(ap);
    pthread_mutex_lock(&lk);
    switch ((uint32_t)req) {
    case 0x800456dd:                    /* AE open: the driver fills the init attribute */
        n_ae_open++; ae_open_cookie = p[0];
        p[2 + 1] = 600;                 /* AeIntegrationTime */
        p[2 + 9] = 1100;                /* AeMaxIntegrationTime */
        break;
    case 0xc00456e3: n_awb_open++; awb_open_arg = p; break;
    case 0x800456de: n_ae_close++; break;
    case 0xc00456e4: n_awb_close++; break;
    case 0x800456db:                    /* AE get: info, sensor words at buffer 0x7c8 / 0x7cc */
        n_ae_get++;
        p[2 + 3 + 0] = 0;
        p[0x7c8 / 4] = 1125;            /* vts */
        p[0x7cc / 4] = (25U << 16) | 1; /* fps 25/1 */
        break;
    case 0x800456dc: n_ae_set++; ae_set_cookie = p[0]; ae_set_change = p[2]; ae_set_it = p[4]; break;
    case 0xc00456e1: n_awb_get++; p[2] = 0x123; p[3] = 0x234; break;
    case 0xc00456e2: n_awb_set++; awb_set_cookie = p[0]; awb_set_r = p[3]; break;
    case 0x40045626: if (p) *(int32_t *)p = 0; n_other++; break;      /* GET_SENSOR_INDEX */
    default: n_other++; break;
    }
    pthread_mutex_unlock(&lk);
    if ((uint32_t)req == 0x800456db || (uint32_t)req == 0xc00456e1)
        usleep(2000);                   /* a frame */
    return 0;
}

ISPDevice *gISP;
static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

typedef struct { void *priv; void *open, *close, *handle, *notify; } Rec;
static int priv_tag;
static volatile int c_ae_open, c_ae_close, c_ae_handle, c_ae_notify, c_awb_open, c_awb_close, c_awb_handle;
static volatile void *seen_priv;
static volatile uint32_t init_maxit, awb_info_r;
static volatile int us_mode;

static int ae_open(void *p, uint32_t *init) { seen_priv = p; init_maxit = init[9]; if (open_on_enabled) c_ae_open += 100; c_ae_open++; return 0; }
static void ae_close(void *p) { (void)p; c_ae_close++; }
static void ae_handle(void *p, const uint32_t *info, uint32_t *attr)
{
    (void)p; (void)info;
    attr[0] = 1;                        /* change */
    attr[1] = us_mode ? 1 : 0;          /* unit */
    attr[2] = us_mode ? 40000 : 321;    /* integration time */
    c_ae_handle++;
}
static int ae_notify(void *p, int n, void *d) { (void)p; (void)d; if (n == 0) c_ae_notify++; return 0; }
static int awb_open(void *p) { (void)p; c_awb_open++; return 0; }
static void awb_close(void *p) { (void)p; c_awb_close++; }
static void awb_handle(void *p, const uint32_t *info, uint32_t *attr) { (void)p; awb_info_r = info[0]; attr[0] = 1; attr[1] = 0x155; attr[2] = 0x166; c_awb_handle++; }

static void wait_for(volatile int *v, int n)
{
    int i;

    for (i = 0; i < 400 && *v < n; i++)
        usleep(5000);
}

int main(void)
{
    static Rec ae = { &priv_tag, (void *)ae_open, (void *)ae_close, (void *)ae_handle, (void *)ae_notify };
    static Rec awb = { &priv_tag, (void *)awb_open, (void *)awb_close, (void *)awb_handle, NULL };
    uint32_t d;

    memset(&dev, 0, sizeof(dev));
    dev.fd = 5;
    gISP = &dev;

    setenv("OPENIMP_ISP_ALGO_FUNC", "0", 1);
    errno = 0;
    CHECK(IMP_ISP_SetAeAlgoFunc(&ae) == -1 && errno == ENOTSUP, "switched off: -1 / ENOTSUP");
    unsetenv("OPENIMP_ISP_ALGO_FUNC");
    CHECK(IMP_ISP_SetAeAlgoFunc(NULL) == -1, "NULL record refused");
    CHECK(IMP_ISP_SetAeAlgoFunc(&ae) == 0 && IMP_ISP_SetAwbAlgoFunc(&awb) == 0, "records accepted");
    CHECK(n_ae_open + n_awb_open + n_other == 0, "registration issues no ioctl (vendor: only at EnableSensor)");

    CHECK(IMP_ISP_EnableSensor() == 0, "EnableSensor");
    CHECK(n_ae_open == 1 && ae_open_cookie == COOKIE, "AE open ioctl 0x800456dd with cookie 0x336ac");
    CHECK(n_awb_open == 1 && awb_open_arg == NULL, "AWB open ioctl 0xc00456e3 without argument");
    CHECK(c_ae_open == 1 && seen_priv == &priv_tag && init_maxit == 1100, "AE open(priv, init attr filled by the driver)");
    CHECK(c_awb_open == 1, "AWB open(priv)");
    wait_for(&c_ae_handle, 5);
    wait_for(&c_awb_handle, 5);
    CHECK(c_ae_handle >= 5 && c_awb_handle >= 5, "workers call handle");
    pthread_mutex_lock(&lk);
    CHECK(n_ae_set >= 4 && ae_set_cookie == COOKIE && ae_set_change == 1 && ae_set_it == 321, "AE set ioctl {cookie, 0, attr}");
    CHECK(n_awb_set >= 4 && awb_set_cookie == COOKIE && awb_set_r == 0x155, "AWB set ioctl {cookie, 0, attr}");
    pthread_mutex_unlock(&lk);
    CHECK(awb_info_r == 0x123, "AWB info = get buffer + 8");

    us_mode = 1;                        /* integration time in us: vendor divisor ((den * 1e6) / vts) * num */
    usleep(50000);
    d = ((1U * 1000000U) / 1125U) * 25U;
    pthread_mutex_lock(&lk);
    CHECK(ae_set_it == 40000U / d, "us -> lines like the vendor worker");
    pthread_mutex_unlock(&lk);
    us_mode = 0;

    tseries_algo_notify_fps();
    CHECK(c_ae_notify == 1, "FPS change notify of the active AE");

    CHECK(IMP_ISP_DisableSensor() == 0, "DisableSensor");
    CHECK(n_ae_close == 1 && n_awb_close == 1, "close ioctls 0x800456de / 0xc00456e4");
    CHECK(c_ae_close == 1 && c_awb_close == 1, "close callbacks");
    {
        int h = c_ae_handle;

        usleep(30000);
        CHECK(c_ae_handle == h, "worker stopped");
    }
    tseries_algo_notify_fps();
    CHECK(c_ae_notify == 1, "no notify after the close");
    CHECK(IMP_ISP_EnableSensor() == 0 && n_ae_open == 1 && n_awb_open == 1, "record gone after the close (vendor)");
    CHECK(IMP_ISP_DisableSensor() == 0 && n_ae_close == 1, "nothing to close");

    if (failures == 0)
        printf("%s custom algo: all checks passed\n",
#ifdef PLATFORM_T23
               "t23"
#else
               "t31"
#endif
        );
    return failures ? 1 : 0;
}
