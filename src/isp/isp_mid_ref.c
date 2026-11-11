/*
 * Test pattern mode 5 (ISP-middle picture): platform hooks for src/openimp_testpattern.c.
 * T23 and T31 (same MDNS block at ISP core 0x13307000: 0x7820 Y ref, 0x7824 stride, 0x7828 C ref) and T21
 * (MDNS block at 0x13301b00: 0x1b10 Y ref, 0x1b14 stride, 0x1b20 C ref, open-tx-isp T21 SET_BUF).
 * Other SoCs (T20 builds with PLATFORM_T21 too) do not use it; the weak hooks stay NULL there and mode 5
 * returns ENOTSUP.
 */
#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || (defined(PLATFORM_T21) && !defined(PLATFORM_T20))
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <sys/mman.h>

#include "isp_tseries_dev.h"
#include "dma_alloc.h"

/* The NCU ("ncubuf") buffer IMP_ISP_AddSensor allocates (isp_tseries.c): T23 sensor_alloc[0], other
 * T-series the word at 0xac of the ISP device object (as the vendor). */
static const IMPDMABufferInfo *mid_ncu(const ISPDevice *isp)
{
    if (isp == NULL)
        return NULL;
#if defined(PLATFORM_T23)
    return (const IMPDMABufferInfo *)isp->sensor_alloc[0];
#else
    return *(const IMPDMABufferInfo *const *)((const uint8_t *)isp + 0xac);
#endif
}

/* --- Test pattern mode 5: picture into the ISP's MDNS reference frame ----- */
/* The T23/T31 ISP core (0x13300000) has no register ioctl; like the vendor (EnableTuning keeps /dev/mem open)
 * the two register pages 0x7000/0x8000 are mapped through gISP->mem_fd.  Registers (open-tx-isp
 * tx_isp_t23_mdns_writers.inc, device probes P-A..P-C 2026-10-25): 0x7820 Y reference, 0x7824 stride,
 * 0x7828 chroma reference (= Y + stride * h), 0x799c luma / 0x7a6c chroma reference weights
 * (f_max | f_min << 8 | b_max << 16 | b_min << 24; the reference weight is min(f_min, b_max) / 255).
 * The driver rewrites the weights on every gain step.  The open-tx-isp driver has the module parameter
 * mdns_ref_hold (bitmask, default 0) that keeps the weight words (and, with bit 1/2, the fake/mv and r words
 * of the same blocks) at maximum while set; the mode then sets it once and writes no register.  Without the
 * parameter (older driver, vendor driver) the frame path writes the two weight words again each frame. */
#if defined(PLATFORM_T21)
/* T21: 0x1cdc luma / 0x1e38 chroma reference weight = max << 16 | min (8 bit each; the driver's
 * mdns_y/c_ref_wei_max/min); both at 0xff hold the reference. */
#define MID_PAGE 0x13301000u
#define MID_MAP  0x1000u
#define MID_REG_Y   0x1b10u
#define MID_REG_YST 0x1b14u
#define MID_REG_C   0x1b20u
#define MID_REG_WY  0x1cdcu
#define MID_REG_WC  0x1e38u
#define MID_WEIGHT_MAX 0x00ff00ffu
#else
#define MID_PAGE 0x13307000u
#define MID_MAP  0x2000u
#define MID_REG_Y   0x7820u
#define MID_REG_YST 0x7824u
#define MID_REG_C   0x7828u
#define MID_REG_WY  0x799cu
#define MID_REG_WC  0x7a6cu
/* All four fields at 0xff.  b_min = 0 (the first T23 value 0x00ffffff) leaves the T31 luma reference weight at
 * about 0.6 (live scene shows through, Y SSIM 0.72) and the T23 chroma at U/V 0.87/0.89; with b_min = b_max =
 * 0xff a still block keeps the reference (Y 0.89 on T31, U/V 0.97 on T23, devices 2026-11-11). */
#define MID_WEIGHT_MAX 0xffffffffu
#endif
#if defined(PLATFORM_T23)
/* T23: bit 0 alone leaves about 30 % live chroma in colour day mode (U/V 0.90): the chroma fake/mv (0x7a68) and
 * r (0x7a70) weights react to live colour.  Bits 1|2 hold them too: U/V 0.97 day and night (#340). */
#define MID_HOLD_VALUE "7"
#else
#define MID_HOLD_VALUE "1"
#endif

static volatile uint32_t *mid_regs;      /* MID_PAGE base: register r at (r - (MID_PAGE & 0xffff)) / 4 */
#if defined(PLATFORM_T23)
static const char *mid_hold_path = "/sys/module/tx_isp_t23/parameters/mdns_ref_hold";
#elif defined(PLATFORM_T31)
static const char *mid_hold_path = "/sys/module/tx_isp_t31/parameters/mdns_ref_hold";
#else
static const char *mid_hold_path = "/sys/module/tx_isp_t21/parameters/mdns_ref_hold";
#endif
static int mid_hold_set;                 /* the driver holds the weights (no zero-weight frames at gain steps) */
static uint32_t mid_save_y, mid_save_c;
static uint8_t *mid_ybuf;                /* virtual address of the Y reference plane */
static uint32_t mid_yphys, mid_cphys, mid_stride, mid_h;

static inline volatile uint32_t *mid_reg(uint32_t r)
{
    return mid_regs + ((r - (MID_PAGE & 0xffffu)) >> 2);
}

/* Write the driver's hold parameter; 0 on success, -1 if it does not exist or is not writable. */
static int mid_hold(const char *v)
{
    int fd = open(mid_hold_path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
}

/* Locate and validate the reference planes from the registers + the AddSensor NCU buffer. */
static int mid_geometry(uint32_t *yphys, uint32_t *cphys, uint32_t *stride, uint32_t *h, uint8_t **yvirt)
{
    ISPDevice *isp = gISP;
    const IMPDMABufferInfo *ncu = mid_ncu(isp);
    uint32_t y = *mid_reg(MID_REG_Y), c = *mid_reg(MID_REG_C), st = *mid_reg(MID_REG_YST);
    uint32_t hh;

    if (ncu == NULL || ncu->virt_addr == 0 || st == 0 || c <= y || (c - y) % st != 0) {
        errno = EIO;
        return -1;
    }
    hh = (c - y) / st;
    if (hh < 16 || hh > 4320 || st < 64 || st > 8192 || y < ncu->phys_addr ||
        (uint64_t)c + (uint64_t)st * hh / 2u > (uint64_t)ncu->phys_addr + ncu->size) {
        errno = EIO;
        return -1;
    }
    *yphys = y; *cphys = c; *stride = st; *h = hh;
    *yvirt = (uint8_t *)(uintptr_t)ncu->virt_addr + (y - ncu->phys_addr);
    return 0;
}

int openimp_tp_ispmid_on(uint32_t *w, uint32_t *h, uint32_t *stride)
{
    ISPDevice *isp = gISP;
    void *m;

    if (isp == NULL || isp->mem_fd <= 0 || mid_ncu(isp) == NULL) {
        errno = ENODEV;
        return -1;
    }
    if (mid_regs == NULL) {
        m = mmap(NULL, MID_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, isp->mem_fd, MID_PAGE);
        if (m == MAP_FAILED) {
            errno = ENODEV;
            return -1;
        }
        mid_regs = (volatile uint32_t *)m;
    }
    if (mid_geometry(&mid_yphys, &mid_cphys, &mid_stride, &mid_h, &mid_ybuf) != 0) {
        int err = errno;

        munmap((void *)mid_regs, MID_MAP);
        mid_regs = NULL;
        errno = err;
        return -1;
    }
    mid_save_y = *mid_reg(MID_REG_WY);
    mid_save_c = *mid_reg(MID_REG_WC);
    mid_hold_set = mid_hold(MID_HOLD_VALUE) == 0;
    *w = *stride = mid_stride;
    *h = mid_h;
    return 0;
}

int openimp_tp_ispmid_put(const uint8_t *nv12, uint32_t stride, uint32_t h)
{
    uint32_t y, c, st, hh;
    uint8_t *yv;

    if (mid_regs == NULL)
        return -1;
    /* Every frame, also with the driver hold: a hold driver older than 2026-11-11 holds 0x00ffffff (b_min 0),
     * which leaves the T23 chroma at U/V SSIM 0.87/0.89 instead of 0.97. */
    *mid_reg(MID_REG_WY) = MID_WEIGHT_MAX;
    *mid_reg(MID_REG_WC) = MID_WEIGHT_MAX;
    if (mid_geometry(&y, &c, &st, &hh, &yv) != 0 || st != stride || hh != h || y != mid_yphys)
        return -1;
    memcpy(yv, nv12, (size_t)st * hh * 3u / 2u);    /* chroma plane is contiguous behind Y */
    (void)DMA_RmemFlushCache(yv, st * hh * 3u / 2u, 1 /* WBACK */);
    return 0;
}

/* 1 when the picture must be stored mirrored: the T31 ISP applies its horizontal flip (IMP_ISP_Tuning_SetISPHflip)
 * behind MDNS, so a picture written as is comes out mirrored (device .26 2026-11-11, hflip=1 vflip=1: only the
 * horizontal flip acts on the injected picture).  T23: 0 (device proof 2026-10-25 not mirrored). */
#if defined(PLATFORM_T31)
int IMP_ISP_Tuning_GetISPHflip(int *pmode);
#endif
int openimp_tp_ispmid_hmirror(void)
{
#if defined(PLATFORM_T31)
    int m = 0;

    return IMP_ISP_Tuning_GetISPHflip(&m) == 0 && m != 0;
#else
    return 0;
#endif
}

void openimp_tp_ispmid_off(void)
{
    if (mid_regs == NULL)
        return;
    *mid_reg(MID_REG_WY) = mid_save_y;
    *mid_reg(MID_REG_WC) = mid_save_c;
    if (mid_hold_set) {
        (void)mid_hold("0");             /* the driver writes its computed weights again */
        mid_hold_set = 0;
    }
    munmap((void *)mid_regs, MID_MAP);
    mid_regs = NULL;
}
/* IMP_ISP_EnableTuning (review 2026-11-11 F8): a streamer that died in mode 5 left the driver hold set (the
 * parameter outlives the process); clear it unless this process holds it itself. */
void openimp_tp_ispmid_reset(void)
{
    char v[16] = "";
    int fd;
    ssize_t n;

    if (mid_hold_set)
        return;
    fd = open(mid_hold_path, O_RDONLY);
    if (fd < 0)
        return;
    n = read(fd, v, sizeof(v) - 1);
    close(fd);
    if (n > 0 && v[0] != '0') {
        (void)mid_hold("0");
        syslog(LOG_WARNING, "libimp: MDNS reference hold left set (%.*s) by an earlier process: cleared", (int)strcspn(v, "\n"), v);
    }
}
#endif /* PLATFORM_T23 || PLATFORM_T31 || PLATFORM_T21 */
