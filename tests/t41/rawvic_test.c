/* T41 IMP_ISP_SetVicDoneCbFunc / IMP_ISP_RAW_RwControl (src/t40/openimp_t41_rawvic.c)
 * against a fake ISP driver: vendor 1.2.5 return codes, ioctl numbers and
 * payloads, one callback per fake frame, the RAW frame address round trip
 * (get -> callback vaddr -> set), and that cancel + join returns even while
 * the worker sleeps in the driver with no frame coming (host test, needs
 * the vendor T41 1.2.0 headers via T41_HEADERS). */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_isp.h>

#include "dma_alloc.h"

void OpenIMP_P1_VicDoneStop(void);
void OpenIMP_P1_RawStop(void);

/* tuning request as the library builds it (word size of the host) */
typedef struct { int32_t vinum, is_get; uint32_t id; uintptr_t payload; } Req;

/* ---- fake P1 / DMA (non-PIE: rmem below 4 GiB, IMPDMABufferInfo is 32-bit) ---- */
static int isp_open = 1, tuning_on = 1, alloc_fail, enable_fail, ctl_fail;
static volatile int block_forever;
static int vic_arm[32], vic_arm_n;
static uint32_t ctl_seen[8][7];
static int ctl_n;
static uintptr_t freed;
static char rmem[4 * 64 * 32 * 2];
#define PHYS 0x03000000U

int OpenIMP_P1_TuningEnabled(void) { return tuning_on; }
int OpenIMP_P1_IspIsOpen(void) { return isp_open; }
int OpenIMP_P1_DupFd(int tuning) { (void)tuning; return open("/dev/null", O_RDONLY); }

int OpenIMP_P1_IspIOCtlOpened(uint32_t command, void *argument)
{
    assert(command == 0xc0045431U);
    vic_arm[vic_arm_n++] = *(int32_t *)argument;
    if (enable_fail && *(int32_t *)argument) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int OpenIMP_P1_TuningWaitIOCtl(uint32_t command, void *argument)
{
    const Req *req = argument;

    assert(command == 0xc0105435U);
    assert(req->is_get == 0 && req->id == 0x080000a7U);
    memcpy(ctl_seen[ctl_n++], (void *)req->payload, 28);
    if (ctl_fail) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    assert(!strcmp(tag, "raw"));
    if (alloc_fail || size <= 0 || (size_t)size > sizeof(rmem))
        return -1;
    info->virt_addr = (uint32_t)(uintptr_t)rmem;
    info->phys_addr = PHYS;
    info->size = (uint32_t)size;
    return 0;
}

int IMP_Free(uintptr_t address) { freed = address; return 0; }

/* test pattern mode 6: ISP core page (colour filter order at 0x88) and the
 * cache write-back of the drawn frame */
volatile uint32_t rawvic_test_isp[1024];
static volatile int flushes;
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    assert(virt == (void *)rmem && size == 64 * 32 && dir == 1);
    flushes++;
    return 0;
}
int openimp_tp_ispin_set(uint32_t pattern);

int32_t IMP_ISP_Tuning_GetSensorAttr(IMPVI_NUM num, IMPISPSENSORAttr *attr)
{
    (void)num;
    attr->width = 64;
    attr->height = 32;
    return 0;
}

/* ---- fake driver ioctls of the workers ---- */
static volatile int vic_frames, raw_gets, raw_sets;
static volatile uint32_t raw_set_addr[64];
static uint32_t raw_frame = 64 * 32 * 2, raw_nrvbs = 3;

static void frame_wait(void)
{
    struct timespec ts = {0, 2 * 1000 * 1000};

    if (block_forever)
        poll(NULL, 0, -1);              /* no frame ever comes */
    nanosleep(&ts, NULL);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    (void)fd;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if ((uint32_t)request == 0xc0045432U) {
        assert(*(int32_t *)arg == -1);
        frame_wait();
        *(int32_t *)arg = 1;
        vic_frames++;
        return 0;
    }
    if ((uint32_t)request == 0xc0105435U) {
        Req *req = arg;
        uint32_t *paddr = (uint32_t *)req->payload;

        assert(req->vinum == 0 && req->id == 0x080000a8U);
        if (req->is_get == 1) {
            frame_wait();
            *paddr = PHYS + (raw_gets % raw_nrvbs) * raw_frame;
            raw_gets++;
        } else {
            raw_set_addr[raw_sets % 64] = *paddr;
            raw_sets++;
        }
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static volatile int cb_count;
static void vic_cb(void) { cb_count++; }

static volatile int raw_cb_count;
static int32_t raw_cb(IMPISPRawRwInfo *info)
{
    uint32_t want = (uint32_t)(uintptr_t)rmem + (raw_cb_count % raw_nrvbs) * raw_frame;

    assert(info->vinum == 0 && info->width == 64 && info->height == 32);
    assert(info->dtype == TISP_RAW_DTYPE_RAW16);
    assert(info->vaddr == want);
    memset((void *)(uintptr_t)info->vaddr, 0, 16);  /* the callback may edit the frame */
    raw_cb_count++;
    return 0;
}

static void wait_count(volatile int *v, int n)
{
    int i;

    for (i = 0; i < 2000 && *v < n; i++)
        usleep(1000);
    assert(*v >= n);
}

int main(void)
{
    IMPISPRawRwAttr a;
    int i;

    /* ---- VIC done ---- */
    tuning_on = 0;
    assert(IMP_ISP_SetVicDoneCbFunc(vic_cb) == -4091);
    tuning_on = 1;
    enable_fail = 1;
    assert(IMP_ISP_SetVicDoneCbFunc(vic_cb) == -4090);
    enable_fail = 0;
    vic_arm_n = 0;
    assert(IMP_ISP_SetVicDoneCbFunc(vic_cb) == 0);
    assert(vic_arm_n == 1 && vic_arm[0] == 1);
    wait_count(&cb_count, 20);
    assert(cb_count <= vic_frames);
    assert(IMP_ISP_SetVicDoneCbFunc(vic_cb) == -4091);  /* already on */
    block_forever = 1;                  /* stream stopped: no frame comes */
    usleep(20 * 1000);
    OpenIMP_P1_VicDoneStop();           /* must not hang */
    block_forever = 0;
    assert(vic_arm_n == 2 && vic_arm[1] == 0);
    OpenIMP_P1_VicDoneStop();           /* off: nothing */
    assert(vic_arm_n == 2);
    for (i = 0; i < 5; i++) {           /* enable/disable cycles */
        assert(IMP_ISP_SetVicDoneCbFunc(i & 1 ? NULL : vic_cb) == 0);  /* NULL cb: skipped */
        usleep(10 * 1000);
        OpenIMP_P1_VicDoneStop();
    }
    assert(vic_arm_n == 12);

    /* ---- RAW ring ---- */
    memset(&a, 0, sizeof(a));
    a.mode = IMPISP_OPS_MODE_ENABLE;
    a.dtype = TISP_RAW_DTYPE_RAW16;
    a.mtype = TISP_RAW_MODULE_START;
    a.nrvbs = raw_nrvbs;
    a.frame_done = raw_cb;
    isp_open = 0;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4088);
    isp_open = 1;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, NULL) == -4087);
    assert(IMP_ISP_RAW_RwControl((IMPVI_NUM)2, &a) == -4084);
    a.mtype = TISP_RAW_MODULE_BUTT;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4091);
    a.mtype = TISP_RAW_MODULE_START;
    tuning_on = 0;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4091);
    tuning_on = 1;
    a.nrvbs = 5;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4092);
    a.nrvbs = raw_nrvbs;
    alloc_fail = 1;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4094);
    alloc_fail = 0;
    assert(ctl_n == 0);

    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);
    assert(ctl_n == 1);
    assert(ctl_seen[0][0] == 1 && ctl_seen[0][1] == 1 && ctl_seen[0][2] == 0);
    for (i = 0; i < 4; i++)
        assert(ctl_seen[0][3 + i] == (i < (int)raw_nrvbs ? PHYS + i * raw_frame : 0));
    wait_count(&raw_cb_count, 12);
    wait_count(&raw_sets, 12);
    for (i = 0; i < 12; i++)            /* every frame goes back as it came */
        assert(raw_set_addr[i] == PHYS + (i % raw_nrvbs) * raw_frame);
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);  /* already on: no-op */
    assert(ctl_n == 1);

    block_forever = 1;
    usleep(20 * 1000);
    a.mode = IMPISP_OPS_MODE_DISABLE;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);  /* must not hang */
    block_forever = 0;
    assert(ctl_n == 2 && ctl_seen[1][0] == 0);
    assert(freed == (uintptr_t)rmem);

    ctl_fail = 1;
    a.mode = IMPISP_OPS_MODE_ENABLE;
    freed = 0;
    i = raw_gets;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4090);
    assert(freed == (uintptr_t)rmem);   /* refused control: block back (F5) */
    usleep(20 * 1000);
    assert(raw_gets == i);              /* and no thread started */
    a.mode = IMPISP_OPS_MODE_DISABLE;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4090);
    ctl_fail = 0;

    /* after a refused enable the next enable really starts the ring */
    a.mode = IMPISP_OPS_MODE_ENABLE;
    i = ctl_n;
    assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);
    assert(ctl_n == i + 1 && ctl_seen[i][0] == 1);
    wait_count(&raw_sets, raw_sets + 3);

    /* DisableTuning/DisableSensor hook: ring left on is stopped (F1) */
    freed = 0;
    block_forever = 1;                  /* thread asleep in the driver */
    usleep(20 * 1000);
    OpenIMP_P1_RawStop();
    block_forever = 0;
    assert(ctl_n == i + 2 && ctl_seen[i + 1][0] == 0 && ctl_seen[i + 1][1] == 1);
    assert(freed == (uintptr_t)rmem);
    OpenIMP_P1_RawStop();               /* off: nothing */
    assert(ctl_n == i + 2);

    /* ---- test pattern mode 6: RAW8 x1 ring of the library ---- */
    {
        const uint8_t *px = (const uint8_t *)rmem;
        const uint8_t on = 15 + 750 * 240 / 1000, off = 15;
#define RAMP(x) (uint8_t)(15 + ((x) * 1000 / 63) * 240 / 1000)

        raw_nrvbs = 1;
        rawvic_test_isp[0x88 / 4] = 0x12345605;      /* order 5: not a plain Bayer order */
        errno = 0;
        assert(openimp_tp_ispin_set(5) == -1 && errno == EINVAL);
        assert(openimp_tp_ispin_set(4) == -1 && errno == ENOTSUP);
        rawvic_test_isp[0x88 / 4] = 0x12345602;      /* GRBG (upper bits ignored) */
        isp_open = 0;
        assert(openimp_tp_ispin_set(4) == -1 && errno == ENODEV);
        isp_open = 1;
        i = ctl_n;
        memset(rmem, 0x5a, sizeof(rmem));
        assert(openimp_tp_ispin_set(4) == 0);
        assert(ctl_n == i + 1);
        assert(ctl_seen[i][0] == 1 && ctl_seen[i][1] == 0 && ctl_seen[i][2] == 0);
        assert(ctl_seen[i][3] == PHYS && ctl_seen[i][4] == 0 && ctl_seen[i][6] == 0);
        wait_count(&flushes, 3);
        /* bars of 8 px: 0 white, 1 yellow, 6 blue, 7 black; GRBG: row 0 G R, row 1 B G */
        assert(px[0] == on && px[1] == on && px[64] == on && px[65] == on);
        assert(px[8] == on && px[9] == on && px[64 + 8] == off && px[64 + 9] == on);
        assert(px[48] == off && px[49] == off && px[64 + 48] == on && px[64 + 49] == off);
        assert(px[56] == off && px[57] == off && px[64 * 31 + 57] == off);
        assert(px[64 * 30 + 9] == on);                /* every row drawn */
        assert(raw_set_addr[(raw_sets - 1) % 64] == PHYS);  /* fed back */

        a.mode = IMPISP_OPS_MODE_ENABLE;
        assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == -4091);  /* held by mode 6 */
        assert(ctl_n == i + 1);

        assert(openimp_tp_ispin_set(2) == 0 && ctl_n == i + 1);  /* switch: no new ring */
        flushes = 0;
        wait_count(&flushes, 3);
        /* bands of 8 rows red, green, blue, white with a ramp along x */
        assert(px[63] == 255 && px[62] == off && px[64 + 63] == off);    /* row 0: R at odd x */
        assert(px[64 * 9 + 63] == 255 && px[64 * 8 + 63] == off);        /* green band */
        assert(px[64 * 17 + 62] == RAMP(62) && px[64 * 16 + 62] == off); /* blue band: B at row 1 x even */
        assert(px[64 * 24 + 62] == RAMP(62) && px[64 * 25 + 63] == 255 && px[64 * 24] == off);
        assert(openimp_tp_ispin_set(3) == 0);
        flushes = 0;
        wait_count(&flushes, 3);
        assert(px[64 * 31 + 49] == 255 && px[64 * 0 + 49] == off);      /* white band, ramp down */
        assert(px[64 * 16 + 1] > px[64 * 2 + 1]);                        /* red band ramps along y */
        rawvic_test_isp[0x88 / 4] = 0;                /* order moved to RGGB: followed */
        flushes = 0;
        wait_count(&flushes, 3);
        assert(px[64 * 30] > 200 && px[64 * 30 + 1] == off);          /* row 30 even: R at x even */

        freed = 0;
        assert(openimp_tp_ispin_set(0) == 0);
        assert(ctl_n == i + 2 && ctl_seen[i + 1][0] == 0 && freed == (uintptr_t)rmem);
        assert(openimp_tp_ispin_set(0) == 0 && ctl_n == i + 2);      /* off stays off */

        /* an application ring has priority */
        raw_nrvbs = 3;
        a.frame_done = NULL;            /* raw_cb counts frames of its own ring only */
        a.mode = IMPISP_OPS_MODE_ENABLE;
        assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);
        assert(openimp_tp_ispin_set(1) == -1 && errno == EBUSY);
        assert(openimp_tp_ispin_set(0) == 0 && ctl_n == i + 3);      /* not its ring */
        a.mode = IMPISP_OPS_MODE_DISABLE;
        assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);
        /* DISABLE by the application stops a mode 6 ring */
        raw_nrvbs = 1;
        assert(openimp_tp_ispin_set(1) == 0);
        flushes = 0;
        wait_count(&flushes, 2);
        assert(px[0] == 15 + 500 * 240 / 1000 && px[64 * 31 + 63] == px[0]);
        assert(IMP_ISP_RAW_RwControl(IMPVI_MAIN, &a) == 0);
        assert(openimp_tp_ispin_set(1) == 0 && ctl_seen[ctl_n - 1][0] == 1);  /* free again */
        OpenIMP_P1_RawStop();
        assert(ctl_seen[ctl_n - 1][0] == 0);
    }

    printf("t41 rawvic test: ok (vic cb %d, raw cb %d)\n", cb_count, raw_cb_count);
    return 0;
}
