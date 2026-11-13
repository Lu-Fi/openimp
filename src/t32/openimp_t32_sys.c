/*
 * OpenIMP T32 System leftovers (#299, planned; not a release gate):
 * IMP_Buf_* client buffer pools, IMP_System_MemPoolRequest/Free,
 * IMP_System_ReadReg32/WriteReg32.
 *
 * Behaviour follows what vendor libimp 2.x does (imp_rmem_alloc.c,
 * sys_interface.c; RE reference only, no vendor code copied) and was
 * compared against vendor libimp 2.2.0 in QEMU -M ingenic-t32
 * (tools/t32/t32_misc_sample.c "buf"):
 *
 *   IMP_Buf_CreatePool  poolID and name non-NULL, alignsize non-zero and a
 *                       multiple of 16 (else 0x800a0013); one contiguous rmem
 *                       block of the pool size (0x800a000d when rmem is
 *                       short); *poolID is an opaque handle (the vendor
 *                       hands out the address of its pool record, so does
 *                       OpenIMP).
 *   IMP_Buf_Alloc       name non-NULL, alignsize non-zero multiple of 16;
 *                       the pool by poolID, else by poolname; first fit in
 *                       address order, the start rounded up to alignsize
 *                       from the pool start; NULL on failure.  The vendor's
 *                       root-pool fallback has no memory on 2.2.0 (NULL,
 *                       "kmem hasn't enough memory"); OpenIMP returns NULL too.
 *   IMP_Buf_Free        a valid poolID: that pool only; otherwise a lookup
 *                       over every pool; 0, or 0x800a0013 for an address
 *                       not handed out there.
 *   IMP_Buf_DestroyPool by poolID, else by name; buffers still allocated
 *                       are logged (vendor: "busy") and released with it;
 *                       an unknown pool is logged and still returns 0.
 *   IMP_Buf_Vaddr2Paddr / Paddr2Vaddr  inside any pool's block, else 0/NULL.
 *
 * IMP_System_MemPoolRequest/Free are IMP_MemPool_InitPool/Release
 * (src/dma_alloc.c), as the vendor wraps its Jz_mempool; MemPoolFree also
 * returns -1 for a pool that was never requested.  FrameSource channels bound
 * with IMP_FrameSource_SetPool take their capture buffers from the pool
 * (#445); IMP_Encoder_SetPool (end of this file, #447) only keeps the
 * association.
 *
 * Not here: IMP_Alloc_direct / IMP_Alloc_get_reuse_block (vendor: the
 * zeratul fast-boot reuse of RTOS buffers; no such boot on the open stack).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <imp/imp_system.h>

#include "dma_alloc.h"
#include "imp_log_int.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_sys.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-SYS"

/* src/dma_alloc.c (vendor Jz_mempool names) */
int IMP_MemPool_InitPool(int pool_id, uint32_t size, int flags);
int IMP_MemPool_Release(int pool_id);
int IMP_MemPool_GetById(int pool_id, void *info_out);

#define BUF_ERR_PARAM   0x800a0013
#define BUF_ERR_INIT    0x800a0004
#define BUF_ERR_NOMEM   0x800a000d
#define BUF_ALIGN       16u     /* vendor system_get_alignwidth() */
#define BUF_NAME        32
#define BUF_POOL_MAGIC  0x54334250u     /* "PB3T" */

struct buf_unit {
    struct buf_unit *next;      /* address order */
    uint32_t off, size;
    char name[BUF_NAME];
};

struct buf_pool {
    uint32_t magic;
    struct buf_pool *next;
    char name[BUF_NAME];
    IMPDMABufferInfo dma;       /* the pool block */
    uint32_t size;
    struct buf_unit *units;     /* allocated units, address order */
};

static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
static struct buf_pool *buf_pools;
static struct buf_pool *buf_by_id_locked(uint32_t id)
{
    struct buf_pool *p;

    if (!id)
        return NULL;
    for (p = buf_pools; p; p = p->next)
        if ((uint32_t)(uintptr_t)p == id)
            return p;
    return NULL;
}

static struct buf_pool *buf_by_name_locked(const char *name)
{
    struct buf_pool *p;

    if (!name)
        return NULL;
    for (p = buf_pools; p; p = p->next)
        if (!strncmp(p->name, name, BUF_NAME - 1))
            return p;
    return NULL;
}

int32_t IMP_Buf_CreatePool(uint32_t *poolID, uint32_t size, const char *name, uint32_t alignsize)
{
    struct buf_pool *p;

    if (!poolID || !name || !size) {
        IMP_LOG_ERR(TAG, "IMP_Buf_CreatePool: parameters invalid");
        return (int32_t)BUF_ERR_PARAM;
    }
    if (!alignsize || (alignsize % BUF_ALIGN)) {
        IMP_LOG_ERR(TAG, "IMP_Buf_CreatePool: alignsize(%u) must be aligned with %u bytes",
                    alignsize, BUF_ALIGN);
        return (int32_t)BUF_ERR_PARAM;
    }
    p = calloc(1, sizeof(*p));
    if (!p)
        return (int32_t)BUF_ERR_NOMEM;
    if (DMA_AllocDescriptor(&p->dma, (int)size, name) != 0 || !p->dma.phys_addr) {
        IMP_LOG_ERR(TAG, "IMP_Buf_CreatePool(%s): %u bytes of rmem not available", name, size);
        free(p);
        return (int32_t)BUF_ERR_NOMEM;
    }
    p->magic = BUF_POOL_MAGIC;
    p->size = size;
    strncpy(p->name, name, BUF_NAME - 1);
    pthread_mutex_lock(&buf_lock);
    p->next = buf_pools;
    buf_pools = p;
    pthread_mutex_unlock(&buf_lock);
    *poolID = (uint32_t)(uintptr_t)p;
    return 0;
}

int32_t IMP_Buf_DestroyPool(uint32_t poolID, const char *name)
{
    struct buf_pool *p, **pp;
    struct buf_unit *u;

    pthread_mutex_lock(&buf_lock);
    p = buf_by_id_locked(poolID);
    if (!p)
        p = buf_by_name_locked(name);
    if (!p) {
        /* vendor 2.2.0: logged, and 0 all the same */
        pthread_mutex_unlock(&buf_lock);
        IMP_LOG_ERR(TAG, "IMP_Buf_DestroyPool: poolID and name are invalid");
        return 0;
    }
    for (pp = &buf_pools; *pp; pp = &(*pp)->next)
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    pthread_mutex_unlock(&buf_lock);
    while ((u = p->units)) {
        IMP_LOG_WARN(TAG, "Buf(%s) is busy when the pool(%s) is being destroyed", u->name, p->name);
        p->units = u->next;
        free(u);
    }
    DMA_FreePhys(p->dma.phys_addr);
    p->magic = 0;
    free(p);
    return 0;
}

void *IMP_Buf_Alloc(uint32_t poolID, const char *poolname, uint32_t size, const char *name,
                    uint32_t alignsize)
{
    struct buf_pool *p;
    struct buf_unit *u, *prev, *n;
    uint32_t start = 0;
    void *ret = NULL;

    if (!name) {
        IMP_LOG_ERR(TAG, "IMP_Buf_Alloc: name is NULL");
        return NULL;
    }
    if (!alignsize || (alignsize % BUF_ALIGN) || !size) {
        IMP_LOG_ERR(TAG, "IMP_Buf_Alloc: alignsize(%u) must be aligned with %u bytes",
                    alignsize, BUF_ALIGN);
        return NULL;
    }
    pthread_mutex_lock(&buf_lock);
    p = buf_by_id_locked(poolID);
    if (!p)
        p = buf_by_name_locked(poolname);
    if (!p) {
        /* the vendor falls back to its root pool, which has no memory on a
         * 2.2.0 system ("kmem hasn't enough memory"): NULL, as there */
        pthread_mutex_unlock(&buf_lock);
        IMP_LOG_ERR(TAG, "IMP_Buf_Alloc(%s): no pool %u/%s, kmem hasn't enough memory", name,
                    poolID, poolname ? poolname : "(null)");
        return NULL;
    }
    /* first fit in address order */
    prev = NULL;
    for (u = p->units;; prev = u, u = u->next) {
        uint32_t lo = prev ? prev->off + prev->size : 0;
        uint32_t hi = u ? u->off : p->size;

        start = (lo + alignsize - 1) / alignsize * alignsize;
        if (start >= lo && start <= hi && hi - start >= size)
            break;
        if (!u) {
            start = UINT32_MAX;
            break;
        }
    }
    if (start == UINT32_MAX || !(n = calloc(1, sizeof(*n)))) {
        pthread_mutex_unlock(&buf_lock);
        IMP_LOG_ERR(TAG, "IMP_Buf_Alloc(%s): pool(%s) hasn't enough memory", name, p->name);
        return NULL;
    }
    n->off = start;
    n->size = size;
    strncpy(n->name, name, BUF_NAME - 1);
    n->next = u;
    if (prev)
        prev->next = n;
    else
        p->units = n;
    ret = (void *)(uintptr_t)(p->dma.virt_addr + start);
    pthread_mutex_unlock(&buf_lock);
    return ret;
}

static int buf_free_in_locked(struct buf_pool *p, uint32_t va)
{
    struct buf_unit *u, **pu;

    if (va < p->dma.virt_addr || va >= p->dma.virt_addr + p->size)
        return 0;
    for (pu = &p->units; (u = *pu); pu = &u->next)
        if (p->dma.virt_addr + u->off == va) {
            *pu = u->next;
            free(u);
            return 1;
        }
    return 0;
}

int32_t IMP_Buf_Free(uint32_t poolID, void *addr)
{
    uint32_t va = (uint32_t)(uintptr_t)addr;
    struct buf_pool *p;
    int done = 0;

    if (!addr) {
        IMP_LOG_ERR(TAG, "IMP_Buf_Free: addr is NULL");
        return (int32_t)BUF_ERR_PARAM;
    }
    pthread_mutex_lock(&buf_lock);
    /* a valid poolID is the only place looked at; otherwise every pool */
    p = buf_by_id_locked(poolID);
    if (p)
        done = buf_free_in_locked(p, va);
    else
        for (p = buf_pools; p && !done; p = p->next)
            done = buf_free_in_locked(p, va);
    pthread_mutex_unlock(&buf_lock);
    if (!done) {
        IMP_LOG_ERR(TAG, "Failed to free buf, because the poolID(%u) and addr(%p) are invalid",
                    poolID, addr);
        return (int32_t)BUF_ERR_PARAM;
    }
    return 0;
}

uint32_t IMP_Buf_Vaddr2Paddr(void *vaddr)
{
    uint32_t va = (uint32_t)(uintptr_t)vaddr, pa = 0;
    struct buf_pool *p;

    if (!vaddr)
        return 0;
    pthread_mutex_lock(&buf_lock);
    for (p = buf_pools; p && !pa; p = p->next)
        if (va >= p->dma.virt_addr && va < p->dma.virt_addr + p->size)
            pa = p->dma.phys_addr + (va - p->dma.virt_addr);
    pthread_mutex_unlock(&buf_lock);
    return pa;
}

void *IMP_Buf_Paddr2Vaddr(uint32_t paddr)
{
    uint32_t va = 0;
    struct buf_pool *p;

    if (!paddr)
        return NULL;
    pthread_mutex_lock(&buf_lock);
    for (p = buf_pools; p && !va; p = p->next)
        if (paddr >= p->dma.phys_addr && paddr < p->dma.phys_addr + p->size)
            va = p->dma.virt_addr + (paddr - p->dma.phys_addr);
    pthread_mutex_unlock(&buf_lock);
    return (void *)(uintptr_t)va;
}

/* ---- MemPool (vendor: IMP_MemPool_InitPool / Release) -------------------- */
int IMP_System_MemPoolRequest(int poolId, size_t size, const char *name)
{
    (void)name;
    return IMP_MemPool_InitPool(poolId, (uint32_t)size, 0);
}

int IMP_System_MemPoolFree(int poolId)
{
    IMPDMABufferInfo info;

    if (IMP_MemPool_GetById(poolId, &info) != 0)
        return -1;
    return IMP_MemPool_Release(poolId) < 0 ? -1 : 0;
}

/* ---- registers through /dev/mem (vendor read_reg_32 / write_reg_32) ------ */
static int t32_reg_access(uint32_t addr, uint32_t *value, int write)
{
    long page = sysconf(_SC_PAGESIZE);
    uint32_t base;
    volatile uint32_t *reg;
    void *map;
    int fd;

    if (page <= 0)
        page = 4096;
    if (addr & 3)
        return -1;
    base = addr & ~((uint32_t)page - 1);
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;
    map = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (map == MAP_FAILED)
        return -1;
    reg = (volatile uint32_t *)((uint8_t *)map + (addr - base));
    if (write) {
        *reg = *value;
        __sync_synchronize();
    } else {
        *value = *reg;
    }
    munmap(map, (size_t)page);
    return 0;
}

/* vendor get_cpu_id (T32 2.2.0 0x38efc = T33 2.0.2.1 0x3d28c), cached */
static int t32_cpu_id_calc(void)
{
    uint32_t soc = 0, v = 0, w = 0;
    unsigned chip, b;

    if (t32_reg_access(0x1300002c, &soc, 0) < 0 || (soc >> 28) != 1)
        return -1;
    chip = (soc & 0x0fffffffu) >> 12;
    if (chip == 0x32) {
        if (t32_reg_access(0x13540238, &v, 0) < 0)
            return -1;
        switch (v >> 16) {
        case 0x9999: return 0;
        case 0xaaaa: case 0x3333: return 1;
        case 0x2222: return 2;
        case 0x4444: return 3;
        case 0x5555: return 4;
        case 0x7777: return 5;
        case 0x6666: return 6;
        case 0xffff: return 7;
        case 0x8888: return 8;
        case 0x1111: return 9;
        case 0xcccc: return 10;
        case 0xbbbb: return 11;
        case 0xdddd: return 12;
        case 0xeeee: return -1;
        default: return 21;
        }
    }
    if (chip == 0x33) {
        /* vendor reads 32 bits at the unaligned 0x1354021f and keeps the low byte */
        if (t32_reg_access(0x1354021c, &v, 0) < 0 || t32_reg_access(0x13540220, &w, 0) < 0)
            return -1;
        b = v >> 24;
        switch (b) {
        case 0x99: return 13;
        case 0xaa: case 0x33: return 14;
        case 0x44: return 15;
        case 0x55: return 16;
        case 0x77: return 17;
        case 0x11: return 18;
        case 0xcc: return 19;
        case 0xdd: return 20;
        case 0xee: return -1;
        case 0: return (w & 0x00ffffffu) == 0 ? 15 : 21;
        default: return 21;
        }
    }
    return -1;
}

int t32_get_cpu_id(void)
{
    static int id, done;

    if (!done) {
        id = t32_cpu_id_calc();
        done = 1;
    }
    return id;
}

const char *IMP_System_GetCPUInfo(void)
{
    static const char *const names[] = {
        "T32-LQ", "T32-NQ", "T32-XQ", "T32-A", "T32-ZL", "T32-ZN", "T32-ZX", "T32-VX",
        "T32-LQH", "T32-DL", "T32-VL", "T32-VNP", "T32-VN", "T33-L", "T33-N", "T33-A",
        "T33-ZL", "T33-ZN", NULL, "T33-VL", "T33-VN",
    };
    int id = t32_get_cpu_id();

    if (id < 0 || id >= (int)(sizeof(names) / sizeof(names[0])) || !names[id])
        return "Unknown";
    return names[id];
}

uint32_t IMP_System_ReadReg32(uint32_t addr)
{
    uint32_t v = 0;

    if (t32_reg_access(addr, &v, 0) < 0) {
        IMP_LOG_ERR(TAG, "IMP_System_ReadReg32(0x%08x) failed", addr);
        return 0;
    }
    return v;
}

void IMP_System_WriteReg32(uint32_t addr, uint32_t val)
{
    if (t32_reg_access(addr, &val, 1) < 0)
        IMP_LOG_ERR(TAG, "IMP_System_WriteReg32(0x%08x) failed", addr);
}

/*
 * IMP_Encoder_SetPool / GetPool (#447), vendor libimp T32 2.2.0 / T33 2.0.2.1
 * (disassembly): channel 0..32, the pool must exist (MemPoolRequest), one
 * pool per channel; every failure is -1; GetPool answers the id or -1.  The
 * association is kept for the application; the T32 encoder allocates its
 * stream and reference buffers from the one rmem arena (as the T31 build).
 */
#define T32_ENC_POOL_CHNS 33
static int enc_pool[T32_ENC_POOL_CHNS];
static int enc_pool_init;
static pthread_mutex_t enc_pool_lock = PTHREAD_MUTEX_INITIALIZER;

int IMP_Encoder_SetPool(int chnNum, int poolID)
{
    uint8_t info[256];
    int rc = -1;

    if ((unsigned)chnNum >= T32_ENC_POOL_CHNS || IMP_MemPool_GetById(poolID, info) != 0)
        return -1;
    pthread_mutex_lock(&enc_pool_lock);
    if (!enc_pool_init) {
        memset(enc_pool, 0xff, sizeof(enc_pool));
        enc_pool_init = 1;
    }
    if (enc_pool[chnNum] < 0) {
        enc_pool[chnNum] = poolID;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_pool_lock);
    return rc;
}

int IMP_Encoder_GetPool(int chnNum)
{
    int rc;

    if ((unsigned)chnNum >= T32_ENC_POOL_CHNS)
        return -1;
    pthread_mutex_lock(&enc_pool_lock);
    rc = enc_pool_init ? enc_pool[chnNum] : -1;
    pthread_mutex_unlock(&enc_pool_lock);
    return rc;
}
