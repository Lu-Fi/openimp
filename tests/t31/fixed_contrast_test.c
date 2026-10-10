/*
 * Host test of IMP_ISP_SetFixedContraster on T31: the argument is a
 * pointer to IMPISPFixedContrastAttr (not a mode) that goes to the driver
 * as tuning control 0x8000102, behind the same checks as the vendor
 * libimp (tuning session open, attr not NULL).
 *
 * Runs src/isp/isp_tseries.c, built without the 32-bit ABI asserts (see
 * the Makefile), against a fake ioctl().  The attr lives in static data so
 * that its address fits the 32-bit request field (non-PIE link).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/globals.h"
#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

extern int IMP_ISP_SetFixedContraster(void *attr);

int __real_ioctl(int fd, unsigned long req, ...);
static int ioctl_calls;
static int ioctl_fd = -1;
static int32_t req_cmd, req_sub;
static uint32_t req_val;

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    int32_t *p;
    __builtin_va_list ap;

    (void)req;
    __builtin_va_start(ap, req);
    p = __builtin_va_arg(ap, int32_t *);
    __builtin_va_end(ap);
    ioctl_calls++;
    ioctl_fd = fd;
    req_cmd = p[0];
    req_sub = p[1];
    req_val = (uint32_t)p[2];
    return 0;
}

ISPDevice *gISP;

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static uint8_t attr[4] = { 1, 40, 200, 0 };
static ISPDevice dev;
static uint8_t tuning_obj[16];

int main(void)
{
    gISP = NULL;
    CHECK(IMP_ISP_SetFixedContraster(attr) == -1, "no ISP device");

    memset(&dev, 0, sizeof(dev));
    gISP = &dev;
    CHECK(IMP_ISP_SetFixedContraster(attr) == -1, "no tuning object");

    dev.tuning = tuning_obj;
    dev.tuning_fd = 77;
    dev.tuning_state = 1;
    CHECK(IMP_ISP_SetFixedContraster(attr) == -1, "tuning not enabled");

    dev.tuning_state = 2;
    CHECK(IMP_ISP_SetFixedContraster(NULL) == -1, "NULL attr");
    CHECK(ioctl_calls == 0, "failed checks never reach the driver");

    CHECK(IMP_ISP_SetFixedContraster(attr) == 0, "valid call");
    CHECK(ioctl_calls == 1 && ioctl_fd == 77, "one ioctl on the tuning fd");
    CHECK(req_cmd == 0 && req_sub == 0x8000102, "set request, control 0x8000102");
    CHECK(req_val == (uint32_t)(uintptr_t)attr, "attr pointer passed as value");

    if (failures == 0)
        printf("t31 fixed contrast: all checks passed\n");
    return failures ? 1 : 0;
}
