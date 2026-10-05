/*
 * video_drop.h - IMP_ISP_Tuning_SetVideoDrop: the video-loss callback.
 *
 * libimp's ISP tuning daemon reads the ISP frame counter once a second
 * (isp_tuning_func_video_drop, /proc/jz/isp/isp-w02 on T31 1.1.6).  When
 * it has not moved for two ticks the registered callback runs, at most
 * three times (2, 4 and 6 s into the stall), and again only after frames
 * came back.
 *
 * OpenIMP counts the frames its FrameSource dequeues instead of reading a
 * driver file (every SoC, every driver), and only while some channel is
 * streaming and has a reader (openimp_fs_video_demand): a stream that was
 * stopped on purpose is no video loss.
 */
#ifndef OPENIMP_VIDEO_DROP_H
#define OPENIMP_VIDEO_DROP_H

#include <stdint.h>

typedef struct {
    uint32_t last_frames;
    unsigned int stale_ticks;
    unsigned int reports;
} OpenIMPVideoDropState;

/* One monitor tick: frames is the FrameSource frame counter, demand is
 * nonzero while frames are expected.  Returns 1 when the callback is due. */
static inline int openimp_video_drop_step(OpenIMPVideoDropState *state,
                                          uint32_t frames, int demand)
{
    if (!demand || frames != state->last_frames) {
        state->last_frames = frames;
        state->stale_ticks = 0;
        state->reports = 0;
        return 0;
    }
    if (++state->stale_ticks < 2u)
        return 0;
    state->stale_ticks = 0;
    if (state->reports >= 3u)
        return 0;
    state->reports++;
    return 1;
}

/* Every frame the FrameSource dequeues (any channel). */
extern volatile uint32_t openimp_video_drop_frames;

static inline void openimp_video_drop_note_frame(void)
{
    __atomic_add_fetch(&openimp_video_drop_frames, 1u, __ATOMIC_RELAXED);
}

/* Registers (cb != NULL) or clears the callback; the monitor thread runs
 * only while one is registered.  0, or -1 when the thread cannot start. */
int openimp_video_drop_set(void (*cb)(void));

/* Provided by the FrameSource of each SoC: nonzero while a channel is
 * streaming and frames are expected from it. */
int openimp_fs_video_demand(void);

#endif
