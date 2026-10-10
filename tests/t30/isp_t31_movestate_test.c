/*
 * T31 IMP_ISP_Tuning_Enable/DisableMovestate (vendor libimp 1.1.x logic):
 * src/isp/isp_tseries.c with PLATFORM_T31 against a fake tuning ioctl.
 *  - -1 without tuning; 0 once past the checks;
 *  - Enable reads the 0x70-byte AE block of CID 0x800002c, then sends a
 *    block with the day or night limits (byte positions of the vendor) and
 *    the enable bytes;
 *  - Disable resends the captured block with the enable bytes only when the
 *    running mode is unchanged, and always asks for an IDR on channel 0.
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

void openimp_t31_movestate_block(uint8_t ae[0x70], const uint8_t cur[0x70],
                                 const uint32_t c[14], uint32_t mode);

int IMP_ISP_Tuning_EnableMovestate(void);
int IMP_ISP_Tuning_DisableMovestate(void);

ISPDevice *gISP;

int IMP_Log_Get_Option(void) { return 0; }
void imp_log_fun(int level, int option, int type, ...) { (void)level; (void)option; (void)type; }
void *IMP_Alloc(void *info, int size, const char *owner) { (void)info; (void)size; (void)owner; return NULL; }
int DMA_FreePhys(uint32_t phys) { (void)phys; return 0; }
static int idr_calls, idr_chn = -1;
int openimp_video_drop_set(void (*cb)(void)) { (void)cb; return 0; }
int IMP_Encoder_RequestIDR(int chn) { idr_calls++; idr_chn = chn; return 0; }

#define TUNING_FD 77
#define TUNING_IOCTL 0xc00c56c6UL

typedef struct { int32_t cmd, subcmd; void *ptr; } PtrReq;

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int32_t running_mode;
static uint8_t drv_ae[0x70];       /* what the driver returns on get */
static uint8_t sent[8][0x70];
static int sets, gets;

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
    if ((uint32_t)req->subcmd == 0x800002cu) {
        if (req->cmd == 0) {
            if (sets < 8)
                memcpy(sent[sets], req->ptr, 0x70);
            sets++;
        } else {
            gets++;
            memcpy(req->ptr, drv_ae, 0x70);
        }
        return 0;
    }
    {   /* running mode (value request) */
        int32_t *w = (int32_t *)req;

        if (w[0] == 1)
            w[2] = running_mode;
        else
            running_mode = w[2];
        return 0;
    }
}

int main(void)
{
    static ISPDevice isp;
    uint32_t c[14];
    uint8_t cur[0x70], ae[0x70];
    int i;

    for (i = 0; i < 14; i++)
        c[i] = 0x10u + (uint32_t)i;
    memset(cur, 0, sizeof(cur));

    /* block builder: night (mode 1) */
    openimp_t31_movestate_block(ae, cur, c, 1);
    CHECK(ae[11] == 1 && ae[46] == c[1] && ae[44] == c[5] && ae[45] == c[4] &&
          ae[35] == c[9] && ae[36] == c[8] && ae[38] == c[13] &&
          ae[39] == c[12]);
    CHECK(ae[71] && ae[101] && ae[99] && ae[100] && ae[90] && ae[91] &&
          ae[93] && ae[94] && !ae[82] && !ae[10] && !ae[26] && !ae[43]);
    /* day (mode 0): it * 3/5, gain ratio, extra enables */
    cur[26] = 100; cur[27] = 0;            /* it 100 -> 60 */
    cur[28] = 2; cur[29] = 30; cur[32] = 6; cur[33] = 2;  /* div = 2*30/30=2 */
    openimp_t31_movestate_block(ae, cur, c, 0);
    CHECK(ae[26] == 60 && ae[27] == 0);
    CHECK(ae[43] == (uint8_t)((6 * 2) / 2 + 1));
    CHECK(ae[11] && ae[82] && ae[10] && ae[70] && ae[98]);
    CHECK(ae[46] == c[0] && ae[44] == c[3] && ae[45] == c[2] &&
          ae[35] == c[7] && ae[36] == c[6] && ae[38] == c[11] &&
          ae[39] == c[10]);
    /* zero divisor does not trap */
    memset(cur, 0, sizeof(cur));
    openimp_t31_movestate_block(ae, cur, c, 0);
    CHECK(ae[43] == 1);

    /* no tuning */
    gISP = NULL;
    CHECK(IMP_ISP_Tuning_EnableMovestate() == -1);
    CHECK(IMP_ISP_Tuning_DisableMovestate() == -1 && idr_calls == 0);
    gISP = &isp;
    isp.tuning_fd = TUNING_FD;
    isp.tuning = &isp;
    isp.tuning_state = 2;

    /* Enable (day), then Disable in the same mode */
    running_mode = 0;
    memset(drv_ae, 0, sizeof(drv_ae));
    drv_ae[26] = 50; drv_ae[28] = 4; drv_ae[29] = 30; drv_ae[32] = 3;
    drv_ae[33] = 1;
    CHECK(IMP_ISP_Tuning_EnableMovestate() == 0);
    CHECK(gets == 1 && sets == 1);
    CHECK(sent[0][26] == 30 && sent[0][11] == 1);
    CHECK(IMP_ISP_Tuning_DisableMovestate() == 0);
    CHECK(sets == 2 && sent[1][26] == 50 && sent[1][71] == 1 &&
          sent[1][94] == 1 && sent[1][28] == 4);
    CHECK(idr_calls == 1 && idr_chn == 0);

    /* mode changed in between: nothing is resent, the IDR still goes */
    CHECK(IMP_ISP_Tuning_EnableMovestate() == 0 && sets == 3);
    running_mode = 1;
    CHECK(IMP_ISP_Tuning_DisableMovestate() == 0);
    CHECK(sets == 3 && idr_calls == 2);

    if (failures) {
        fprintf(stderr, "isp_t31_movestate_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("isp_t31_movestate_test: ok\n");
    return 0;
}
