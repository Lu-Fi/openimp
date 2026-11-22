/*
 * T23 IVDC direct mode, OpenIMP side (openimp #767).
 *
 * With tx-isp-t23 direct_mode=1 the ISP hands frame channel 0 line by line
 * to the Helix encoder through the IVDC block (0x13200000) and a small DDR
 * ring (ivdc_mem_line lines) instead of writing whole frames.  The flow
 * follows the vendor libimp T23 1.3.0 (Ghidra addresses):
 *  - IMP_FrameSource_CreateChn @0x98498: channels 0/3/6 open /dev/misc-ivdc
 *    and ask GET_DIRECT (0xc004543f); direct mode 1..3 sets the drop
 *    threshold to 300 lines;
 *  - IMP_FrameSource_EnableChn @0x9b640: channel 0 in direct mode asks
 *    GET_MEM_LINE (0xc0045442), allocates the ring align256(width) *
 *    mem_line * 3 / 2 (width * height * 3 / 8 without a line count) and
 *    hands its address over with SET_PADDR (0xc0045441);
 *  - release_Frame @0x940ec: QBUF of such a frame passes +0x20 (0x80000000);
 *  - IMP_FrameSource_SetDirectModeAttr @0x9e504 sets the threshold;
 *  - VBMCreatePool @0x2d120: no frame memory for such a channel, every
 *    frame record gets phys 0, virt 0 and +0x20 = 0x80000000;
 *  - get_frame @0x94744: DQBUF reserved byte 0 = frame type (3), byte 2 =
 *    sensor, reserved2 = IVDC overflow count, +0x3f4 = the threshold;
 *  - on_encoder_group_data_update @0x5b750 / do_channel_process @0x5a040,
 *    direct modes 2 and 3 only: drop the frame when the IVDC overflow count (0x1320002c) passed the
 *    frame's, or the ISP write line (0x13200208 bits 30:16) leads the VPU
 *    read line (0x132003a8) by more than the threshold; else the Helix
 *    source is 0x80000000 / 0x81000000 with frame type 3, and the soc_vpu
 *    channel node gets overflow count, ivdc_mem_line, threshold and the
 *    frame time in ms (hwicodec_pf_h264e_t21_enc @0x3907c).
 * Only the single-sensor direct mode 1 on channel 0 is supported, which is
 * what open-tx-isp T23 implements.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "openimp_t23_ivdc.h"

#define IVDC_CHANNELS   9
#define IVDC_REG_BASE   0x13200000u
#define IVDC_MAP_SIZE   0x80000u
#define IVDC_OVERFLOW   0x2cu
#define IVDC_ISP_LINE   0x208u
#define IVDC_VPU_LINE   0x3a8u

typedef struct {
    int fd;              /* /dev/misc-ivdc, -1 = none */
    int mode;            /* GET_DIRECT result */
    int threshold;       /* vendor channel +0x28 */
    int enabled;
    IMPDMABufferInfo ring;
} IvdcChn;

static pthread_mutex_t ivdc_lock = PTHREAD_MUTEX_INITIALIZER;
static IvdcChn ivdc_chn[IVDC_CHANNELS] = {
    [0 ... IVDC_CHANNELS - 1] = { .fd = -1 },
};
static uint32_t ivdc_mem_line;
static uint32_t ivdc_mode;        /* last GET_DIRECT result */
static volatile uint32_t *ivdc_regs;
static int ivdc_map_failed;
static int (*ivdc_reg_reader)(uint32_t addr, uint32_t *val);

static int ivdc_valid(int chn)
{
    return chn >= 0 && chn < IVDC_CHANNELS;
}

int OpenIMP_T23_IvdcCreateChn(int chn)
{
    int fd, mode = 0;

    /* vendor: (0x49 >> chn) & 1, channels 0, 3 and 6 */
    if (!ivdc_valid(chn) || chn > 6 || !((0x49u >> chn) & 1u))
        return 0;
    pthread_mutex_lock(&ivdc_lock);
    if (ivdc_chn[chn].fd >= 0) {
        pthread_mutex_unlock(&ivdc_lock);
        return 0;
    }
    fd = open("/dev/misc-ivdc", O_RDONLY);
    if (fd < 0) {
        /* a driver without the IVDC device: normal capture */
        pthread_mutex_unlock(&ivdc_lock);
        return 0;
    }
    if (ioctl(fd, T23_IVDC_GET_DIRECT, &mode) < 0) {
        close(fd);
        pthread_mutex_unlock(&ivdc_lock);
        IMP_LOG_ERR("Framesource", "CreateChn(%d): IVDC GET_DIRECT failed: %s",
                    chn, strerror(errno));
        return -1;
    }
    ivdc_chn[chn].fd = fd;
    ivdc_chn[chn].mode = mode;
    __atomic_store_n(&ivdc_mode, (uint32_t)mode, __ATOMIC_RELAXED);
    if (mode >= 1 && mode <= 3)
        ivdc_chn[chn].threshold = T23_IVDC_THRESHOLD;
    pthread_mutex_unlock(&ivdc_lock);
    if (mode)
        IMP_LOG_INFO("Framesource", "channel %d: IVDC direct mode %d", chn, mode);
    return 0;
}

void OpenIMP_T23_IvdcDestroyChn(int chn)
{
    if (!ivdc_valid(chn))
        return;
    OpenIMP_T23_IvdcDisableChn(chn);
    pthread_mutex_lock(&ivdc_lock);
    if (ivdc_chn[chn].fd >= 0)
        close(ivdc_chn[chn].fd);
    ivdc_chn[chn].fd = -1;
    ivdc_chn[chn].mode = 0;
    pthread_mutex_unlock(&ivdc_lock);
}

/* channel 0 of one sensor in mode 1..3 (vendor EnableChn: index 0 and
 * direct != 0; VBMCreatePool: index 0 with mode 1..3) */
static int ivdc_direct_locked(int chn)
{
    return chn == 0 && ivdc_chn[chn].fd >= 0 &&
           ivdc_chn[chn].mode >= 1 && ivdc_chn[chn].mode <= 3;
}

int OpenIMP_T23_IvdcChnDirect(int chn)
{
    int direct;

    if (!ivdc_valid(chn))
        return 0;
    pthread_mutex_lock(&ivdc_lock);
    direct = ivdc_direct_locked(chn);
    pthread_mutex_unlock(&ivdc_lock);
    return direct;
}

int OpenIMP_T23_IvdcEnableChn(int chn, int width, int height)
{
    uint32_t line = 0, aligned;
    int size, fd, paddr;

    if (!ivdc_valid(chn))
        return -1;
    pthread_mutex_lock(&ivdc_lock);
    if (!ivdc_direct_locked(chn) || ivdc_chn[chn].enabled) {
        pthread_mutex_unlock(&ivdc_lock);
        return 0;
    }
    fd = ivdc_chn[chn].fd;
    if (ioctl(fd, T23_IVDC_GET_MEM_LINE, &line) < 0) {
        pthread_mutex_unlock(&ivdc_lock);
        IMP_LOG_ERR("Framesource", "EnableChn(%d): IVDC GET_MEM_LINE failed: %s",
                    chn, strerror(errno));
        return -1;
    }
    aligned = ((uint32_t)width + 0xffu) & ~0xffu;
    size = line ? (int)(aligned * line * 3u / 2u)
                : (int)(aligned * (uint32_t)height * 3u / 8u);
    memset(&ivdc_chn[chn].ring, 0, sizeof(ivdc_chn[chn].ring));
    /* vendor: IMP_PoolAlloc of the channel's pool, else IMP_Alloc; OpenIMP
     * pools are names only (IMP_System_MemPoolRequest), the frame pools
     * allocate from the one rmem arena as well */
    if (DMA_AllocDescriptor(&ivdc_chn[chn].ring, size, "ivdc_ring") != 0) {
        pthread_mutex_unlock(&ivdc_lock);
        IMP_LOG_ERR("Framesource", "EnableChn(%d): no rmem for the %d-byte "
                    "IVDC ring", chn, size);
        return -1;
    }
    paddr = (int)ivdc_chn[chn].ring.phys_addr;
    if (ioctl(fd, T23_IVDC_SET_PADDR, &paddr) < 0) {
        DMA_FreePhys(ivdc_chn[chn].ring.phys_addr);
        memset(&ivdc_chn[chn].ring, 0, sizeof(ivdc_chn[chn].ring));
        pthread_mutex_unlock(&ivdc_lock);
        IMP_LOG_ERR("Framesource", "EnableChn(%d): IVDC SET_PADDR failed: %s",
                    chn, strerror(errno));
        return -1;
    }
    ivdc_mem_line = line;
    ivdc_chn[chn].enabled = 1;
    pthread_mutex_unlock(&ivdc_lock);
    IMP_LOG_INFO("Framesource", "channel %d: IVDC ring 0x%08x %d bytes "
                 "(%u lines), threshold %d", chn, (unsigned)paddr, size, line,
                 ivdc_chn[chn].threshold);
    return 0;
}

void OpenIMP_T23_IvdcDisableChn(int chn)
{
    if (!ivdc_valid(chn))
        return;
    pthread_mutex_lock(&ivdc_lock);
    if (ivdc_chn[chn].enabled && ivdc_chn[chn].ring.phys_addr)
        DMA_FreePhys(ivdc_chn[chn].ring.phys_addr);
    memset(&ivdc_chn[chn].ring, 0, sizeof(ivdc_chn[chn].ring));
    ivdc_chn[chn].enabled = 0;
    pthread_mutex_unlock(&ivdc_lock);
}

int OpenIMP_T23_IvdcThreshold(int chn)
{
    int t;

    if (!ivdc_valid(chn))
        return 0;
    pthread_mutex_lock(&ivdc_lock);
    t = ivdc_chn[chn].threshold;
    pthread_mutex_unlock(&ivdc_lock);
    return t;
}

int OpenIMP_T23_IvdcSetThreshold(int chn, int threshold)
{
    if (!ivdc_valid(chn))
        return -1;
    pthread_mutex_lock(&ivdc_lock);
    ivdc_chn[chn].threshold = threshold;
    pthread_mutex_unlock(&ivdc_lock);
    return 0;
}

uint32_t OpenIMP_T23_IvdcMemLine(void)
{
    return __atomic_load_n(&ivdc_mem_line, __ATOMIC_RELAXED);
}

void OpenIMP_T23_IvdcSetRegReader(int (*fn)(uint32_t addr, uint32_t *val))
{
    ivdc_reg_reader = fn;
}

/* vendor ivdc_mmap @0x2ee70 (/dev/mem O_RDWR|O_SYNC, 0x80000 at
 * 0x13200000) and ivdc_regrw @0x2f0c0 */
static int ivdc_reg_read(uint32_t addr, uint32_t *val)
{
    if (ivdc_reg_reader)
        return ivdc_reg_reader(addr, val);
    if (addr < IVDC_REG_BASE || addr - IVDC_REG_BASE >= IVDC_MAP_SIZE)
        return -1;
    if (!ivdc_regs) {
        int fd;
        void *map;

        pthread_mutex_lock(&ivdc_lock);
        if (!ivdc_regs && !ivdc_map_failed) {
            fd = open("/dev/mem", O_RDWR | O_SYNC);
            map = fd < 0 ? MAP_FAILED
                         : mmap(NULL, IVDC_MAP_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, IVDC_REG_BASE);
            if (fd >= 0)
                close(fd);
            if (map == MAP_FAILED) {
                ivdc_map_failed = 1;
                IMP_LOG_ERR("Encoder", "IVDC registers not mapped (/dev/mem): "
                            "frames are coded without the drop check");
            } else {
                ivdc_regs = map;
            }
        }
        pthread_mutex_unlock(&ivdc_lock);
        if (!ivdc_regs)
            return -1;
    }
    *val = ivdc_regs[(addr - IVDC_REG_BASE) >> 2];
    return 0;
}

uint32_t OpenIMP_T23_IvdcLead(uint32_t isp_line_reg, uint32_t vpu_line_reg,
                              uint32_t mem_line)
{
    uint32_t isp = isp_line_reg >> 16;

    if (((isp ^ vpu_line_reg) & 0x8000u) == 0)
        return (isp & 0x7fffu) - (vpu_line_reg & 0x7fffu);
    return (isp & 0x7fffu) - (vpu_line_reg & 0x7fffu) + mem_line;
}

int OpenIMP_T23_IvdcFrame(const void *frame)
{
    const uint8_t *f = frame;
    uint32_t phys, direct;

    if (!f)
        return 0;
    memcpy(&phys, f + 0x18, 4);
    memcpy(&direct, f + 0x20, 4);
    return f[T23_FRAME_TYPE] == T23_IVDC_FRAME_TYPE && !phys &&
           (direct & T23_IVDC_SRC_Y);
}

int OpenIMP_T23_IvdcCheck(const void *frame, uint32_t *overflow_cnt,
                          uint32_t *mem_line, uint32_t *threshold,
                          uint64_t *time_ms)
{
    const uint8_t *f = frame;
    uint32_t frame_ovf, frame_thr, ovf, isp, vpu, lead, line;
    int64_t ts_us;
    static unsigned int drops;

    if (overflow_cnt)
        *overflow_cnt = 0;
    if (mem_line)
        *mem_line = 0;
    if (threshold)
        *threshold = 0;
    if (time_ms)
        *time_ms = 0;
    /* The vendor checks and fills the node values only for the encoder
     * channel's direct modes 2 and 3 (several sensors): "mode - 2 < 2" in
     * on_encoder_group_data_update @0x5b750 and do_channel_process
     * @0x5a040.  Mode 1 (one sensor) codes every IVDC frame with frame
     * type 3 and zero node values (soc_vpu then checks the overflow count
     * and the lead against its own data_threshold). */
    if (__atomic_load_n(&ivdc_mode, __ATOMIC_RELAXED) - 2u >= 2u)
        return 0;
    memcpy(&frame_ovf, f + T23_FRAME_OVERFLOW, 4);
    memcpy(&frame_thr, f + T23_FRAME_THRESHOLD, 4);
    memcpy(&ts_us, f + 0x30, 8);
    line = OpenIMP_T23_IvdcMemLine();
    if (ivdc_reg_read(IVDC_REG_BASE + IVDC_OVERFLOW, &ovf) == 0) {
        /* on_encoder_group_data_update: log 0x61b */
        if (ovf > frame_ovf) {
            if (drops++ < 5u || drops % 100u == 0u)
                IMP_LOG_WARN("Encoder", "IVDC overflow count %u > frame %u, "
                             "dropping the frame (%u drops)", ovf, frame_ovf,
                             drops);
            return -1;
        }
        if (ivdc_reg_read(IVDC_REG_BASE + IVDC_ISP_LINE, &isp) == 0 &&
            ivdc_reg_read(IVDC_REG_BASE + IVDC_VPU_LINE, &vpu) == 0) {
            lead = OpenIMP_T23_IvdcLead(isp, vpu, line);
            /* log 0x62b / do_channel_process log 0x3cb */
            if (lead > frame_thr) {
                if (drops++ < 5u || drops % 100u == 0u)
                    IMP_LOG_WARN("Encoder", "IVDC data line %u > threshold %u, "
                                 "dropping the frame (%u drops)", lead,
                                 frame_thr, drops);
                return -1;
            }
        }
    }
    if (overflow_cnt)
        *overflow_cnt = frame_ovf;
    if (mem_line)
        *mem_line = line;
    if (threshold)
        *threshold = frame_thr;
    if (time_ms)
        *time_ms = ts_us > 0 ? (uint64_t)ts_us / 1000u : 0u;
    return 0;
}
