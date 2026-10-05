/*
 * Thin T31 stock-driver compatibility surface for the shared encoder.
 *
 * The T31 FrameSource adapter uses the lightweight VBM
 * implementation in kernel_interface.c rather than the incomplete port in
 * core/vbm.c.  Keep the few public/private bookkeeping entry points required
 * by Raptor and the FrameSource lifecycle here so the two VBM
 * implementations do not need to be linked together.
 */

#include <stdint.h>
#include <string.h>

static uint8_t t31_vbm_compat_state[0x20];

/* IMP_FrameSource_SetPool/GetPool/ClearPoolId live in src/dma_alloc.c next
 * to the memory pools they bind channels to. */

/*
 * FrameSource only uses this vendor-global view to set the NCU shutdown flag
 * at offset 0x14.  Buffer ownership itself remains in kernel_interface.c.
 */
void *VBMGetInstance(void)
{
    return t31_vbm_compat_state;
}

int32_t VBMDumpPoolInfo(void)
{
    return 0;
}
