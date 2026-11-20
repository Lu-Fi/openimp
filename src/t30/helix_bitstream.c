/* One Helix bitstream buffer shared by all jobs; see helix_bitstream.h. */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "imp_log_int.h"
#include "t30/helix_bitstream.h"

static struct {
    pthread_mutex_t lock;
    IMPDMABufferInfo dma;
    uint32_t pool_size;        /* IMP_Encoder_SetPoolSize, 0 = default */
    uint32_t refused;          /* size that found no rmem (0 = none) */
    uint32_t skipped;          /* requests not retried since */
} helix_bs = { .lock = PTHREAD_MUTEX_INITIALIZER };

#if defined(PLATFORM_T30)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t helix_bs_read_reg(uint32_t phys)
{
    long page = sysconf(_SC_PAGESIZE);
    uint32_t base, value = 0;
    void *m;
    int fd;

    if (page <= 0)
        page = 4096;
    base = phys & ~((uint32_t)page - 1u);
    fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0)
        return 0;
    m = mmap(NULL, (size_t)page, PROT_READ, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (m == MAP_FAILED)
        return 0;
    value = *(volatile uint32_t *)((uint8_t *)m + (phys - base));
    munmap(m, (size_t)page);
    return value;
}

/* Stock T30 1.0.5 EncoderInit (0xa9cf0..0xa9d3c, 0xa9f44..0xa9f90) sizes
 * "vpuBs" by get_cpu_id (0x202cc): T30 (soc id 0x1300002c family 1, id
 * 0x30) with cppsr 0x10000034 = 0x10, or cppsr 1 and subtype (0x13540238
 * bits 31..16) 0x3333 / 0x1111, is cpu id 6 / 7 -> 0xfd200 bytes; the
 * subtypes 0x2222 / 0x4444 / 0x5555 / 0x6666 and any other are cpu id
 * 8..11 -> 0x258000.  An unreadable id takes the larger size. */
static uint32_t helix_bs_t30_default_pool(void)
{
    static uint32_t cached;
    uint32_t soc, cppsr, sub;

    if (cached)
        return cached;
    cached = 0x258000u;
    soc = helix_bs_read_reg(0x1300002cu);
    if ((soc >> 28) == 1u && ((soc >> 12) & 0xffffu) == 0x30u) {
        cppsr = helix_bs_read_reg(0x10000034u) & 0xffu;
        sub = helix_bs_read_reg(0x13540238u) >> 16;
        if (cppsr == 0x10u || (cppsr == 1u && (sub == 0x3333u || sub == 0x1111u)))
            cached = 0xfd200u;
    }
    return cached;
}
#endif

static uint32_t helix_bs_default_pool(void)
{
#if defined(PLATFORM_T30)
    return helix_bs_t30_default_pool();
#else
    return OPENIMP_HELIX_BS_DEFAULT_POOL;
#endif
}

uint32_t OpenIMP_HelixBitstream_PoolSize(void)
{
    uint32_t size;

    pthread_mutex_lock(&helix_bs.lock);
    size = helix_bs.pool_size;
    pthread_mutex_unlock(&helix_bs.lock);
    return size ? size : helix_bs_default_pool();
}

void OpenIMP_HelixBitstream_SetPoolSize(uint32_t size)
{
    pthread_mutex_lock(&helix_bs.lock);
    helix_bs.pool_size = size;
    pthread_mutex_unlock(&helix_bs.lock);
}

#if defined(PLATFORM_T21) && !defined(PLATFORM_T23)
/* Stock (T21 1.0.33; T20 / T10 3.12.0 0x4ac40): stores the size for the next
 * EncoderInit (IMP_System_Init); call it before IMP_System_Init.  Sizes
 * <= 0 are refused. */
int IMP_Encoder_SetPoolSize(int size)
{
    if (size <= 0)
        return -1;
    pthread_mutex_lock(&helix_bs.lock);
    helix_bs.pool_size = (uint32_t)size;
    pthread_mutex_unlock(&helix_bs.lock);
    return 0;
}
#endif

static int helix_bs_alloc(uint32_t size, IMPDMABufferInfo *next)
{
    memset(next, 0, sizeof(*next));
    if (DMA_AllocDescriptorTop(next, (int)size, "helix-bs") != 0 ||
        !next->phys_addr || !next->virt_addr) {
        memset(next, 0, sizeof(*next));
        return -1;
    }
    /* No dirty line of an earlier user may be evicted over what the VPU
     * writes: drop the range from the cache. */
    if (DMA_RmemFlushCache((void *)(uintptr_t)next->virt_addr, size, 2) != 0) {
        DMA_FreePhys(next->phys_addr);
        memset(next, 0, sizeof(*next));
        return -1;
    }
    return 0;
}

/* Called with the lock held.  The buffer is the pool size from the start
 * (stock vpuBs) and only grows when a smaller IMP_Encoder_SetPoolSize
 * leaves less than one H.264 window or one JPEG macroblock row; the larger
 * buffer is allocated before the old one is freed, so the old one is kept
 * if rmem has no room, and a size that failed is tried again only every
 * 64th request, not for every picture. */
static int helix_bs_grow_locked(uint32_t size)
{
    uint32_t old_size = helix_bs.dma.size;
    size_t used = 0, total = 0, largest = 0;
    IMPDMABufferInfo next;

    /* a new buffer has at least the pool size, as the stock vpuBs */
    if (!helix_bs.dma.phys_addr) {
        uint32_t pool = helix_bs.pool_size ? helix_bs.pool_size
                                           : helix_bs_default_pool();

        if (size < pool)
            size = pool;
    }
    if (size > (uint32_t)INT32_MAX - 4095u)
        return -1;
    size = (size + 4095u) & ~4095u;
    if (helix_bs.dma.phys_addr && helix_bs.dma.size >= size)
        return 0;
    /* after a failure, try again only every 64th request */
    if (helix_bs.refused && size >= helix_bs.refused &&
        (++helix_bs.skipped & 63u))
        return -1;
    if (helix_bs_alloc(size, &next) == 0) {
        if (helix_bs.dma.phys_addr)
            DMA_FreePhys(helix_bs.dma.phys_addr);
        helix_bs.dma = next;
        helix_bs.refused = 0;
        (void)DMA_RmemStats(&used, &total, &largest);
        IMP_LOG_INFO("Encoder", "Helix: shared bitstream buffer %u bytes "
                     "at 0x%08x%s (rmem used %zu of %zu, largest free "
                     "block %zu)", size, helix_bs.dma.phys_addr,
                     old_size ? ", grown" : "", used, total, largest);
        return 0;
    }
    if (helix_bs.refused == size)
        return -1;          /* logged before */
    helix_bs.refused = size;
    helix_bs.skipped = 0;
    (void)DMA_RmemStats(&used, &total, &largest);
    IMP_LOG_ERR("Encoder", "Helix: no rmem for a %u-byte shared bitstream "
                "buffer%s (rmem used %zu of %zu, largest free block %zu)",
                size, old_size ? ", keeping the smaller one" : "", used,
                total, largest);
    return -1;
}

int OpenIMP_HelixBitstream_Lock(uint32_t size, IMPDMABufferInfo *dma)
{
    if (!dma || !size)
        return -1;
    pthread_mutex_lock(&helix_bs.lock);
    if (helix_bs_grow_locked(size) != 0) {
        pthread_mutex_unlock(&helix_bs.lock);
        return -1;
    }
    *dma = helix_bs.dma;
    return 0;
}

int OpenIMP_HelixBitstream_LockTimeout(uint32_t size, IMPDMABufferInfo *dma,
                                       uint32_t timeout_ms)
{
    struct timespec deadline;

    if (!dma || !size)
        return -1;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000u;
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    if (pthread_mutex_timedlock(&helix_bs.lock, &deadline) != 0)
        return -EBUSY;
    if (helix_bs_grow_locked(size) != 0) {
        pthread_mutex_unlock(&helix_bs.lock);
        return -1;
    }
    *dma = helix_bs.dma;
    return 0;
}

void OpenIMP_HelixBitstream_Unlock(void)
{
    pthread_mutex_unlock(&helix_bs.lock);
}

int OpenIMP_HelixBitstream_Reserve(uint32_t size)
{
    int ret;

    if (!size)
        return -1;
    pthread_mutex_lock(&helix_bs.lock);
    ret = helix_bs_grow_locked(size);
    pthread_mutex_unlock(&helix_bs.lock);
    return ret;
}

int OpenIMP_HelixBitstream_Init(void)
{
    return OpenIMP_HelixBitstream_Reserve(1u);
}

void OpenIMP_HelixBitstream_Exit(void)
{
    pthread_mutex_lock(&helix_bs.lock);
    if (helix_bs.dma.phys_addr)
        DMA_FreePhys(helix_bs.dma.phys_addr);
    memset(&helix_bs.dma, 0, sizeof(helix_bs.dma));
    helix_bs.refused = 0;
    pthread_mutex_unlock(&helix_bs.lock);
}

uint32_t OpenIMP_HelixBitstream_Size(void)
{
    uint32_t size;

    pthread_mutex_lock(&helix_bs.lock);
    size = helix_bs.dma.size;
    pthread_mutex_unlock(&helix_bs.lock);
    return size;
}
