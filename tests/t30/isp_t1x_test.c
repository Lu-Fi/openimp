/*
 * T20/T21 (and T10, which runs the T20 libimp) ISP tuning calls that
 * reach the driver: WaitFrame (0x8000162).
 * src/isp/isp_tseries.c against a fake tuning ioctl (--wrap=ioctl).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

ISPDevice *gISP;

/* stubs for the rest of libimp */
int IMP_Log_Get_Option(void) { return 0; }
void imp_log_fun(int level, int option, int type, ...) { (void)level; (void)option; (void)type; }
void *IMP_Alloc(void *info, int size, const char *owner) { (void)info; (void)size; (void)owner; return NULL; }
int DMA_FreePhys(uint32_t phys) { (void)phys; return 0; }

#define TUNING_FD 77
#define TUNING_IOCTL 0xc00c56c6UL

typedef struct {
    int32_t cmd;
    int32_t subcmd;
    void *ptr;
} PtrReq;

static int failures;
static int calls;
static int32_t last_cmd, last_subcmd;
static int driver_ret;
static uint64_t driver_cnt;
static uint32_t seen_timeout;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, \
    __LINE__, #c); failures++; } } while (0)

int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    PtrReq *req;

    va_start(ap, request);
    req = va_arg(ap, PtrReq *);
    va_end(ap);
    if (fd != TUNING_FD || request != TUNING_IOCTL) {
        errno = EINVAL;
        return -1;
    }
    calls++;
    last_cmd = req->cmd;
    last_subcmd = req->subcmd;
    if (req->subcmd == 0x8000162) {
        /* driver struct isp_frame_done_info: u32 timeout, (pad), u64 cnt,
         * int reserved = 0x18 bytes; the count is written back also when
         * the wait timed out */
        uint32_t *w = req->ptr;

        seen_timeout = w[0];
        w[2] = (uint32_t)driver_cnt;
        w[3] = (uint32_t)(driver_cnt >> 32);
    }
    if (driver_ret) {
        errno = -driver_ret;
        return -1;
    }
    return 0;
}

static void test_wait_frame(void)
{
    IMPISPWaitFrameAttr attr;

    /* tuning not running: -1, nothing sent, count untouched */
    gISP->tuning_state = 1;
    memset(&attr, 0, sizeof(attr));
    attr.timeout = 100;
    attr.cnt = 5;
    calls = 0;
    CHECK(IMP_ISP_Tuning_WaitFrame(&attr) == -1);
    CHECK(calls == 0 && attr.cnt == 5);
    gISP->tuning_state = 2;
    CHECK(IMP_ISP_Tuning_WaitFrame(NULL) == -1);

    /* a frame end: the 64-bit count comes back */
    driver_ret = 0;
    driver_cnt = 0x100000002ULL;
    attr.timeout = 1000;
    attr.cnt = 0;
    calls = 0;
    CHECK(IMP_ISP_Tuning_WaitFrame(&attr) == 0);
    CHECK(calls == 1 && last_cmd == 1 && last_subcmd == 0x8000162);
    CHECK(seen_timeout == 1000);
    CHECK(attr.cnt == 0x100000002ULL);

    /* timeout: the error, and the count as the driver reported it */
    driver_ret = -ETIMEDOUT;
    driver_cnt = 7;
    CHECK(IMP_ISP_Tuning_WaitFrame(&attr) != 0);
    CHECK(attr.cnt == 7);
    driver_ret = 0;
}

int main(void)
{
    static ISPDevice isp;

    gISP = &isp;
    isp.tuning_fd = TUNING_FD;
    isp.tuning = &isp;
    isp.tuning_state = 2;
    test_wait_frame();
    if (failures) {
        fprintf(stderr, "isp_t1x_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("isp_t1x_test: ok\n");
    return 0;
}
