/*
 * OpenIMP T32 IVDC direct mode (#301, planned; not a release gate): the ISP
 * hands frame channel 0 to the encoder through the IVDC line buffer instead
 * of DDR frame buffers.
 *
 * Whether the ISP runs in direct mode is the kernel's choice (tx-isp
 * module parameter direct_mode, 1 = one sensor); IMP_System_Init asks
 * /dev/misc-ivdc GET_DIRECT.  The flow follows what vendor libimp 2.2.0 does
 * in QEMU -M ingenic-t32 with the vendor tx-isp-t32.ko direct_mode=1
 * (ioctls: tests/t32/vendor-2.2.0-ioctl-seq-ivdc.txt; IVDC register accesses
 * recorded with QEMU's memory_region_ops trace, audit #301); no vendor code
 * copied.
 *
 *   System_Init   direct: map the IVDC registers (0x13100000) through
 *                 /dev/mem, reset the block (0x0 = 3) unless CPM 0x10000028
 *                 bit 3 is set; keep /dev/misc-ivdc open for FrameSource
 *   FS EnableChn  chn 0: MEM_LINE (lines the driver keeps, 0 = half a
 *                 frame), allocate the DDR overflow ring (width rounded up to
 *                 64 x lines x 3/2), SET_PADDR; the channel's buffers are
 *                 queued with the IVDC source address (no frame memory)
 *   StartRecvPic  a channel fed by the direct channel: picture size of its
 *                 IVDC reader (0x18 VPU0 H.264/H.265, 0x1c VPU1 JPEG), its
 *                 threshold word (0xc0 / 0xc4) and the reader enable (0x08);
 *                 the first video channel tops up the run FIFO (0x80 / 0x78)
 *   each picture  source Y 0x80000000, UV 0x81000000, stride 4096, and the
 *                 soc_vpu channel node marked frame_type 3 (FRAME_TYPE_IVDC)
 *                 with the sensor: the kernel's radix / helix driver then
 *                 starts the IVDC reader (0x70 / 0x88) for the job
 *   StopRecvPic   the last video channel: 0x74 = 1
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>

#include "imp_log_int.h"
#include "t32/openimp_t32_ioctl.h"
#include "t32/openimp_t32_isp.h"
#include "t32/openimp_t32_ivdc.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_ivdc.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-IVDC"

#define IVDC_BASE       0x13100000u
#define IVDC_MAP_LEN    0x80000u
#define CPM_BASE        0x10000000u
#define CPM_REG_28      0x28u           /* bit 3 set: the block is not reset */
#define IVDC_MEM_LINE_MAX 6480

#define IVDC_CTRL       0x000u          /* 3: reset */
#define IVDC_VPU_EN     0x008u          /* 0x24: VPU0 reader, 0x28: VPU1 */
#define IVDC_VPU0_SIZE  0x018u
#define IVDC_VPU1_SIZE  0x01cu
#define IVDC_VPU_STOP   0x074u
#define IVDC_RUN_FIFO   0x078u
#define IVDC_FIFO_STAT  0x080u
#define IVDC_VPU0_THR   0x0c0u
#define IVDC_VPU1_THR   0x0c4u

static struct {
    pthread_mutex_t lock;
    int mode;                   /* GET_DIRECT at System_Init: 0 memory, 1..4 direct */
    int fd;                     /* /dev/misc-ivdc while in direct mode */
    volatile uint32_t *regs;
    size_t map_len;
    int video_users;            /* started H.264/H.265 channels on the direct path */
} iv = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
};

static int ivdc_map(void)
{
    long page = sysconf(_SC_PAGESIZE);
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    void *p;

    if (fd < 0) {
        IMP_LOG_ERR(TAG, "open /dev/mem: %s", strerror(errno));
        return -1;
    }
    iv.map_len = page > 0 && IVDC_MAP_LEN % (size_t)page == 0 ? IVDC_MAP_LEN : IVDC_MAP_LEN + 0x400u;
    p = mmap(NULL, iv.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IVDC_BASE);
    close(fd);
    if (p == MAP_FAILED) {
        IMP_LOG_ERR(TAG, "map IVDC registers: %s", strerror(errno));
        return -1;
    }
    iv.regs = p;
    return 0;
}

/* CPM 0x10000028 bit 3 (read once at System_Init); -1 when unreadable */
static int cpm_reg28_bit3(void)
{
    long page = sysconf(_SC_PAGESIZE);
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC), bit;
    void *p;

    if (fd < 0)
        return -1;
    p = mmap(NULL, page > 0 ? (size_t)page : 4096u, PROT_READ, MAP_SHARED, fd, CPM_BASE);
    close(fd);
    if (p == MAP_FAILED)
        return -1;
    bit = (((volatile uint32_t *)p)[CPM_REG_28 / 4u] >> 3) & 1u;
    munmap(p, page > 0 ? (size_t)page : 4096u);
    return bit;
}

int OpenIMP_T32_IvdcRead(uint32_t off, uint32_t *v)
{
    if (!iv.regs || off >= IVDC_MAP_LEN || (off & 3u) || !v)
        return -1;
    *v = iv.regs[off / 4u];
    return 0;
}

int OpenIMP_T32_IvdcWrite(uint32_t off, uint32_t v)
{
    if (!iv.regs || off >= IVDC_MAP_LEN || (off & 3u))
        return -1;
    iv.regs[off / 4u] = v;
    return 0;
}

int OpenIMP_T32_IvdcSystemInit(int mode)
{
    int bit3;

    pthread_mutex_lock(&iv.lock);
    iv.mode = mode;
    iv.video_users = 0;
    if (!mode) {
        pthread_mutex_unlock(&iv.lock);
        return 0;
    }
    if (mode != 1)
        IMP_LOG_WARN(TAG, "IVDC direct mode %d (several sensors) is not supported; "
                     "only frame channel 0 of one sensor goes direct", mode);
    if (iv.fd < 0) {
        iv.fd = open("/dev/misc-ivdc", O_RDWR | O_CLOEXEC);
        if (iv.fd < 0)
            IMP_LOG_ERR(TAG, "open /dev/misc-ivdc: %s", strerror(errno));
    }
    if (!iv.regs && ivdc_map() == 0) {
        bit3 = cpm_reg28_bit3();
        if (bit3 == 0)
            OpenIMP_T32_IvdcWrite(IVDC_CTRL, 3u);   /* reset the block */
    }
    IMP_LOG_INFO(TAG, "IVDC direct mode %d (ISP channel 0 -> encoder without frame buffers)", mode);
    pthread_mutex_unlock(&iv.lock);
    return iv.regs && iv.fd >= 0 ? 0 : -1;
}

void OpenIMP_T32_IvdcSystemExit(void)
{
    pthread_mutex_lock(&iv.lock);
    if (iv.regs)
        munmap((void *)iv.regs, iv.map_len);
    iv.regs = NULL;
    if (iv.fd >= 0)
        close(iv.fd);
    iv.fd = -1;
    iv.mode = 0;
    iv.video_users = 0;
    pthread_mutex_unlock(&iv.lock);
}

int OpenIMP_T32_IvdcMode(void)
{
    return iv.mode;
}

int OpenIMP_T32_IvdcIoctl(uint32_t req, int32_t *arg, const char *name)
{
    if (iv.fd < 0)
        return -1;
    if (ioctl(iv.fd, req, arg) < 0) {
        IMP_LOG_ERR(TAG, "%s (0x%08x) failed: %s", name, req, strerror(errno));
        return -1;
    }
    return 0;
}

/* the reader's threshold word: @t lines, the value's low part @v */
static uint32_t ivdc_thr(uint32_t t, uint32_t v, uint32_t tshift)
{
    uint32_t f = v != 0;

    return t << tshift | v | f << 15 | f << 7;
}

void OpenIMP_T32_IvdcEncStart(int codec, uint32_t w, uint32_t h, int direct)
{
    uint32_t v, wr, rd, t;
    int check = 0;

    pthread_mutex_lock(&iv.lock);
    if (!iv.mode || !iv.regs) {
        pthread_mutex_unlock(&iv.lock);
        return;
    }
    if (direct) {
        switch (codec) {
        case OI_T32_IVDC_H265:
            OpenIMP_T32_IvdcWrite(IVDC_VPU0_SIZE, ((h + 7u) >> 3) << 19 | ((w + 15u) & ~15u));
            t = ((h + 15u) >> 4) * 2u - ((h + 7u) >> 3);
            OpenIMP_T32_IvdcWrite(IVDC_VPU0_THR, ivdc_thr(t, t * 8u, 10));
            OpenIMP_T32_IvdcWrite(IVDC_VPU_EN, 0x24u);
            break;
        case OI_T32_IVDC_H264:
            OpenIMP_T32_IvdcWrite(IVDC_VPU0_SIZE, ((h + 15u) >> 4) << 20 | ((w + 15u) & ~15u));
            OpenIMP_T32_IvdcWrite(IVDC_VPU0_THR, 0u);
            OpenIMP_T32_IvdcWrite(IVDC_VPU_EN, 0x24u);
            break;
        default:                        /* JPEG on VPU1 */
            OpenIMP_T32_IvdcWrite(IVDC_VPU1_SIZE, ((h + 1u) >> 1) << 17 | ((w + 15u) & ~15u));
            t = ((h + 15u) >> 4) * 8u - ((h + 1u) >> 1);
            OpenIMP_T32_IvdcWrite(IVDC_VPU1_THR, ivdc_thr(t, t * 2u, 8));
            OpenIMP_T32_IvdcWrite(IVDC_VPU_EN, 0x28u);
            break;
        }
        if (codec != OI_T32_IVDC_JPEG)
            check = ++iv.video_users >= 1;
    } else {
        check = iv.video_users >= 1 && codec != OI_T32_IVDC_JPEG;
    }
    /* run FIFO (write slot bits 2:0, read slot 6:4, bit 17 wrapped): top it
     * up when the reader is at most one entry behind */
    if (check && OpenIMP_T32_IvdcRead(IVDC_FIFO_STAT, &v) == 0) {
        wr = v & 7u;
        rd = (v & 0x7fu) >> 4;
        if ((v & 1u << 17) ? wr + 7u == rd : wr - rd <= 1u) {
            IMP_LOG_INFO(TAG, "run FIFO 0x%08x: two entries added", v);
            OpenIMP_T32_IvdcWrite(IVDC_RUN_FIFO, 0x1000000u);
            OpenIMP_T32_IvdcWrite(IVDC_RUN_FIFO, 0x1000000u);
        }
    }
    pthread_mutex_unlock(&iv.lock);
}

void OpenIMP_T32_IvdcEncStop(int codec, int direct)
{
    pthread_mutex_lock(&iv.lock);
    if (iv.mode && iv.regs && direct && codec != OI_T32_IVDC_JPEG && iv.video_users > 0 &&
        --iv.video_users == 0) {
        OpenIMP_T32_IvdcWrite(IVDC_VPU_STOP, 1u);
        IMP_LOG_INFO(TAG, "last direct video channel stopped");
    }
    pthread_mutex_unlock(&iv.lock);
}

/* ---- FrameSource direct-mode API (vendor 2.x) ------------------------------- */
int IMP_FrameSource_GetDirectMode(int *direct_mode)
{
    if (!direct_mode)
        return -1;
    if (!OpenIMP_T32_SystemInitDone()) {
        IMP_LOG_ERR(TAG, "GetDirectMode before IMP_System_Init");
        return -1;
    }
    *direct_mode = iv.mode;
    return 0;
}

/* the kernel's mode for the next System_Init (0 memory, 1..4 direct); vendor 2.0.2.1 codes:
 * mode >= 5 0x80010002, node open / ioctl failure 0x80011000 */
int IMP_FrameSource_SetDirectMode(int direct_mode)
{
    int32_t m = direct_mode;
    int fd, rc;

    if ((unsigned)direct_mode > 4) {
        IMP_LOG_ERR(TAG, "SetDirectMode %d (0..4)", direct_mode);
        return (int)0x80010002;
    }
    fd = open("/dev/misc-ivdc", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "open /dev/misc-ivdc: %s", strerror(errno));
        return (int)0x80011000;
    }
    rc = ioctl(fd, OI_T32_IOC_SET_DIRECT, &m);
    if (rc < 0)
        IMP_LOG_ERR(TAG, "SET_DIRECT %d: %s", direct_mode, strerror(errno));
    close(fd);
    return rc < 0 ? (int)0x80011000 : 0;
}

int OpenIMP_T32_FsChnState(int chn, int *depth);   /* openimp_t32_fs.c */

/* vendor 2.0.2.1: lines > 0x1950 0x80010002; only in single-sensor direct mode (1) and before
 * FrameSource channel 0 runs, else 0x80040080; ioctl failure 0x80011000 */
int IMP_FrameSource_SetIvdcMemLine(int ivdc_mem_line)
{
    int32_t n = ivdc_mem_line;

    if ((unsigned)ivdc_mem_line > IVDC_MEM_LINE_MAX) {
        IMP_LOG_ERR(TAG, "SetIvdcMemLine %d (0..%d)", ivdc_mem_line, IVDC_MEM_LINE_MAX);
        return (int)0x80010002;
    }
    if (iv.mode != 1 || OpenIMP_T32_FsChnState(0, NULL) >= 2) {
        IMP_LOG_ERR(TAG, "SetIvdcMemLine: needs single-sensor direct mode before channel 0 runs (mode %d)", iv.mode);
        return (int)0x80040080;
    }
    if (OpenIMP_T32_IvdcIoctl(OI_T32_IOC_SET_MEM_LINE, &n, "SET_MEM_LINE") != 0)
        return (int)0x80011000;
    IMP_LOG_INFO(TAG, "IVDC mem line %d", ivdc_mem_line);
    return 0;
}
