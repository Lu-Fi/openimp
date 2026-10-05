/* OEM memory pools (IMP_System_MemPoolRequest / IMP_FrameSource_SetPool):
 *  - the continuous block allocator (256-byte units, first fit, merge on
 *    free, region cleared at init) against what libimp's
 *    Jz_mempool_continuous.c does;
 *  - the real src/dma_alloc.c on a private arena: pool request/release,
 *    blocks out of the pool through DMA_PoolAllocDescriptor / DMA_FreePhys,
 *    the FrameSource channel binding, the 0x94-byte descriptor layout and
 *    that nothing is written outside the descriptor. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#define syslog test_syslog
static int test_syslog(int prio, const char *fmt, ...)
{
    (void)prio;
    (void)fmt;
    return 0;
}
#include "../../src/dma_alloc.c"
#undef syslog

static unsigned char arena[1024 * 1024] __attribute__((aligned(4096)));
static unsigned char region[4096] __attribute__((aligned(256)));

static void test_continuous(void)
{
    MpcPool *p;
    void *a, *b, *c, *d;

    memset(region, 0xaa, sizeof(region));
    assert(!mpc_init(NULL, 4096));
    assert(!mpc_init(region, 0xff));
    p = mpc_init(region, sizeof(region));
    assert(p);
    for (size_t i = 0; i < sizeof(region); i++)
        assert(region[i] == 0);             /* the region is cleared */

    assert(!mpc_alloc(p, 0));
    assert(!mpc_alloc(p, -5));
    assert(!mpc_alloc(p, 4097));
    assert(!mpc_alloc(p, 0x7fffffff));      /* no wrap in the rounding */
    a = mpc_alloc(p, 1);                    /* rounds to 256 */
    b = mpc_alloc(p, 256);
    c = mpc_alloc(p, 257);                  /* rounds to 512 */
    assert(a == region && b == region + 256 && c == region + 512);
    assert(p->used_blocks == 3);

    assert(!mpc_free(p, b));
    assert(mpc_free(p, b) == -1);           /* second free is refused */
    assert(mpc_free(p, region + 1) == -1);  /* not a block start */
    d = mpc_alloc(p, 200);                  /* first fit: the hole at 256 */
    assert(d == region + 256);
    assert(!mpc_free(p, d));
    assert(!mpc_free(p, a));                /* a and the hole merge */
    assert(!mpc_free(p, c));                /* ... and with the rest */
    assert(p->used_blocks == 0);
    d = mpc_alloc(p, 4096);                 /* back to one whole block */
    assert(d == region);
    assert(!mpc_alloc(p, 1));
    assert(!mpc_free(p, d));
    mpc_deinit(p);
}

static void test_pools(void)
{
    struct {
        unsigned char before[256];
        IMPDMABufferInfo info;
        unsigned char after[256];
    } g1, g2;
    IMPDMABufferInfo info3;
    size_t used0;
    uint32_t pool_phys;

    _Static_assert(sizeof(IMPDMABufferInfo) == 0x94, "OEM descriptor size");
    g_dma_initialized = 1;
    g_rmem_supported = 1;
    g_mem_fd = open("/dev/null", O_RDWR);
    g_is_rmem = 1;
    g_rmem_virt_base = arena;
    g_rmem_base_phys = 0x1000000;
    g_rmem_size = sizeof(arena);
    memset(arena, 0xaa, sizeof(arena));

    /* Nothing is bound or requested at the start. */
    assert(IMP_FrameSource_GetPool(0) == -1);
    assert(IMP_FrameSource_SetPool(0, 3) == -1);        /* pool 3 missing */
    assert(IMP_MemPool_GetById(3, NULL) == -1);
    {
        IMPDMABufferInfo x;

        assert(DMA_PoolAllocDescriptor(3, &x, 256, "vbm") == -1);
    }

    /* Request: id range 0..31, at least 0x100 bytes, once. */
    assert(IMP_MemPool_InitPool(-1, 65536, "x") == -1);
    assert(IMP_MemPool_InitPool(32, 65536, "x") == -1);
    assert(IMP_MemPool_InitPool(3, 0xff, "x") == -1);
    assert(IMP_MemPool_InitPool(3, 4u << 20, "big") == -1);   /* > arena */
    used0 = g_rmem_arena.used;
    assert(used0 == 0);
    assert(IMP_MemPool_InitPool(3, 65536, "test") == 0);
    assert(g_rmem_arena.used == 65536);
    assert(IMP_MemPool_InitPool(3, 65536, "test") == -1);
    assert(IMP_MemPool_GetById(3, NULL) == 0);
    pool_phys = g_mem_pools[3].phys_base;
    for (size_t i = 0; i < 65536; i++)
        assert(((unsigned char *)g_mem_pools[3].virt_base)[i] == 0);

    /* FrameSource binding: pool must exist, channel 0..32, bound once. */
    assert(IMP_FrameSource_SetPool(0, 3) == 0);
    assert(IMP_FrameSource_SetPool(0, 3) == -1);
    assert(IMP_FrameSource_SetPool(1, 5) == -1);
    assert(IMP_FrameSource_SetPool(1, -1) == -1);
    assert(IMP_FrameSource_SetPool(33, 3) == -1);
    assert(IMP_FrameSource_SetPool(-1, 3) == -1);
    assert(IMP_FrameSource_SetPool(32, 3) == 0);        /* last channel */
    assert(IMP_FrameSource_GetPool(0) == 3);
    assert(IMP_FrameSource_GetPool(32) == 3);
    assert(IMP_FrameSource_GetPool(1) == -1);
    assert(IMP_FrameSource_GetPool(33) == -1);
    assert(IMP_FrameSource_GetPool(-1) == -1);

    /* Blocks come out of the pool, not the arena; descriptor as the OEM
     * fills it (use count 1, pool id field 0). */
    memset(&g1, 0x5a, sizeof(g1));
    memset(&g2, 0x5a, sizeof(g2));
    assert(DMA_PoolAllocDescriptor(3, &g1.info, 1000, "vbm0") == 0);
    assert(DMA_PoolAllocDescriptor(3, &g2.info, 300, "vbm1") == 0);
    for (size_t i = 0; i < sizeof(g1.before); i++)
        assert(g1.before[i] == 0x5a && g1.after[i] == 0x5a &&
               g2.before[i] == 0x5a && g2.after[i] == 0x5a);
    assert(g1.info.phys_addr == pool_phys);             /* offset 0 */
    assert(g2.info.phys_addr == pool_phys + 1024);      /* 1000 -> 1024 */
    assert(g1.info.size == 1000 && g2.info.size == 300);
    assert(g1.info.flags == 1 && g1.info.pool_id == 0);
    assert(!strcmp(g1.info.tag, "vbm0"));
    assert((uintptr_t)DMA_PhysToVirt(g2.info.phys_addr) ==
           (uintptr_t)g_mem_pools[3].virt_base + 1024);
    assert(g_rmem_arena.used == 65536);
    {
        IMPDMABufferInfo none;

        assert(DMA_PoolAllocDescriptor(3, &none, 65536, "big") == -1);
        assert(DMA_PoolAllocDescriptor(3, &none, 0, "zero") == -1);
        assert(DMA_PoolAllocDescriptor(4, &none, 256, "x") == -1);
    }

    /* Release is refused while blocks are out. */
    assert(IMP_MemPool_Release(3) == -1);
    assert(IMP_MemPool_GetById(3, NULL) == 0);
    assert(DMA_FreePhys(g1.info.phys_addr) == 0);       /* first block */
    {
        IMPDMABufferInfo again;

        assert(DMA_PoolAllocDescriptor(3, &again, 500, "re") == 0);
        assert(again.phys_addr == pool_phys);           /* hole reused */
        assert(DMA_FreePhys(again.phys_addr) == 0);
    }
    assert(DMA_FreePhys(g2.info.phys_addr) == 0);
    assert(g_rmem_arena.used == 65536);

    /* Release gives the rmem back; the id can be requested again. */
    IMP_FrameSource_ClearPoolId();
    assert(IMP_FrameSource_GetPool(0) == -1);
    assert(IMP_MemPool_Release(3) == 0);
    assert(g_rmem_arena.used == 0);
    assert(IMP_MemPool_Release(3) == -1);
    assert(IMP_MemPool_GetById(3, NULL) == -1);
    assert(IMP_MemPool_InitPool(3, 4096, "again") == 0);
    assert(DMA_PoolAllocDescriptor(3, &info3, 4096, "all") == 0);
    assert(DMA_FreePhys(info3.phys_addr) == 0);
    assert(IMP_MemPool_Release(3) == 0);
    assert(g_rmem_arena.used == 0);
}

int main(void)
{
    test_continuous();
    test_pools();
    puts("memory pool tests passed");
    return 0;
}
