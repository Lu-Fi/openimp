/*
 * Host test of the T21 / T30 ISP flips (openimp #948): like the vendor T21
 * 1.0.33 (SetISPHflip 0x63534, SetISPVflip 0x6391c) and T30 1.0.5
 * (SetISPHflip 0xd7e14, SetISPVflip 0xd822c, SetISPHVflip 0xd8658) a flip
 * needs the tuning session (else -1, no driver call); with Temper in AUTO it
 * switches Temper to MANUAL, sends the flip, waits 120 ms and restores AUTO.
 * Build with -DPLATFORM_T21 or -DPLATFORM_T30 (isp_tseries.c, 32-bit ABI
 * asserts off, fake ioctl).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/globals.h"
#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

#define S_CTRL 0xc008561cUL
#define G_CTRL 0xc008561bUL
#define TEMPER_ATTR 0x8000083
#define T21_TYPE 0x8000167

enum { EV_MAN = 1, EV_REST, EV_FLIP };
struct ev { int kind; int32_t val; double t; };
static struct ev ev_[32];
static int n_ev, n_calls;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void add(int kind, int32_t val)
{
    if (n_ev < 32) {
        ev_[n_ev].kind = kind;
        ev_[n_ev].val = val;
        ev_[n_ev].t = now_ms();
        n_ev++;
    }
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    int32_t *p;
    __builtin_va_list ap;

    (void)fd;
    __builtin_va_start(ap, req);
    p = __builtin_va_arg(ap, int32_t *);
    __builtin_va_end(ap);
    n_calls++;
    if (req == G_CTRL) {
        p[1] = p[0] == T21_TYPE ? 1 : 0;   /* Temper AUTO */
        return 0;
    }
    if (req == S_CTRL) {
        if (p[0] == T21_TYPE) {
            add(p[1] == 2 ? EV_MAN : EV_REST, p[1]);
        } else if (p[0] == 0x980914 || p[0] == 0x980915) {
            add(EV_FLIP, p[0] * 2 + (p[1] != 0));
        }
        return 0;
    }
#if defined(PLATFORM_T30)
    /* tuning {dir, sub, ptr}; T21 uses value requests (no pointer) */
    if (p[1] == TEMPER_ATTR) {
        IMPISPTemperDenoiseAttr *a = *(IMPISPTemperDenoiseAttr **)(void *)&p[2];

        if (p[0] == 1) {
            memset(a, 0, sizeof(*a));
            a->type = IMPISP_TEMPER_AUTO;
        } else {
            add(a->type == IMPISP_TEMPER_MANUAL ? EV_MAN : EV_REST, a->type);
        }
    }
#endif
    return 0;
}

ISPDevice *gISP;
static ISPDevice dev;
static uint8_t tuning_obj[0x48];
static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void check_guard(const char *what, int nflips)
{
    int man = -1, rest = -1, first = -1, last = -1, i, flips = 0;
    char m[100];

    for (i = 0; i < n_ev; i++) {
        if (ev_[i].kind == EV_MAN && man < 0) man = i;
        if (ev_[i].kind == EV_REST) rest = i;
        if (ev_[i].kind == EV_FLIP) { flips++; if (first < 0) first = i; last = i; }
    }
    snprintf(m, sizeof(m), "%s: Temper MANUAL before the flip", what);
    CHECK(man >= 0 && first > man, m);
    snprintf(m, sizeof(m), "%s: AUTO restored after the flip(s)", what);
    CHECK(rest > last, m);
    snprintf(m, sizeof(m), "%s: restore >= 120 ms after the flip", what);
    CHECK(rest > last && ev_[rest].t - ev_[last].t >= 119.0, m);
    snprintf(m, sizeof(m), "%s: %d flip(s), one guard", what, nflips);
    CHECK(flips == nflips, m);
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
#if defined(PLATFORM_T30)
    CHECK(IMP_ISP_Tuning_SetISPHVflip(1, 1) == -1, "HVflip without tuning -> -1");
#endif
    CHECK(n_calls == 0, "no driver call without tuning");

    dev.tuning_state = 2;
    n_ev = 0;
    CHECK(IMP_ISP_Tuning_SetISPHflip(IMPISP_TUNING_OPS_MODE_ENABLE) == 0, "Hflip ok");
    check_guard("Hflip", 1);
    n_ev = 0;
    CHECK(IMP_ISP_Tuning_SetISPVflip(IMPISP_TUNING_OPS_MODE_DISABLE) == 0, "Vflip ok");
    check_guard("Vflip", 1);
#if defined(PLATFORM_T30)
    n_ev = 0;
    CHECK(IMP_ISP_Tuning_SetISPHVflip(1, 0) == 0, "HVflip ok");
    check_guard("HVflip", 2);
#endif
    if (failures == 0)
        printf("flip guard (T21/T30): all checks passed\n");
    return failures ? 1 : 0;
}
