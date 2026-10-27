/* T41 media memory pools in the P2 DMA ledger (host): IMP_MemPool_InitPool /
 * GetById, DMA_PoolAllocDescriptor / IMP_PoolAlloc inside the pool, IMP_Free
 * of a pool buffer (also the one at the pool start) frees only that buffer,
 * a too small pool fails without side effects, OpenIMP_P2_PoolRelease is
 * refused while a buffer lives, 3 create/use/release cycles leave the arena
 * as it was, and an unknown pool id keeps the old arena path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
static void quiet_syslog(int p, const char *f, ...) { (void)p; (void)f; }
#define syslog quiet_syslog
#include "../../src/t40/openimp_p2_dma.c"
#undef syslog

#define RMEM_SIZE (24U * 1024U * 1024U)
#define RMEM_BASE 0x02800000U
static unsigned char arena[16] __attribute__((aligned(4096)));

int main(void)
{
    IMPDMABufferInfo a, b, c, d;
    uint32_t base_used, pool_phys, pool_size;
    int cycle;

    p2_dma.mapping = arena;     /* never dereferenced by the ledger */
    p2_dma.base = RMEM_BASE;
    p2_dma.size = RMEM_SIZE;
    p2_dma.floor = p2_dma.next = 0;

    assert(!DMA_AllocDescriptor(&a, 8 << 20, "isp-mdns"));     /* something long-lived */
    base_used = p2_dma_used_bytes();

    assert(IMP_MemPool_GetById(3) == NULL && IMP_MemPool_GetById(-1) == NULL &&
           IMP_MemPool_GetById(32) == NULL);
    assert(IMP_MemPool_InitPool(32, 4096, "x") == -1 && IMP_MemPool_InitPool(3, 0, "x") == -1);
    /* an unknown pool id keeps the arena (old behaviour) */
    assert(IMP_PoolAlloc(7, &d, 65536, "osd") == 0 && d.pool_id == 7);
    assert(p2_pool_at(d.phys_addr - RMEM_BASE) == NULL);
    assert(!IMP_Free(d.phys_addr));
    assert(p2_dma_used_bytes() == base_used);

    for (cycle = 0; cycle < 3; cycle++) {
        assert(IMP_MemPool_InitPool(3, 4 << 20, "pool3") == 0);
        assert(IMP_MemPool_InitPool(3, 4096, "again") == -1);
        assert(IMP_MemPool_GetById(3) != NULL);
        assert(!OpenIMP_P2_PoolRange(3, &pool_phys, &pool_size) && pool_size == 4U << 20);
        assert(p2_dma_used_bytes() == base_used + (4U << 20));
        /* two 1.5 MB capture buffers inside the pool, the third does not fit */
        assert(!DMA_PoolAllocDescriptor(3, &b, 1536 * 1024, "capture0") && b.pool_id == 3);
        assert(!DMA_PoolAllocDescriptor(3, &c, 1536 * 1024, "capture0"));
        assert(b.phys_addr == pool_phys);
        assert(c.phys_addr >= pool_phys + 1536 * 1024 &&
               c.phys_addr + 1536 * 1024 <= pool_phys + pool_size);
        assert(DMA_PoolAllocDescriptor(3, &d, 1536 * 1024, "capture0") == -1);
        assert(p2_dma_used_bytes() == base_used + (4U << 20));   /* no arena fallback */
        /* the pool cannot go while a buffer lives */
        assert(OpenIMP_P2_PoolRelease(3) == -1);
        /* freeing the buffer at the pool start frees the buffer, not the pool */
        assert(!DMA_FreePhys(b.phys_addr));
        assert(IMP_MemPool_GetById(3) != NULL && p2_pool_live(&p2_pools[3]) == 1);
        assert(DMA_FreePhys(b.phys_addr) == -1);        /* double free refused */
        /* the hole is reused (first fit), IMP_PoolAlloc goes to the pool */
        assert(IMP_PoolAlloc(3, &d, 1024 * 1024, "enc") == 0 && d.phys_addr == pool_phys);
        assert(!IMP_Free(d.virt_addr) && !IMP_Free(c.phys_addr));
        assert(OpenIMP_P2_PoolRelease(3) == 0);
        assert(IMP_MemPool_GetById(3) == NULL && OpenIMP_P2_PoolRelease(3) == -1);
        assert(p2_dma_used_bytes() == base_used);        /* no leak */
    }
    /* a pool larger than the free arena fails cleanly */
    assert(IMP_MemPool_InitPool(4, 20U << 20, "big") == -1 && IMP_MemPool_GetById(4) == NULL);
    assert(p2_dma_used_bytes() == base_used);
    printf("t41 mempool ledger tests passed\n");
    return 0;
}
