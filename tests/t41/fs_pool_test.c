/* T41 IMP_FrameSource_SetPool / GetPool (host): vendor 1.2.5/1.2.6 return
 * codes, the pool binding is independent of CreateChn, a bound channel's
 * capture buffers come from DMA_PoolAllocDescriptor, an unbound one keeps
 * the arena, and a released pool unbinds its channels. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/t40/openimp_p1.c"

int openimp_t31_ivs_source_active(int fs_chn) { (void)fs_chn; return 0; }
void openimp_t31_ivs_capture(int fs_chn, const void *frame) { (void)fs_chn; (void)frame; }
static int arena_allocs, pool_allocs, pool_fail;
static unsigned char mem[3][4096];
int DMA_AllocDescriptor(IMPDMABufferInfo *i, int size, const char *tag)
{ (void)tag; memset(i, 0, sizeof(*i)); i->phys_addr = 0x02800000; i->virt_addr = (uint32_t)(uintptr_t)mem[0]; i->size = (uint32_t)size; arena_allocs++; return 0; }
int OpenIMP_P2_DMAState(uint32_t *a, uint32_t *b) { (void)a; (void)b; return 0; }
int OpenIMP_P2_DMARegion(uint32_t *a, uint32_t *b, void **c) { *a = 0x02800000; *b = 1; *c = mem; return 0; }
int DMA_FreePhys(uint32_t p) { (void)p; return 0; }
int DMA_RmemFlushCache(void *a, uint32_t n, int d) { (void)a; (void)n; (void)d; abort(); }
void *IMP_MemPool_GetById(int id) { return id == 2 || id == 5 ? (void *)mem : NULL; }
int DMA_PoolAllocDescriptor(int id, IMPDMABufferInfo *i, int size, const char *tag)
{
    (void)tag;
    if (pool_fail || (id != 2 && id != 5))
        return -1;
    memset(i, 0, sizeof(*i));
    i->phys_addr = 0x03000000; i->virt_addr = (uint32_t)(uintptr_t)mem[1];
    i->size = (uint32_t)size; i->pool_id = (uint32_t)id;
    pool_allocs++;
    return 0;
}
OpenIMPProfileStamp openimp_profile_begin(void) { OpenIMPProfileStamp z; memset(&z, 0, sizeof z); return z; }
void openimp_profile_end(OpenIMPProfileStage st, OpenIMPProfileStamp t) { (void)st; (void)t; }
void openimp_profile_count(OpenIMPProfileCounter c, uint64_t n) { (void)c; (void)n; }
volatile uint32_t openimp_video_drop_frames;
int openimp_video_drop_set(void (*cb)(void)) { (void)cb; return 0; }
int64_t IMP_System_GetTimeStamp(void) { return 0; }
int64_t OpenIMP_P0_NormalizeMonotonicTimeStamp(uint64_t t) { return (int64_t)t; }

int main(void)
{
    uint32_t phys;
    void *virt;

    /* range and pool checks, vendor codes */
    assert(IMP_FrameSource_SetPool(33, 2) == (int)0x80010001);
    assert(IMP_FrameSource_SetPool(-1, 2) == (int)0x80010001);
    assert(IMP_FrameSource_GetPool(33) == (int)0x80010001);
    assert(IMP_FrameSource_SetPool(0, 9) == (int)0x80010040);   /* no such pool */
    assert(IMP_FrameSource_GetPool(0) == (int)0x80010020);      /* none bound */
    /* bind before the channel exists; a second set is refused */
    assert(IMP_FrameSource_SetPool(1, 2) == 0 && IMP_FrameSource_GetPool(1) == 2);
    assert(IMP_FrameSource_SetPool(1, 5) == (int)0x80010020 && IMP_FrameSource_GetPool(1) == 2);
    assert(IMP_FrameSource_SetPool(32, 5) == 0 && IMP_FrameSource_GetPool(32) == 5);
    /* capture buffers: bound channel -> pool, unbound -> arena */
    assert(!p1_capture_alloc(1, 4096, "capture1", &phys, &virt) && phys == 0x03000000 &&
           pool_allocs == 1 && arena_allocs == 0);
    assert(!p1_capture_alloc(0, 4096, "capture0", &phys, &virt) && phys == 0x02800000 &&
           arena_allocs == 1);
    /* a full pool fails, without falling back to the arena */
    pool_fail = 1;
    assert(p1_capture_alloc(1, 4096, "capture1", &phys, &virt) == -1 && arena_allocs == 1);
    pool_fail = 0;
    /* pool released: its channels are unbound and may be bound again */
    OpenIMP_P1_PoolReleased(2);
    assert(IMP_FrameSource_GetPool(1) == (int)0x80010020 && IMP_FrameSource_GetPool(32) == 5);
    assert(IMP_FrameSource_SetPool(1, 5) == 0);
    printf("t41 FrameSource SetPool/GetPool tests passed\n");
    return 0;
}
