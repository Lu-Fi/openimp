#ifndef OPENIMP_T31_OSD_H
#define OPENIMP_T31_OSD_H

/* Draw the visible LINE/RECT/BITMAP regions (CPU) and blend the PIC/COVER
 * regions (IPU) of OSD group `group` into an NV12
 * frame (FrameSource frame info: +0x08 width, +0x0c height, +0x18 phys,
 * +0x1c virt) with the IPU. Used on T31 and on the Helix path (T20, T21,
 * T30). No-op if OPENIMP_T31_OSD=0. */
/* Both return 0, or -1 when the (T21) IPU blend was verified to have no
 * effect after retries: a JPEG caller drops that frame. */
int openimp_t31_osd_apply(int group, void *frame);

/* openimp_t31_osd_apply_ex() flags.  OPENIMP_T31_OSD_DMA_ONLY: the caller
 * guarantees that every later CPU read of this frame invalidates its range
 * first (the encoder's AVPU/Helix input is read by DMA, IVS and the JPEG
 * fan-out copy invalidate themselves), so the per-band cache invalidation
 * after the IPU blend is skipped.  Without it the bands are invalidated as
 * openimp_t31_osd_apply() always did. */
#define OPENIMP_T31_OSD_DMA_ONLY 1u
/* OPENIMP_T31_OSD_NO_RETRY (#873): the T21 idle-wake check still probes the
 * blend and returns -1 when it did not take, but does not repeat the op.  For
 * the video encoder path: each repeat after an idle gap is one more 100 ms
 * kernel IPU timeout (4 x ~110 ms before the first main picture), while the
 * vendor libimp blends once and encodes the picture. */
#define OPENIMP_T31_OSD_NO_RETRY 2u
int openimp_t31_osd_apply_ex(int group, void *frame, unsigned int flags);

#endif
