/* Host test of the T23 IVDC direct mode (src/t23/openimp_t23_ivdc.c,
 * openimp #767): misc-ivdc ioctl sequence, ring size, frame detection, the
 * vendor drop rule (modes 2/3) and the mode 1 node values.  open/ioctl/
 * close are wrapped, the IVDC registers come from a fake reader. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma_alloc.h"
#include "t23/openimp_t23_ivdc.h"

#define FAKE_FD 77

static int fake_mode = 1;
static uint32_t fake_line = 300;
static uint32_t paddr_seen;
static int ioctls;
static uint32_t reg_ovf, reg_isp, reg_vpu;
static int fails;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #c); fails++; } } while (0)

int __wrap_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, "/dev/misc-ivdc") == 0)
        return FAKE_FD;
    errno = ENOENT;
    return -1;
}

int __real_close(int fd);
int __wrap_close(int fd)
{
    return fd == FAKE_FD ? 0 : __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    uint32_t *arg;

    va_start(ap, req);
    arg = va_arg(ap, uint32_t *);
    va_end(ap);
    if (fd != FAKE_FD)
        return -1;
    ioctls++;
    switch ((uint32_t)req) {
    case T23_IVDC_GET_DIRECT: *arg = (uint32_t)fake_mode; return 0;
    case T23_IVDC_GET_MEM_LINE: *arg = fake_line; return 0;
    case T23_IVDC_SET_PADDR: paddr_seen = *arg; return 0;
    }
    return -1;
}

static int ring_size;
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    (void)tag;
    memset(info, 0, sizeof(*info));
    ring_size = size;
    info->phys_addr = 0x01eb1400u;
    info->virt_addr = 0x77000000u;
    return 0;
}

static int freed;
int DMA_FreePhys(uint32_t phys)
{
    freed = phys == 0x01eb1400u;
    return 0;
}

static int fake_reg(uint32_t addr, uint32_t *val)
{
    switch (addr) {
    case 0x1320002cu: *val = reg_ovf; return 0;
    case 0x13200208u: *val = reg_isp; return 0;
    case 0x132003a8u: *val = reg_vpu; return 0;
    }
    return -1;
}

static void make_frame(uint8_t *f, uint32_t ovf, uint32_t thr, int64_t ts)
{
    uint32_t direct = T23_IVDC_SRC_Y;

    memset(f, 0, 0x428);
    memcpy(f + 0x20, &direct, 4);
    memcpy(f + 0x30, &ts, 8);
    memcpy(f + T23_FRAME_OVERFLOW, &ovf, 4);
    memcpy(f + T23_FRAME_THRESHOLD, &thr, 4);
    f[T23_FRAME_TYPE] = 3;
}

int main(void)
{
    static uint8_t frame[0x428];
    uint32_t ovf, line, thr;
    uint64_t ms;

    OpenIMP_T23_IvdcSetRegReader(fake_reg);

    /* lead, vendor do_channel_process: same wrap bit / wrapped */
    CHECK(OpenIMP_T23_IvdcLead(120u << 16, 100u, 300u) == 20u);
    CHECK(OpenIMP_T23_IvdcLead((0x8000u | 10u) << 16, 290u, 300u) == 20u);
    CHECK(OpenIMP_T23_IvdcLead(10u << 16, 0x8000u | 290u, 300u) == 20u);

    /* direct_mode=0: GET_DIRECT 0, nothing else changes (no ring, no
     * IVDC frames, the T21 EMC layout stays: IvdcMode 0) */
    fake_mode = 0;
    CHECK(OpenIMP_T23_IvdcCreateChn(0) == 0 && ioctls == 1);
    CHECK(OpenIMP_T23_IvdcMode() == 0 && !OpenIMP_T23_IvdcChnDirect(0));
    CHECK(OpenIMP_T23_IvdcEnableChn(0, 1280, 720) == 0 && ioctls == 1 &&
          ring_size == 0 && paddr_seen == 0);
    OpenIMP_T23_IvdcDestroyChn(0);
    fake_mode = 1;
    ioctls = 0;

    /* channel 1 never asks the IVDC; channel 0 does (GET_DIRECT) */
    CHECK(OpenIMP_T23_IvdcCreateChn(1) == 0 && ioctls == 0);
    CHECK(OpenIMP_T23_IvdcCreateChn(0) == 0 && ioctls == 1);
    CHECK(OpenIMP_T23_IvdcChnDirect(0) == 1);
    CHECK(OpenIMP_T23_IvdcMode() == 1);
    CHECK(OpenIMP_T23_IvdcChnDirect(1) == 0);
    CHECK(OpenIMP_T23_IvdcThreshold(0) == T23_IVDC_THRESHOLD);

    /* EnableChn: GET_MEM_LINE, ring align256(w) * lines * 3 / 2, SET_PADDR */
    CHECK(OpenIMP_T23_IvdcEnableChn(0, 1280, 720) == 0);
    CHECK(ring_size == 1280 * 300 * 3 / 2);
    CHECK(paddr_seen == 0x01eb1400u);
    CHECK(OpenIMP_T23_IvdcMemLine() == 300u);
    OpenIMP_T23_IvdcDisableChn(0);
    CHECK(freed);
    CHECK(OpenIMP_T23_IvdcEnableChn(0, 1000, 720) == 0);  /* 1024 aligned */
    CHECK(ring_size == 1024 * 300 * 3 / 2);
    OpenIMP_T23_IvdcDisableChn(0);

    /* frame detection */
    make_frame(frame, 0, 300, 5000000);
    CHECK(OpenIMP_T23_IvdcFrame(frame));
    frame[T23_FRAME_TYPE] = 0;
    CHECK(!OpenIMP_T23_IvdcFrame(frame));

    /* mode 1: no drop check, zero node values (vendor "mode - 2 < 2") */
    make_frame(frame, 0, 300, 5000000);
    reg_ovf = 5; reg_isp = 400u << 16; reg_vpu = 0;
    CHECK(OpenIMP_T23_IvdcCheck(frame, &ovf, &line, &thr, &ms) == 0);
    CHECK(ovf == 0 && line == 0 && thr == 0 && ms == 0);
    OpenIMP_T23_IvdcDestroyChn(0);

    /* mode 2: overflow and lead drop, else the node values */
    fake_mode = 2;
    CHECK(OpenIMP_T23_IvdcCreateChn(0) == 0);
    CHECK(OpenIMP_T23_IvdcEnableChn(0, 1280, 720) == 0);
    make_frame(frame, 3, 300, 5000000);
    reg_ovf = 4; reg_isp = 10u << 16; reg_vpu = 0;
    CHECK(OpenIMP_T23_IvdcCheck(frame, &ovf, &line, &thr, &ms) == -1);
    reg_ovf = 3; reg_isp = 301u << 16;
    CHECK(OpenIMP_T23_IvdcCheck(frame, &ovf, &line, &thr, &ms) == -1);
    reg_isp = 300u << 16;
    CHECK(OpenIMP_T23_IvdcCheck(frame, &ovf, &line, &thr, &ms) == 0);
    CHECK(ovf == 3 && line == 300 && thr == 300 && ms == 5000);
    CHECK(OpenIMP_T23_IvdcSetThreshold(0, 50) == 0 &&
          OpenIMP_T23_IvdcThreshold(0) == 50);
    OpenIMP_T23_IvdcDestroyChn(0);
    CHECK(OpenIMP_T23_IvdcChnDirect(0) == 0);

    if (fails) {
        fprintf(stderr, "t23 ivdc test: %d failures\n", fails);
        return 1;
    }
    printf("t23 ivdc test: ok\n");
    return 0;
}
