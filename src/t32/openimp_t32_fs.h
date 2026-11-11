/*
 * OpenIMP T32 FrameSource sinks (#290, planned): how a bound module (encoder,
 * OSD, IVS: later T32 issues) receives the frames of a FrameSource channel.
 */
#ifndef OPENIMP_T32_FS_H
#define OPENIMP_T32_FS_H

#include <imp/imp_common.h>

/*
 * Called in the channel's frame thread for every delivered frame of the
 * FrameSource channel bound to (devID, grpID).  frame points at the capture
 * buffer itself (NV12, Y stride = width rounded up to 16, UV after
 * ALIGN16(height) luma rows).  Return 0 when done with it, 1 to keep it: the
 * buffer then goes back to the ISP after OpenIMP_FrameSource_ReleaseSinkFrame.
 */
typedef int (*OpenIMP_T32_FrameSink)(void *ctx, int fs_chn, IMPFrameInfo *frame);

/* one sink per (devID, grpID); NULL removes it.  0 or -1 */
int OpenIMP_FrameSource_RegisterSink(int devID, int grpID, OpenIMP_T32_FrameSink sink, void *ctx);
/* give back a frame a sink kept (returned 1 for) */
int OpenIMP_FrameSource_ReleaseSinkFrame(int fs_chn, IMPFrameInfo *frame);

#endif
