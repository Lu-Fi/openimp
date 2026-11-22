#ifndef OPENIMP_T23_HELIX_BS_H
#define OPENIMP_T23_HELIX_BS_H

#include <stdint.h>

#include "dma_alloc.h"

/*
 * T23: one Helix bitstream area for the native H.264 channels and the
 * hardware JPEG encoder, like the stock "vpuBs" that the T23 libimp shares
 * between its H.264 and JPEG jobs under bsbufsem.
 *
 * The first H.264 channel allocates it directly below its EMC scratch (the
 * arena is filled top-down): the Helix core runs past the window end when a
 * picture overflows, and the spill then lands in that scratch, never in a
 * reference picture.  The area is only used while that guard sits right
 * above it; when the guard's channel is destroyed first, this module keeps
 * the scratch allocation as the guard until the last H.264 channel leaves.
 * Further H.264 channels with a window that fits use the same area.
 *
 * The JPEG encoder borrows the area per picture when it exists
 * (JPGC_MAX_BS stops the JPEG core at its limit, so it never spills) and
 * falls back to a buffer of its own otherwise.
 *
 * Lock() serializes the jobs: taken from the command list / slice header to
 * the copy of the bitstream out of the area.
 */

/* 0 and *dma filled (owned by this module) with the lock held when the
 * area exists and holds size bytes; -1 without the lock otherwise. */
int OpenIMP_T23_HelixBs_Lock(uint32_t size, IMPDMABufferInfo *dma);
void OpenIMP_T23_HelixBs_Unlock(void);
/* Size of the area, 0 when no H.264 channel holds it. */
uint32_t OpenIMP_T23_HelixBs_Size(void);
/* Bitstream window of one H.264 channel of w x h (t30_helix_encoder.c):
 * one macroblock-aligned NV12 picture, 64 KiB steps, 256 KiB .. 2 MiB. */
static inline uint32_t t23_bs_window_bytes(uint32_t w, uint32_t h)
{
    uint64_t luma = (((uint64_t)w + 15u) & ~15ull) *
                    (((uint64_t)h + 15u) & ~15ull);
    uint64_t window = ((luma + luma / 2u) + 0xffffu) & ~(uint64_t)0xffffu;

    if (window < (256u << 10))
        window = 256u << 10;
    if (window > (2u << 20))
        window = 2u << 20;
    return (uint32_t)window;
}

/* Size of the shared area: the window of the largest picture among the
 * n FrameSource channel sizes (w[i] x h[i], 0 = no channel) and its page
 * of slack, never less than need; with no channel known the stock vpuBs
 * size class (2 MiB + 4 KiB).  #768: a 720p sensor takes 1412 KiB instead
 * of the 1080p 2052 KiB. */
static inline uint32_t t23_bs_area_bytes(const uint32_t *w, const uint32_t *h,
                                         unsigned int n, uint32_t need)
{
    uint32_t area = 0;
    unsigned int i;

    for (i = 0; i < n; i++) {
        uint32_t window;

        if (!w[i] || !h[i])
            continue;
        window = t23_bs_window_bytes(w[i], h[i]) + 4096u;
        if (window > area)
            area = window;
    }
    if (!area)
        area = (2u << 20) + 4096u;
    return area > need ? area : need;
}

/* helix_jpeg.c: called by the encoder when the area was created. */
void OpenIMP_HelixJpeg_AreaReady(void);

#endif
