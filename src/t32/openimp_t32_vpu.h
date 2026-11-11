/*
 * OpenIMP T32 soc_vpu channel layer (#291, planned): the kernel's
 * /dev/soc_vpu (4.4.94 drivers/video/ingenic_vpu) shared by the JPEG v1.2
 * engine ("helix0") and HERA ("radix0").  A channel is one CHANNEL_REQ'd
 * kernel channel_node; a job is a physical register image (JPEG: the
 * jpeg_reg_info, HERA: the command list) run on the engine its vpu_id
 * names.  Buffers the engines read or write come from rmem (dma_alloc.c).
 */
#ifndef OPENIMP_T32_VPU_H
#define OPENIMP_T32_VPU_H

#include <stdint.h>

#include "dma_alloc.h"
#include "t32/openimp_t32_ioctl.h"

typedef struct {
    T32IocChannelNode node;     /* the kernel's copy, as returned */
    int requested;              /* CHANNEL_REQ done, no REL yet */
    int started;                /* CHANNEL_START done, vpu held (node.vlist) */
} T32VpuChannel;

typedef struct {                /* what a finished job reports */
    long rc;                    /* ioctl result: 0, a positive VPU_RET_*, -1 */
    uint32_t status;            /* engine interrupt status */
    uint32_t output_len;        /* bitstream bytes */
    uint32_t hw_time;           /* ms */
} T32VpuResult;

typedef struct {                /* one rmem buffer */
    IMPDMABufferInfo dma;
    uint32_t size;
} T32VpuBuf;

/* /dev/soc_vpu, shared and counted: 0 or -1 */
int OpenIMP_T32_VpuOpen(void);
void OpenIMP_T32_VpuClose(void);

/* CHANNEL_REQ / CHANNEL_REL (codecdir OI_T32_VPU_CODEC_*): 0 or -1 */
int OpenIMP_T32_VpuRequest(T32VpuChannel *c, uint32_t codecdir);
int OpenIMP_T32_VpuRelease(T32VpuChannel *c);

/* one job on the engine vpu_id (OI_T32_VPU_*_ID | 1), image at job_pa:
 * RUN = request vpu, reset, start, wait, release in one call */
int OpenIMP_T32_VpuRun(T32VpuChannel *c, uint32_t vpu_id, uint32_t job_pa, T32VpuResult *r);
/* the same in two steps: START (vpu held), then WAIT_COMPLETE (released) */
int OpenIMP_T32_VpuStart(T32VpuChannel *c, uint32_t vpu_id, uint32_t job_pa);
int OpenIMP_T32_VpuWait(T32VpuChannel *c, T32VpuResult *r);
/* HERA sections: WAIT_BSFULL_COMPLETE after START (vpu stays held while it
 * returns OI_T32_VPU_RET_HERA_BSFULL), SET_BSFULL_REG to continue.  Only on
 * a started channel: the kernel dereferences the node's vpu pointer. */
int OpenIMP_T32_VpuWaitBsFull(T32VpuChannel *c, T32VpuResult *r);
int OpenIMP_T32_VpuSetBsFull(T32VpuChannel *c);

/* FLUSH_CACHE on a user range (OI_T32_VPU_FLUSH_WBACK / _INV) */
int OpenIMP_T32_VpuFlush(const void *va, uint32_t len, uint32_t dir);
/* WOR_VPU_REG: read / write an engine register by physical address */
int OpenIMP_T32_VpuReadReg(uint32_t paddr, uint32_t *value);
int OpenIMP_T32_VpuWriteReg(uint32_t paddr, uint32_t value);

/* rmem buffers for jobs and bitstreams (long-lived: top of the arena) */
int OpenIMP_T32_VpuBufAlloc(T32VpuBuf *b, uint32_t size, const char *tag);
void OpenIMP_T32_VpuBufFree(T32VpuBuf *b);
static inline void *t32_vpu_buf_va(const T32VpuBuf *b) { return (void *)(uintptr_t)b->dma.virt_addr; }
static inline uint32_t t32_vpu_buf_pa(const T32VpuBuf *b) { return b->dma.phys_addr; }

#endif
