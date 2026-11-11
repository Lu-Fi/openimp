/*
 * OpenIMP T32 soc_vpu channel layer (#291, planned; not a release gate).
 *
 * /dev/soc_vpu is the kernel's channel/vpu arbiter (4.4.94
 * drivers/video/ingenic_vpu/soc_vpu.c): CHANNEL_REQ hands out a channel
 * (the node's clist), RUN / START take an engine by vpu_id for one job,
 * WAIT_COMPLETE / WAIT_BSFULL_COMPLETE end it, CHANNEL_REL gives the
 * channel back; closing the file frees whatever the process still holds.
 * Every request carries the 64-byte channel_node (openimp_t32_ioctl.h,
 * the same in the thingino 4.4.94 tree and in the 2.0.0 SDK's 4.4.94 and
 * 3.10.14 kernels), except FLUSH_CACHE (12-byte flush_cache_info) and
 * WOR_VPU_REG (12-byte reg_info), which the kernel copies alone.
 *
 * The kernel trusts the node's clist/vlist pointers it handed out, so the
 * node is always the one the kernel last returned (never rebuilt), and the
 * vpu-held requests (WAIT_*, SET_BSFULL_REG) are refused here unless a
 * START left the engine held.  Observed vendor libimp 2.2.0 use (QEMU
 * trace, tests/t32/vendor-2.2.0-ioctl-seq-jpeg.txt): REQ with mdelay
 * 20000 ms, codecdir JPEGENC, thread_id -1; per JPEG picture FLUSH_CACHE
 * (write back the job), RUN on vpu_id HELIX|1, FLUSH_CACHE (invalidate the
 * bitstream); REL at DestroyChn.  HERA uses START + WAIT_BSFULL_COMPLETE.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "imp_log_int.h"
#include "t32/openimp_t32_vpu.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_vpu.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-VPU"
#define T32_VPU_PATH "/dev/soc_vpu"
#define T32_VPU_MDELAY_MS 20000u        /* the vendor's channel / engine wait */

static pthread_mutex_t vpu_lock = PTHREAD_MUTEX_INITIALIZER;
static int vpu_fd = -1;
static int vpu_users;

int OpenIMP_T32_VpuOpen(void)
{
    int rc = 0;

    pthread_mutex_lock(&vpu_lock);
    if (vpu_fd < 0) {
        vpu_fd = open(T32_VPU_PATH, O_RDWR | O_CLOEXEC);
        if (vpu_fd < 0) {
            IMP_LOG_ERR(TAG, "open %s: %s", T32_VPU_PATH, strerror(errno));
            rc = -1;
        }
    }
    if (!rc)
        vpu_users++;
    pthread_mutex_unlock(&vpu_lock);
    return rc;
}

void OpenIMP_T32_VpuClose(void)
{
    pthread_mutex_lock(&vpu_lock);
    if (vpu_users > 0 && --vpu_users == 0 && vpu_fd >= 0) {
        close(vpu_fd);
        vpu_fd = -1;
    }
    pthread_mutex_unlock(&vpu_lock);
}

static long vpu_ioctl(uint32_t req, void *arg, const char *name, int quiet_positive)
{
    long rc;

    if (vpu_fd < 0) {
        IMP_LOG_ERR(TAG, "%s: %s not open", name, T32_VPU_PATH);
        return -1;
    }
    rc = ioctl(vpu_fd, req, arg);
    if (rc < 0)
        IMP_LOG_ERR(TAG, "%s (0x%08x) failed: %s", name, req, strerror(errno));
    else if (rc > 0 && !quiet_positive)
        IMP_LOG_WARN(TAG, "%s (0x%08x): engine status 0x%08lx", name, req, rc);
    return rc;
}

#define VPU_IOCTL(req, arg, q) vpu_ioctl(OI_T32_VPU_##req, (arg), #req, (q))

static void vpu_result(const T32VpuChannel *c, long rc, T32VpuResult *r)
{
    if (!r)
        return;
    r->rc = rc;
    r->status = c->node.status;
    r->output_len = c->node.output_len;
    r->hw_time = c->node.hw_time;
}

int OpenIMP_T32_VpuRequest(T32VpuChannel *c, uint32_t codecdir)
{
    if (!c || c->requested)
        return -1;
    memset(&c->node, 0, sizeof(c->node));
    c->node.mdelay = T32_VPU_MDELAY_MS;
    c->node.codecdir = codecdir;
    c->node.workphase = OI_T32_VPU_WORK_OPEN;
    c->node.thread_id = -1;             /* no H.264 chain mode */
    if (VPU_IOCTL(CHANNEL_REQ, &c->node, 0) != 0 || !c->node.clist)
        return -1;
    c->requested = 1;
    c->started = 0;
    IMP_LOG_DBG(TAG, "channel %u (codec 0x%x)", c->node.channel_id, codecdir);
    return 0;
}

int OpenIMP_T32_VpuRelease(T32VpuChannel *c)
{
    long rc;

    if (!c || !c->requested)
        return -1;
    if (c->started) {                   /* never leave an engine held */
        T32VpuResult r;
        OpenIMP_T32_VpuWait(c, &r);
    }
    c->node.workphase = OI_T32_VPU_WORK_CLOSE;
    rc = VPU_IOCTL(CHANNEL_REL, &c->node, 0);
    c->requested = 0;
    return rc == 0 ? 0 : -1;
}

static void vpu_job(T32VpuChannel *c, uint32_t vpu_id, uint32_t job_pa)
{
    c->node.vpu_id = (int32_t)vpu_id;
    c->node.dma_addr = job_pa;
    c->node.workphase = OI_T32_VPU_WORK_OPEN;
    c->node.status = 0;
    c->node.output_len = 0;
    c->node.thread_id = -1;
}

int OpenIMP_T32_VpuRun(T32VpuChannel *c, uint32_t vpu_id, uint32_t job_pa, T32VpuResult *r)
{
    long rc;

    if (!c || !c->requested || c->started || !job_pa)
        return -1;
    vpu_job(c, vpu_id, job_pa);
    rc = VPU_IOCTL(CHANNEL_RUN, &c->node, 1);
    vpu_result(c, rc, r);
    return rc < 0 ? -1 : 0;
}

int OpenIMP_T32_VpuStart(T32VpuChannel *c, uint32_t vpu_id, uint32_t job_pa)
{
    if (!c || !c->requested || c->started || !job_pa)
        return -1;
    vpu_job(c, vpu_id, job_pa);
    if (VPU_IOCTL(CHANNEL_START, &c->node, 0) != 0 || !c->node.vlist)
        return -1;                      /* the kernel released the engine */
    c->started = 1;
    return 0;
}

int OpenIMP_T32_VpuWait(T32VpuChannel *c, T32VpuResult *r)
{
    long rc;

    if (!c || !c->started || !c->node.vlist)
        return -1;
    rc = VPU_IOCTL(CHANNEL_WAIT_COMPLETE, &c->node, 1);
    c->started = 0;                     /* released on success and on error */
    vpu_result(c, rc, r);
    return rc < 0 ? -1 : 0;
}

int OpenIMP_T32_VpuWaitBsFull(T32VpuChannel *c, T32VpuResult *r)
{
    long rc;

    if (!c || !c->started || !c->node.vlist)
        return -1;
    rc = VPU_IOCTL(CHANNEL_WAIT_BSFULL_COMPLETE, &c->node, 1);
    /* a full section keeps the engine (node.vlist stays set); the end of
     * the picture and every error release it */
    if (rc != OI_T32_VPU_RET_HERA_BSFULL || !c->node.vlist)
        c->started = 0;
    vpu_result(c, rc, r);
    return rc < 0 ? -1 : 0;
}

int OpenIMP_T32_VpuSetBsFull(T32VpuChannel *c)
{
    long rc;

    if (!c || !c->started || !c->node.vlist)
        return -1;
    rc = VPU_IOCTL(CHANNEL_SET_BSFULL_REG, &c->node, 0);
    if (rc < 0 || !c->node.vlist)
        c->started = 0;
    return rc < 0 ? -1 : 0;
}

int OpenIMP_T32_VpuFlush(const void *va, uint32_t len, uint32_t dir)
{
    /* the request's size field is a channel_node: hand over that much */
    union { T32IocFlushCache info; T32IocChannelNode pad; } arg;

    if (!va || !len)
        return -1;
    memset(&arg, 0, sizeof(arg));
    arg.info.addr = (uint32_t)(uintptr_t)va;
    arg.info.len = len;
    arg.info.dir = dir;
    return VPU_IOCTL(CHANNEL_FLUSH_CACHE, &arg, 0) == 0 ? 0 : -1;
}

static int vpu_reg(uint32_t paddr, uint32_t *value, uint32_t dir)
{
    union { T32IocVpuReg reg; T32IocChannelNode pad; } arg;

    memset(&arg, 0, sizeof(arg));
    arg.reg.paddr = paddr;
    arg.reg.value = *value;
    arg.reg.dir = dir;
    if (VPU_IOCTL(CHANNEL_WOR_VPU_REG, &arg, 0) != 0)
        return -1;
    *value = arg.reg.value;
    return 0;
}

int OpenIMP_T32_VpuReadReg(uint32_t paddr, uint32_t *value)
{
    if (!value)
        return -1;
    *value = 0;
    return vpu_reg(paddr, value, 0);
}

int OpenIMP_T32_VpuWriteReg(uint32_t paddr, uint32_t value)
{
    return vpu_reg(paddr, &value, 1);
}

int OpenIMP_T32_VpuBufAlloc(T32VpuBuf *b, uint32_t size, const char *tag)
{
    if (!b || !size)
        return -1;
    memset(b, 0, sizeof(*b));
    size = (size + 255u) & ~255u;       /* engine addresses: 256-byte aligned */
    if (DMA_AllocDescriptorTop(&b->dma, (int)size, tag) < 0 || !b->dma.phys_addr ||
        !b->dma.virt_addr || (b->dma.phys_addr & 255u)) {
        IMP_LOG_ERR(TAG, "%s: cannot allocate %u bytes (pa 0x%08x)", tag, size, b->dma.phys_addr);
        if (b->dma.phys_addr)
            DMA_FreePhys(b->dma.phys_addr);
        memset(b, 0, sizeof(*b));
        return -1;
    }
    b->size = size;
    memset(t32_vpu_buf_va(b), 0, size);
    return 0;
}

void OpenIMP_T32_VpuBufFree(T32VpuBuf *b)
{
    if (b && b->dma.phys_addr)
        DMA_FreePhys(b->dma.phys_addr);
    if (b)
        memset(b, 0, sizeof(*b));
}
