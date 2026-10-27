/* P2 DMA adapter for the recovered Allegro/AVPU userspace backend.
 *
 * FrameSource, ISP history and the encoder share one allocation ledger.
 * A snapshot of P1's bump pointer is not a reservation: capture allocations
 * after encoder startup would otherwise overlap live codec buffers.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>
#include "dma_alloc.h"
#include "t40/openimp_p2_dma.h"

#define P2_RMEM_SIZE (96U * 1024U * 1024U)
#define P2_RMEM_FLUSH_IOCTL 0xc00c7200U
#define P2_DMA_MAX_ALLOCS 128U

struct p2_flush_info {
    uint32_t address;
    uint32_t length;
    uint32_t direction;
};

struct p2_dma_allocation {
    uint32_t start;
    uint32_t size;
    int active;
    char tag[16];
};

static struct {
    int fd;
    uint32_t base;
    uint32_t size;
    uint32_t floor;
    uint32_t next;
    void *mapping;
    volatile int lock;
    struct p2_dma_allocation allocations[P2_DMA_MAX_ALLOCS];
} p2_dma = { .fd = -1, .size = P2_RMEM_SIZE };

static void p2_lock(void)
{
    while (__sync_lock_test_and_set(&p2_dma.lock, 1))
        usleep(1000);
}

static void p2_unlock(void)
{
    __sync_lock_release(&p2_dma.lock);
}

static uint32_t align_page(uint32_t value)
{
    return (value + 4095U) & ~4095U;
}

static uint32_t p2_dma_high_water;

static void p2_dma_recompute_next(void)
{
    uint32_t next = p2_dma.floor;
    unsigned int i;

    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i) {
        uint32_t end;

        if (!p2_dma.allocations[i].active)
            continue;
        end = p2_dma.allocations[i].start +
              p2_dma.allocations[i].size;
        if (end > next)
            next = end;
    }
    p2_dma.next = next;
}

/* Placement policy.
 *
 * T40 keeps the original first-fit from the bottom.  On T41 the per-stream
 * capture buffers come and go (FrameSource idles a channel without clients
 * and re-enables it on demand) while codec, ISP, OSD and JPEG buffers live
 * for the whole run.  With a single first-fit ledger, anything allocated
 * while a channel idles (OSD bitmaps growing, the JPEG source copy, the other
 * channel's capture queue) lands in the hole that channel left, and the
 * 1080p queue (2 x 3 MB) then finds no contiguous block any more.  So the
 * T41 arena is split: capture buffers stack up from the bottom, everything
 * else stacks down from the top, each best-fit (smallest gap that fits,
 * closest to its own end).  A capture hole is then only reused by capture
 * buffers, or by long-lived buffers once the middle gap is gone - which is
 * real rmem shortage and is reported as such. */
#if defined(PLATFORM_T41)
#define P2_DMA_SPLIT_ARENA 1
#else
#define P2_DMA_SPLIT_ARENA 0
#endif

struct p2_dma_gap {
    uint32_t start;
    uint32_t end;
};

/* Collect the free gaps of [floor, size) in address order.  Allocations are
 * few (P2_DMA_MAX_ALLOCS), so an insertion sort per call is cheap and keeps
 * the ledger a plain table. */
static unsigned int p2_dma_gaps(struct p2_dma_gap *gaps)
{
    uint32_t starts[P2_DMA_MAX_ALLOCS], ends[P2_DMA_MAX_ALLOCS];
    unsigned int count = 0, gap_count = 0, i;
    uint32_t cursor = p2_dma.floor;

    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i) {
        const struct p2_dma_allocation *allocation = &p2_dma.allocations[i];
        unsigned int j;

        if (!allocation->active)
            continue;
        for (j = count; j > 0 && starts[j - 1] > allocation->start; --j) {
            starts[j] = starts[j - 1];
            ends[j] = ends[j - 1];
        }
        starts[j] = allocation->start;
        ends[j] = allocation->start + allocation->size;
        ++count;
    }
    for (i = 0; i <= count; ++i) {
        uint32_t limit = i < count ? starts[i] : p2_dma.size;

        if (limit > cursor) {
            gaps[gap_count].start = cursor;
            gaps[gap_count].end = limit;
            ++gap_count;
        }
        if (i < count && ends[i] > cursor)
            cursor = align_page(ends[i]);
    }
    return gap_count;
}

static uint32_t p2_dma_used_bytes(void)
{
    uint32_t used = 0;
    unsigned int i;

    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i)
        if (p2_dma.allocations[i].active)
            used += p2_dma.allocations[i].size;
    return used;
}

static uint32_t p2_dma_largest_gap(void)
{
    struct p2_dma_gap gaps[P2_DMA_MAX_ALLOCS + 1U];
    unsigned int count = p2_dma_gaps(gaps), i;
    uint32_t largest = 0;

    for (i = 0; i < count; ++i)
        if (gaps[i].end - gaps[i].start > largest)
            largest = gaps[i].end - gaps[i].start;
    return largest;
}

static int p2_dma_is_capture(const char *tag)
{
    return tag && !strncmp(tag, "capture", 7);
}

/* Pick a page-aligned start for size bytes; see the policy above. */
static int p2_dma_find_gap(uint32_t size, const char *tag,
                           uint32_t *start_out)
{
    struct p2_dma_gap gaps[P2_DMA_MAX_ALLOCS + 1U];
    unsigned int count, i;
    int best = -1, from_top = 0;

    if (!start_out || !size)
        return -1;
    count = p2_dma_gaps(gaps);
    if (P2_DMA_SPLIT_ARENA)
        from_top = !p2_dma_is_capture(tag);
    for (i = 0; i < count; ++i) {
        uint32_t length = gaps[i].end - gaps[i].start;

        if (length < size)
            continue;
        if (!P2_DMA_SPLIT_ARENA) {      /* T40: first fit */
            best = (int)i;
            break;
        }
        if (best < 0 ||
            length < gaps[best].end - gaps[best].start ||
            (length == gaps[best].end - gaps[best].start && from_top))
            best = (int)i;
    }
    if (best < 0)
        return -1;
    if (from_top)
        *start_out = (gaps[best].end - size) & ~4095U;
    else
        *start_out = gaps[best].start;
    if (*start_out < gaps[best].start)
        return -1;
    return 0;
}

static int p2_dma_record_allocation(uint32_t start, uint32_t size,
                                    const char *tag)
{
    unsigned int i;

    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i) {
        if (!p2_dma.allocations[i].active) {
            p2_dma.allocations[i].start = start;
            p2_dma.allocations[i].size = size;
            p2_dma.allocations[i].active = 1;
            memset(p2_dma.allocations[i].tag, 0,
                   sizeof(p2_dma.allocations[i].tag));
            if (tag)
                strncpy(p2_dma.allocations[i].tag, tag,
                        sizeof(p2_dma.allocations[i].tag) - 1U);
            return 0;
        }
    }
    return -1;
}

/* A failed allocation says what is missing and how much more rmem= would
 * cover it (same wording as dma_alloc.c on the other SoCs), then lists the
 * live allocations once per run so fragmentation is visible in the log. */
static int p2_dma_oom_mapped;

static void p2_dma_log_alloc_failed(uint32_t size, const char *tag,
                                    int table_full)
{
    uint32_t arena = p2_dma.size - p2_dma.floor;
    uint32_t used = p2_dma_used_bytes();
    uint32_t free_bytes = arena > used ? arena - used : 0;
    uint32_t largest = p2_dma_largest_gap();
    uint32_t missing = size > largest ? size - largest : 0;
    uint64_t suggest = ((uint64_t)p2_dma.size + missing + 0xfffffU) >> 20;
    unsigned int i;

    if (table_full) {
        syslog(LOG_ERR, "openimp-dma: rmem allocation of %u KB for %s "
               "failed: allocation table full (%u entries), free %u KB",
               (size + 1023U) / 1024U, tag ? tag : "?",
               P2_DMA_MAX_ALLOCS, free_bytes / 1024U);
    } else {
        syslog(LOG_ERR, "openimp-dma: rmem allocation of %u KB for %s "
               "failed: used %u KB of %u KB, free %u KB, largest block "
               "%u KB%s; raise rmem by at least %u KB (suggest rmem=%lluM)",
               (size + 1023U) / 1024U, tag ? tag : "?", used / 1024U,
               arena / 1024U, free_bytes / 1024U, largest / 1024U,
               free_bytes >= size ? " (fragmented)" : "",
               (missing + 1023U) / 1024U, (unsigned long long)suggest);
    }
    if (p2_dma_oom_mapped)
        return;
    p2_dma_oom_mapped = 1;
    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i) {
        const struct p2_dma_allocation *allocation = &p2_dma.allocations[i];

        if (allocation->active)
            syslog(LOG_ERR, "openimp-dma: rmem  +0x%07x %8u B %s",
                   allocation->start, allocation->size,
                   allocation->tag[0] ? allocation->tag : "?");
    }
}

static int p2_rmem_from_cmdline(uint32_t *base_out, uint32_t *size_out)
{
    char command_line[1024];
    const char *value;
    char *end;
    unsigned long long size;
    unsigned long base;
    ssize_t count;
    int fd;

    if (!base_out || !size_out)
        return -1;
    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    count = read(fd, command_line, sizeof(command_line) - 1u);
    close(fd);
    if (count <= 0)
        return -1;
    command_line[count] = '\0';
    value = strstr(command_line, "rmem=");
    if (!value)
        return -1;
    value += 5;
    errno = 0;
    size = strtoull(value, &end, 0);
    if (errno || end == value || !size)
        return -1;
    if (*end == 'K' || *end == 'k') {
        size *= 1024u;
        ++end;
    } else if (*end == 'M' || *end == 'm') {
        size *= 1024u * 1024u;
        ++end;
    } else if (*end == 'G' || *end == 'g') {
        size *= 1024u * 1024u * 1024u;
        ++end;
    }
    if (*end != '@' || size > UINT32_MAX)
        return -1;
    errno = 0;
    base = strtoul(end + 1, &end, 0);
    if (errno || base > UINT32_MAX ||
        (*end != '\0' && *end != ' ' && *end != '\n'))
        return -1;
    *base_out = (uint32_t)base;
    *size_out = (uint32_t)size;
    return *base_out ? 0 : -1;
}

static int p2_dma_prepare(void)
{
    uint32_t base, size, floor = 0;
    const char *offset_text;

    if (p2_dma.mapping)
        return 0;
    if (p2_rmem_from_cmdline(&base, &size) < 0 || size > UINT32_MAX - base)
        return -1;
    offset_text = getenv("OPENIMP_RMEM_START_OFFSET");
    if (offset_text && *offset_text) {
        char *end;
        unsigned long parsed;

        errno = 0;
        parsed = strtoul(offset_text, &end, 0);
        if (errno || end == offset_text || *end || parsed >= size ||
            (parsed & 4095U)) {
            errno = EINVAL;
            return -1;
        }
        floor = (uint32_t)parsed;
    }
    if (floor >= size) {
        errno = EINVAL;
        return -1;
    }
    p2_dma.size = size;
    p2_dma.fd = open("/dev/rmem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (p2_dma.fd < 0)
        return -1;
    p2_dma.mapping = mmap(NULL, p2_dma.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, p2_dma.fd, (off_t)base);
    if (p2_dma.mapping == MAP_FAILED) {
        p2_dma.mapping = NULL;
        close(p2_dma.fd);
        p2_dma.fd = -1;
        return -1;
    }
    p2_dma.base = base;
    p2_dma.floor = floor;
    p2_dma.next = p2_dma.floor;
    return 0;
}

int DMA_AllocDescriptor(IMPDMABufferInfo *out, int size, const char *tag)
{
    uint32_t start;
    uint32_t allocation_size;
    uint32_t used;

    if (!out || size <= 0)
        return -1;
    p2_lock();
    if (p2_dma_prepare() < 0) {
        p2_unlock();
        return -1;
    }
    allocation_size = align_page((uint32_t)size);
    if (p2_dma_find_gap(allocation_size, tag, &start) < 0) {
        p2_dma_log_alloc_failed(allocation_size, tag, 0);
        p2_unlock();
        errno = ENOMEM;
        return -1;
    }
    if (p2_dma_record_allocation(start, allocation_size, tag) < 0) {
        p2_dma_log_alloc_failed(allocation_size, tag, 1);
        p2_unlock();
        errno = ENOMEM;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (tag)
        strncpy(out->tag, tag, sizeof(out->tag) - 1U);
    out->virt_addr = (uint32_t)(uintptr_t)
        ((unsigned char *)p2_dma.mapping + start);
    out->phys_addr = p2_dma.base + start;
    out->size = (uint32_t)size;
    out->flags = 2U;
    p2_dma_recompute_next();
#if defined(P2_DMA_TRACE)
    syslog(LOG_INFO, "openimp-dma: alloc +0x%07x %u B %s", start,
           allocation_size, tag ? tag : "?");
#endif
    /* Sizing aid for the rmem= boot argument: log every new peak of the
     * bytes in use, with what is left and the largest free block. */
    used = p2_dma_used_bytes();
    if (used > p2_dma_high_water) {
        p2_dma_high_water = used;
        syslog(LOG_INFO, "openimp-dma: rmem peak %u KB of %u KB (free %u KB, "
               "largest block %u KB) after %s %u B",
               used / 1024U, (p2_dma.size - p2_dma.floor) / 1024U,
               (p2_dma.size - p2_dma.floor - used) / 1024U,
               p2_dma_largest_gap() / 1024U, tag ? tag : "?",
               allocation_size);
    }
    p2_unlock();
    return 0;
}

int DMA_RmemFlushCache(void *address, uint32_t length, int direction)
{
    struct p2_flush_info info;
    uintptr_t virt = (uintptr_t)address;
    uintptr_t base;
    uint32_t offset;

    if (!address || !length)
        return -1;
    p2_lock();
    if (p2_dma_prepare() < 0) {
        p2_unlock();
        return -1;
    }
#if defined(PLATFORM_T41)
    /* The T41 rmem driver hands the address to dma_sync_single_for_device() as
     * a dma_addr_t, i.e. a physical address, and the kernel turns it into
     * KSEG0 + address.  A user virtual address (0x7xxxxxxx) becomes an
     * unmapped kernel address and oopses in the cache maintenance (T41:
     * "Unable to handle kernel paging request at f5d9d000" from the OSD
     * bitmap write-back).  Translate the rmem mapping to its physical
     * range, clip the range to the reserved memory (callers round small
     * ranges up to 1 MiB) and refuse anything outside it. */
    base = (uintptr_t)p2_dma.mapping;
    if (virt >= base && virt < base + p2_dma.size) {
        offset = (uint32_t)(virt - base);
    } else if (virt >= p2_dma.base && virt < (uintptr_t)p2_dma.base + p2_dma.size) {
        offset = (uint32_t)virt - p2_dma.base;     /* already physical */
    } else {
        p2_unlock();
        errno = EINVAL;
        return -1;
    }
    if (length > p2_dma.size - offset)
        length = p2_dma.size - offset;
    info.address = p2_dma.base + offset;
#else
    /* T40's rmem driver runs dma_cache_sync() on the caller's virtual
     * address, which is valid for any mapping of the calling process. */
    (void)base;
    (void)offset;
    info.address = (uint32_t)virt;
#endif
    info.length = length;
    info.direction = (uint32_t)direction;
    direction = ioctl(p2_dma.fd, P2_RMEM_FLUSH_IOCTL, &info);
    p2_unlock();
    return direction;
}

int OpenIMP_P2_DMAState(uint32_t *base, uint32_t *used)
{
    int ready;

    p2_lock();
    if (base)
        *base = p2_dma.base;
    if (used)
        *used = p2_dma_used_bytes();
    ready = p2_dma.mapping != NULL;
    p2_unlock();
    return ready ? 0 : -1;
}

/* Diagnostic for test tools (tools/bstest): bytes in use, the arena size
 * (from the floor), the largest free block, and the bytes/count of live
 * allocations whose tag starts with tag_prefix. */
int OpenIMP_P2_DMAUsage(uint32_t *used, uint32_t *total, uint32_t *largest,
                        const char *tag_prefix, uint32_t *tag_bytes,
                        uint32_t *tag_count)
{
    uint32_t tb = 0, tc = 0, i;
    size_t n = tag_prefix ? strlen(tag_prefix) : 0;
    int ready;

    p2_lock();
    ready = p2_dma.mapping != NULL;
    if (used)
        *used = p2_dma_used_bytes();
    if (total)
        *total = p2_dma.size - p2_dma.floor;
    if (largest)
        *largest = p2_dma_largest_gap();
    for (i = 0; n && i < P2_DMA_MAX_ALLOCS; ++i) {
        if (p2_dma.allocations[i].active &&
            strncmp(p2_dma.allocations[i].tag, tag_prefix, n) == 0) {
            tb += p2_dma.allocations[i].size;
            tc++;
        }
    }
    if (tag_bytes)
        *tag_bytes = tb;
    if (tag_count)
        *tag_count = tc;
    p2_unlock();
    return ready ? 0 : -1;
}

int OpenIMP_P2_DMARegion(uint32_t *base, uint32_t *size, void **mapping)
{
    int result;

    if (!base || !size || !mapping)
        return -1;
    p2_lock();
    result = p2_dma_prepare();
    if (!result) {
        *base = p2_dma.base;
        *size = p2_dma.size;
        *mapping = p2_dma.mapping;
    }
    p2_unlock();
    return result;
}

/* Public T40 allocator compatibility.  libimp consumers use both the
 * descriptor ABI (IMP_Alloc(info, size, tag) -> status) and older pointer ABI
 * (IMP_Alloc(size) -> virtual address).  A valid MIPS userspace pointer is
 * well above the largest supported allocation, so the first argument makes
 * the two forms unambiguous on this target. */
uintptr_t IMP_Alloc(void *info_or_size, intptr_t size, char *tag)
{
    uintptr_t first = (uintptr_t)info_or_size;
    IMPDMABufferInfo info;

    if (first > 0 && first <= P2_RMEM_SIZE) {
        if (DMA_AllocDescriptor(&info, (int)first, "imp") != 0)
            return (uintptr_t)0;
        return (uintptr_t)info.virt_addr;
    }
    return (uintptr_t)DMA_AllocDescriptor((IMPDMABufferInfo *)info_or_size,
                                           (int)size, tag);
}

uintptr_t IMP_PoolAlloc(int pool_id, void *info_or_size, intptr_t size,
                        char *tag)
{
    uintptr_t second = (uintptr_t)info_or_size;
    IMPDMABufferInfo info;
    int result;

#if defined(PLATFORM_T41)
    /* an existing pool (IMP_System_MemPoolRequest): the buffer comes from
     * it, as in the vendor library; an unknown id keeps the arena */
    if (IMP_MemPool_GetById(pool_id)) {
        if (second > 0 && second <= P2_RMEM_SIZE) {
            if (DMA_PoolAllocDescriptor(pool_id, &info, (int)second, "pool") != 0)
                return (uintptr_t)0;
            return (uintptr_t)info.virt_addr;
        }
        return (uintptr_t)DMA_PoolAllocDescriptor(pool_id,
            (IMPDMABufferInfo *)info_or_size, (int)size, tag);
    }
#endif
    if (second > 0 && second <= P2_RMEM_SIZE) {
        result = DMA_AllocDescriptor(&info, (int)second, "pool");
        if (result != 0)
            return (uintptr_t)0;
        info.pool_id = (uint32_t)pool_id;
        return (uintptr_t)info.virt_addr;
    }
    result = DMA_AllocDescriptor((IMPDMABufferInfo *)info_or_size,
                                 (int)size, tag);
    if (result == 0)
        ((IMPDMABufferInfo *)info_or_size)->pool_id = (uint32_t)pool_id;
    return (uintptr_t)result;
}

#if defined(PLATFORM_T41)
/* Media memory pools (vendor T41 1.2.5/1.2.6 Jz_mempool: IMP_System_MemPoolRequest
 * -> IMP_MemPool_InitPool carves one contiguous block out of rmem;
 * IMP_PoolAlloc places buffers inside it; IMP_FrameSource_SetPool binds a
 * channel's capture buffers to it).  A pool is one allocation of the arena
 * ledger (tag "mempool", placed from the top like other long-lived buffers);
 * its buffers are kept in a small per-pool ledger, first fit, page aligned.
 * IMP_Free / DMA_FreePhys / IMP_PoolFree of an address inside a pool free the
 * pool buffer, never the pool itself (a buffer at the pool start has the
 * pool's address).  Nothing here runs unless an application creates a pool:
 * the default path is unchanged. */
#define P2_MEMPOOLS 32
#define P2_POOL_ALLOCS 16

struct p2_mempool {
    int active;
    uint32_t start;                 /* arena offset */
    uint32_t size;                  /* page aligned */
    char name[32];
    struct { uint32_t start, size; } buf[P2_POOL_ALLOCS];   /* pool offsets, size 0 = free */
};

static struct p2_mempool p2_pools[P2_MEMPOOLS];

int IMP_Free(uintptr_t address);

/* Under p2_lock: the pool holding arena offset off, or NULL. */
static struct p2_mempool *p2_pool_at(uint32_t off)
{
    unsigned int i;

    for (i = 0; i < P2_MEMPOOLS; ++i)
        if (p2_pools[i].active && off >= p2_pools[i].start &&
            off - p2_pools[i].start < p2_pools[i].size)
            return &p2_pools[i];
    return NULL;
}

static unsigned int p2_pool_live(const struct p2_mempool *pool)
{
    unsigned int i, n = 0;

    for (i = 0; i < P2_POOL_ALLOCS; ++i)
        n += pool->buf[i].size != 0;
    return n;
}

int IMP_MemPool_InitPool(int pool_id, uint32_t size, const char *name)
{
    IMPDMABufferInfo info;
    struct p2_mempool *pool;
    uint32_t off;

    if (pool_id < 0 || pool_id >= P2_MEMPOOLS || !size || size > INT32_MAX)
        return -1;
    p2_lock();
    if (p2_pools[pool_id].active) {
        p2_unlock();
        syslog(LOG_ERR, "openimp-dma: mempool %d already requested", pool_id);
        return -1;
    }
    p2_unlock();
    if (DMA_AllocDescriptor(&info, (int)size, "mempool") != 0) {
        syslog(LOG_ERR, "openimp-dma: mempool %d: %u B not available", pool_id, size);
        return -1;
    }
    p2_lock();
    off = info.phys_addr - p2_dma.base;
    pool = &p2_pools[pool_id];
    if (pool->active) {             /* raced with a second request */
        p2_unlock();
        (void)IMP_Free((uintptr_t)info.phys_addr);
        return -1;
    }
    memset(pool, 0, sizeof(*pool));
    pool->active = 1;
    pool->start = off;
    pool->size = align_page(size);
    if (name)
        strncpy(pool->name, name, sizeof(pool->name) - 1U);
    p2_unlock();
    syslog(LOG_INFO, "openimp-dma: mempool %d \"%s\" %u B at 0x%08x",
           pool_id, name ? name : "", size, info.phys_addr);
    return 0;
}

/* Vendor: non-NULL when the pool exists (IMP_FrameSource_SetPool checks it). */
void *IMP_MemPool_GetById(int pool_id)
{
    void *found = NULL;

    if (pool_id < 0 || pool_id >= P2_MEMPOOLS)
        return NULL;
    p2_lock();
    if (p2_pools[pool_id].active)
        found = &p2_pools[pool_id];
    p2_unlock();
    return found;
}

/* Physical range of a pool (tests, FrameSource logs); 0 on success. */
int OpenIMP_P2_PoolRange(int pool_id, uint32_t *phys, uint32_t *size)
{
    int result = -1;

    if (pool_id < 0 || pool_id >= P2_MEMPOOLS)
        return -1;
    p2_lock();
    if (p2_pools[pool_id].active) {
        if (phys)
            *phys = p2_dma.base + p2_pools[pool_id].start;
        if (size)
            *size = p2_pools[pool_id].size;
        result = 0;
    }
    p2_unlock();
    return result;
}

/* Release a pool (beyond vendor: the vendor T41 library has no public call
 * for it).  Refused while a buffer of the pool is still allocated. */
int OpenIMP_P2_PoolRelease(int pool_id)
{
    uint32_t phys;
    unsigned int live;

    if (pool_id < 0 || pool_id >= P2_MEMPOOLS)
        return -1;
    p2_lock();
    if (!p2_pools[pool_id].active) {
        p2_unlock();
        return -1;
    }
    live = p2_pool_live(&p2_pools[pool_id]);
    if (live) {
        p2_unlock();
        syslog(LOG_ERR, "openimp-dma: mempool %d: %u buffers still allocated",
               pool_id, live);
        return -1;
    }
    phys = p2_dma.base + p2_pools[pool_id].start;
    memset(&p2_pools[pool_id], 0, sizeof(p2_pools[pool_id]));
    p2_unlock();
    return IMP_Free((uintptr_t)phys);
}

/* A buffer of size bytes inside pool pool_id (first fit); -1 when the pool
 * does not exist or has no room (nothing allocated then). */
int DMA_PoolAllocDescriptor(int pool_id, IMPDMABufferInfo *out, int size,
                            const char *tag)
{
    struct p2_mempool *pool;
    uint32_t need, cursor = 0;
    int slot = -1;
    unsigned int i;

    if (!out || size <= 0 || pool_id < 0 || pool_id >= P2_MEMPOOLS)
        return -1;
    need = align_page((uint32_t)size);
    p2_lock();
    pool = &p2_pools[pool_id];
    if (!pool->active) {
        p2_unlock();
        return -1;
    }
    for (i = 0; i < P2_POOL_ALLOCS; ++i)
        if (!pool->buf[i].size && slot < 0)
            slot = (int)i;
    /* lowest start at or above cursor that overlaps nothing */
    while (slot >= 0) {
        uint32_t next = cursor;

        for (i = 0; i < P2_POOL_ALLOCS; ++i) {
            uint32_t b0 = pool->buf[i].start, b1 = b0 + pool->buf[i].size;

            if (pool->buf[i].size && b0 < cursor + need && b1 > cursor && b1 > next)
                next = align_page(b1);
        }
        if (next == cursor)
            break;
        cursor = next;
    }
    if (slot < 0 || cursor > pool->size || pool->size - cursor < need) {
        syslog(LOG_ERR, "openimp-dma: mempool %d \"%s\" (%u B): no room for "
               "%s %u B", pool_id, pool->name, pool->size, tag ? tag : "?",
               need);
        p2_unlock();
        errno = ENOMEM;
        return -1;
    }
    pool->buf[slot].start = cursor;
    pool->buf[slot].size = need;
    memset(out, 0, sizeof(*out));
    if (tag)
        strncpy(out->tag, tag, sizeof(out->tag) - 1U);
    out->virt_addr = (uint32_t)(uintptr_t)
        ((unsigned char *)p2_dma.mapping + pool->start + cursor);
    out->phys_addr = p2_dma.base + pool->start + cursor;
    out->size = (uint32_t)size;
    out->flags = 2U;
    out->pool_id = (uint32_t)pool_id;
    p2_unlock();
    return 0;
}

/* Under p2_lock: free the pool buffer at arena offset off.  1 = it was a
 * pool address (result in *result), 0 = not inside a pool. */
static int p2_pool_free_locked(uint32_t off, int *result)
{
    struct p2_mempool *pool = p2_pool_at(off);
    unsigned int i;

    if (!pool)
        return 0;
    *result = -1;
    for (i = 0; i < P2_POOL_ALLOCS; ++i) {
        if (pool->buf[i].size && pool->start + pool->buf[i].start == off) {
            pool->buf[i].start = 0;
            pool->buf[i].size = 0;
            *result = 0;
            break;
        }
    }
    return 1;
}
#endif

int IMP_Free(uintptr_t address)
{
    uintptr_t virtual_base;
    uint32_t offset;
    unsigned int i;
    int result = -1;

    if (!address)
        return -1;
    p2_lock();
    if (p2_dma_prepare() != 0) {
        p2_unlock();
        return -1;
    }
    virtual_base = (uintptr_t)p2_dma.mapping;
    if (address >= p2_dma.base && address < p2_dma.base + p2_dma.size)
        offset = (uint32_t)(address - p2_dma.base);
    else if (address >= virtual_base && address < virtual_base + p2_dma.size)
        offset = (uint32_t)(address - virtual_base);
    else {
        p2_unlock();
        return -1;
    }

#if defined(PLATFORM_T41)
    if (p2_pool_free_locked(offset, &result)) {
        p2_unlock();
        return result;
    }
#endif
    for (i = 0; i < P2_DMA_MAX_ALLOCS; ++i) {
        struct p2_dma_allocation *allocation = &p2_dma.allocations[i];

        if (!allocation->active || offset != allocation->start)
            continue;
#if defined(P2_DMA_TRACE)
        syslog(LOG_INFO, "openimp-dma: free  +0x%07x %u B %s",
               allocation->start, allocation->size, allocation->tag);
#endif
        allocation->active = 0;
        allocation->start = 0;
        allocation->size = 0;
        p2_dma_recompute_next();
        result = 0;
        break;
    }
    p2_unlock();
    return result;
}

int DMA_FreePhys(uint32_t phys_addr)
{
    return IMP_Free((uintptr_t)phys_addr);
}

int IMP_FlushCache(void *address, uint32_t length)
{
    return DMA_RmemFlushCache(address, length, 1);
}

/* The mapping is set up once and never torn down, so once it exists the
 * translations below read it without the lock; the first-time setup must
 * not race a concurrent allocation's p2_dma_prepare (two /dev/rmem
 * mappings, one of them leaked, otherwise). */
static int p2_dma_ready(void)
{
    int result;

    if (p2_dma.mapping)
        return 0;
    p2_lock();
    result = p2_dma_prepare();
    p2_unlock();
    return result;
}

void *IMP_Phys_to_Virt(uint32_t physical)
{
    if (p2_dma_ready() != 0 || physical < p2_dma.base ||
        physical >= p2_dma.base + p2_dma.size)
        return NULL;
    return (unsigned char *)p2_dma.mapping + (physical - p2_dma.base);
}

uint32_t IMP_Virt_to_Phys(void *virtual_address)
{
    uintptr_t address = (uintptr_t)virtual_address;
    uintptr_t base;

    if (p2_dma_ready() != 0)
        return 0;
    base = (uintptr_t)p2_dma.mapping;
    if (address < base || address >= base + p2_dma.size)
        return 0;
    return p2_dma.base + (uint32_t)(address - base);
}

/* dma_alloc.h translations for code shared with the other platforms (the
 * T23-family OSD on T41): an rmem address maps, anything else is returned
 * as it is, like dma_alloc.c does. */
void *DMA_PhysToVirt(uint32_t phys_addr)
{
    return IMP_Phys_to_Virt(phys_addr);
}

uint32_t DMA_VirtToPhys(const void *virt_addr)
{
    uint32_t phys;

    if (!virt_addr)
        return 0;
    phys = IMP_Virt_to_Phys((void *)(uintptr_t)virt_addr);
    return phys ? phys : (uint32_t)(uintptr_t)virt_addr;
}

void IMP_PoolFree(void *address)
{
    (void)IMP_Free((uintptr_t)address);
}

int IMP_PoolFlushCache(void *address, uint32_t length)
{
    return IMP_FlushCache(address, length);
}

void *IMP_PoolPhys_to_Virt(uint32_t physical)
{
    return IMP_Phys_to_Virt(physical);
}

uint32_t IMP_PoolVirt_to_Phys(void *virtual_address)
{
    return IMP_Virt_to_Phys(virtual_address);
}
