/*
 * Ingenic "eprc" rate control, T33 revision (#295, planned): the entries of
 * src/eprc/eprc_t33_h264.c.  @e is the arena address of the controller block
 * (the vendor's i264e parameter block + 0x360); see eprc_t33_mem.h.
 */
#ifndef OPENIMP_EPRC_T33_H
#define OPENIMP_EPRC_T33_H

#include "eprc/eprc_t33_mem.h"

void eprc_t33_h264_default_set(E33 *X, uint32_t e);   /* eprc_default_set_H264_T33 */
void eprc_t33_h264_video_cfg(E33 *X, uint32_t e);     /* JZ_VPU_RC_VIDEO_CFG_H264_T33 */
void eprc_t33_h264_frame_start(E33 *X, uint32_t e);   /* JZ_VPU_RC_FRAME_START_H264_T33 */
void eprc_t33_h264_frame_end(E33 *X, uint32_t e);     /* JZ_VPU_RC_FRAME_END_H264_T33 */

#endif
