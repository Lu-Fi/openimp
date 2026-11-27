/*
 * Host test of the T10/T20 ISP flips (openimp #944): like the vendor 3.12.0
 * library (SetISPHflip 0x5ceec, SetISPVflip 0x5d2a0, SetISPHVflip 0x5d684)
 * a flip needs the tuning session (else -1, no driver call); with Temper in
 * AUTO it switches Temper to MANUAL strength 0x28, sends the flip, waits
 * 120 ms and restores AUTO.  The wait is what puts an app's later image
 * settings (HiLightDepress) after the driver's IQ reload at the first frame
 * end after EnableTuning.  HVflip guards both flips once.
 *
 * Runs src/isp/isp_tseries.c (32-bit ABI asserts off, see the Makefile)
 * against a fake ioctl().
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/globals.h"
#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

#define S_CTRL 0xc008561cUL
#define TUNING 0xc00c56c6UL

struct call { unsigned long req; int32_t a, b; double t; };
static struct call log_[64];
static int n_calls;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    int32_t *p;
    __builtin_va_list ap;

    (void)fd;
    __builtin_va_start(ap, req);
    p = __builtin_va_arg(ap, int32_t *);
    __builtin_va_end(ap);
    if (n_calls < 64) {
        log_[n_calls].req = req;
        /* S_CTRL {id, value}; tuning {dir, id, value/pointer} */
        log_[n_calls].a = req == TUNING ? p[1] : p[0];
        log_[n_calls].b = req == TUNING ? p[0] : p[1];
        log_[n_calls].t = now_ms();
    }
    n_calls++;
    return 0;
}

ISPDevice *gISP;

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static ISPDevice dev;
static uint8_t tuning_obj[0x48];

static int find(unsigned long req, int32_t a, int from)
{
    int i;

    for (i = from; i < n_calls && i < 64; i++)
        if (log_[i].req == req && log_[i].a == a)
            return i;
    return -1;
}

static int count(unsigned long req, int32_t a)
{
    int i, n = 0;

    for (i = 0; i < n_calls && i < 64; i++)
        n += log_[i].req == req && log_[i].a == a;
    return n;
}

/* one guarded flip: Temper MANUAL (type 2 + attr 0x8000083) before the flip,
 * AUTO (1) restored >= 120 ms after it */
static void check_guard(const char *what, int32_t flip_id, int32_t value)
{
    int man = find(S_CTRL, 0x98e90c, 0);
    int attr = find(TUNING, 0x8000083, 0);
    int flip = find(S_CTRL, flip_id, 0);
    int rest = flip >= 0 ? find(S_CTRL, 0x98e90c, flip + 1) : -1;
    char m[160];

    snprintf(m, sizeof(m), "%s: Temper MANUAL before the flip", what);
    CHECK(man >= 0 && attr > man && flip > attr && log_[man].b == 2, m);
    snprintf(m, sizeof(m), "%s: flip value", what);
    CHECK(flip >= 0 && log_[flip].b == value, m);
    snprintf(m, sizeof(m), "%s: AUTO restored after the flip", what);
    CHECK(rest > flip && log_[rest].b == 1, m);
    snprintf(m, sizeof(m), "%s: restore >= 120 ms after the flip", what);
    CHECK(rest > flip && log_[rest].t - log_[flip].t >= 119.0, m);
}

int main(void)
{
    memset(&dev, 0, sizeof(dev));
    gISP = &dev;
    dev.tuning = tuning_obj;
    dev.tuning_fd = 55;
    dev.tuning_state = 1;
    CHECK(IMP_ISP_Tuning_SetISPHflip(IMPISP_TUNING_OPS_MODE_ENABLE) == -1, "Hflip without tuning -> -1");
    CHECK(IMP_ISP_Tuning_SetISPVflip(IMPISP_TUNING_OPS_MODE_ENABLE) == -1, "Vflip without tuning -> -1");
    CHECK(IMP_ISP_Tuning_SetISPHVflip(IMPISP_TUNING_OPS_MODE_ENABLE,
                                      IMPISP_TUNING_OPS_MODE_ENABLE) == -1, "HVflip without tuning -> -1");
    CHECK(n_calls == 0, "no driver call without tuning");

    dev.tuning_state = 2;
    n_calls = 0;
    CHECK(IMP_ISP_Tuning_SetISPHflip(IMPISP_TUNING_OPS_MODE_ENABLE) == 0, "Hflip ok");
    check_guard("Hflip", 0x980914, 1);

    n_calls = 0;
    CHECK(IMP_ISP_Tuning_SetISPVflip(IMPISP_TUNING_OPS_MODE_DISABLE) == 0, "Vflip ok");
    check_guard("Vflip", 0x980915, 0);

    n_calls = 0;
    CHECK(IMP_ISP_Tuning_SetISPHVflip(IMPISP_TUNING_OPS_MODE_ENABLE,
                                      IMPISP_TUNING_OPS_MODE_DISABLE) == 0, "HVflip ok");
    check_guard("HVflip", 0x980914, 1);
    CHECK(count(S_CTRL, 0x980915) == 1 && count(TUNING, 0x8000083) == 1 &&
          count(S_CTRL, 0x98e90c) == 2, "HVflip: one Temper guard for both flips");
    {
        int v = find(S_CTRL, 0x980915, 0), r = find(S_CTRL, 0x98e90c, v + 1);
        CHECK(v >= 0 && r > v && log_[v].b == 0, "HVflip: Vflip 0 inside the guard");
    }

    if (failures == 0)
        printf("t20 flip: all checks passed\n");
    return failures ? 1 : 0;
}
