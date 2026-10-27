#ifndef OPENIMP_P2_DMA_H
#define OPENIMP_P2_DMA_H
#include <stdint.h>

/* Internal shared arena owner. Callers borrow its mapping, never munmap it.
 * Allocation/free go through DMA_AllocDescriptor/DMA_FreePhys for all users.
 * This owner never calls back into P1, keeping the P1 -> arena lock order.
 */
int OpenIMP_P2_DMARegion(uint32_t *base, uint32_t *size, void **mapping);
int OpenIMP_P2_DMAState(uint32_t *base, uint32_t *used);
#if defined(PLATFORM_T41)
/* media memory pools (openimp_p2_dma.c) */
int IMP_MemPool_InitPool(int pool_id, uint32_t size, const char *name);
void *IMP_MemPool_GetById(int pool_id);
int OpenIMP_P2_PoolRange(int pool_id, uint32_t *phys, uint32_t *size);
int OpenIMP_P2_PoolRelease(int pool_id);
#endif

#endif
