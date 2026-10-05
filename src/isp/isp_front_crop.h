/*
 * IMPISPFrontCrop <-> the 5-dword FRONT_CROP control (0x80000e3).
 *
 * The OEM T31 kernel and open-tx-isp (T31; T10/T20/T21 since
 * claude/t1x-crop-csc) copy 20 bytes: {enable, top, left, width, height},
 * enable in the low byte.  The vendor struct has a bool there, so passing
 * the caller's struct would hand the kernel three undefined padding bytes;
 * OpenIMP builds clean words instead.
 *
 * T10/T20/T21 (open-tx-isp): channels whose output fits into the window
 * show it (T21 every channel, T10/T20 the DS channels; shrink only), the
 * others keep the full frame.  A window no channel can show fails (-1),
 * as do odd or out-of-frame values.
 */
#ifndef ISP_FRONT_CROP_H
#define ISP_FRONT_CROP_H

#include <stdint.h>
#include "imp/imp_isp.h"

#define ISP_FRONT_CROP_WORDS 5

static inline void isp_front_crop_pack(const IMPISPFrontCrop *fc, uint32_t *w)
{
    w[0] = fc->fcrop_enable ? 1u : 0u;
    w[1] = fc->fcrop_top;
    w[2] = fc->fcrop_left;
    w[3] = fc->fcrop_width;
    w[4] = fc->fcrop_height;
}

static inline void isp_front_crop_unpack(const uint32_t *w, IMPISPFrontCrop *fc)
{
    fc->fcrop_enable = (w[0] & 0xffu) != 0;
    fc->fcrop_top = w[1];
    fc->fcrop_left = w[2];
    fc->fcrop_width = w[3];
    fc->fcrop_height = w[4];
}

#endif /* ISP_FRONT_CROP_H */
