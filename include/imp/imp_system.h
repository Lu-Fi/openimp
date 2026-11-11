/**
 * IMP System Module
 * 
 * System initialization, binding, and utility functions
 */

#ifndef __IMP_SYSTEM_H__
#define __IMP_SYSTEM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "imp_common.h"

/**
 * Initialize the IMP system
 * 
 * @return 0 on success, negative on error
 */
int IMP_System_Init(void);

/**
 * Exit and cleanup the IMP system
 * 
 * @return 0 on success, negative on error
 */
int IMP_System_Exit(void);

/**
 * Bind two cells together
 * 
 * @param srcCell Source cell
 * @param dstCell Destination cell
 * @return 0 on success, negative on error
 */
int IMP_System_Bind(IMPCell *srcCell, IMPCell *dstCell);

/**
 * Unbind two cells
 * 
 * @param srcCell Source cell
 * @param dstCell Destination cell
 * @return 0 on success, negative on error
 */
int IMP_System_UnBind(IMPCell *srcCell, IMPCell *dstCell);

/**
 * Get IMP library version
 * 
 * @param pstVersion Pointer to version structure
 * @return 0 on success, negative on error
 */
int IMP_System_GetVersion(IMPVersion *pstVersion);

/**
 * Get CPU information string
 * 
 * @return CPU info string (static buffer)
 */
const char *IMP_System_GetCPUInfo(void);

/**
 * Get current system timestamp in microseconds
 * 
 * @return Timestamp in microseconds
 */
uint64_t IMP_System_GetTimeStamp(void);

/**
 * Rebase the system timestamp
 * 
 * Sets a new base timestamp for the system. All subsequent timestamps
 * will be relative to this base.
 * 
 * @param basets Base timestamp in microseconds
 * @return 0 on success, negative on error
 */
int IMP_System_RebaseTimeStamp(uint64_t basets);

/**
 * Get source cell bound to a destination cell
 */
int IMP_System_GetBindbyDest(IMPCell *dstCell, IMPCell *srcCell);

/**
 * Request a named memory pool from reserved media memory.
 */
int IMP_System_MemPoolRequest(int poolId, size_t size, const char *name);

/**
 * Free a previously requested memory pool.
 */
int IMP_System_MemPoolFree(int poolId);

#if defined(PLATFORM_T32)
/*
 * T32 2.x client rmem buffer pools (vendor imp_system.h of SDK 2.0.0).
 * alignsize: non-zero multiple of 16; poolID: the handle CreatePool gives;
 * Alloc falls back to a lookup by name, Free (invalid poolID) to all pools.
 * Errors: 0x800a0013 (parameters), 0x800a0004 (unknown pool), 0x800a000d
 * (out of memory); Alloc returns NULL.  OpenIMP: src/t32/openimp_t32_sys.c.
 */
int32_t IMP_Buf_CreatePool(uint32_t *poolID, uint32_t size, const char *name, uint32_t alignsize);
int32_t IMP_Buf_DestroyPool(uint32_t poolID, const char *name);
void *IMP_Buf_Alloc(uint32_t poolID, const char *poolname, uint32_t size, const char *name,
                    uint32_t alignsize);
int32_t IMP_Buf_Free(uint32_t poolID, void *addr);
uint32_t IMP_Buf_Vaddr2Paddr(void *vaddr);
void *IMP_Buf_Paddr2Vaddr(uint32_t paddr);
#endif

/**
 * Read a 32-bit hardware register
 */
uint32_t IMP_System_ReadReg32(uint32_t addr);

/**
 * Write a 32-bit hardware register
 */
void IMP_System_WriteReg32(uint32_t addr, uint32_t val);

/**
 * Translate physical address to virtual address
 */
void *IMP_System_PhysToVirt(uint32_t phys_addr);

/**
 * Translate virtual address to physical address
 */
uint32_t IMP_System_VirtToPhys(void *virt_addr);

#ifdef __cplusplus
}
#endif

#endif /* __IMP_SYSTEM_H__ */

