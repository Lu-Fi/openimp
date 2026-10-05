/*
 * vbm_delay.h - the FrameSource delay FIFO of IMP_FrameSource_SetMaxDelay/
 * SetDelay/SetChnFifoAttr and IMP_FrameSource_GetTimedFrame (pure helpers,
 * locking and frame memory are kernel_interface.c's).
 *
 * libimp (T31 1.1.6 on_framesource_group_data_update, FIFO_CACHE_PRIORITY)
 * keeps the newest `delay` captured frames in a FIFO and hands the
 * channel's readers each frame only once `delay` newer ones were captured;
 * GetTimedFrame copies the held frame nearest to a timestamp.  The pool
 * has nrVBs + maxdelay buffers, so the capture keeps its own.
 */
#ifndef OPENIMP_VBM_DELAY_H
#define OPENIMP_VBM_DELAY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VBM_DELAY_MAX_FRAMES 32

typedef struct {
    int idx[VBM_DELAY_MAX_FRAMES];   /* pool indices, oldest at head */
    int head;
    int count;
    int max;                         /* 0: no FIFO */
    int delay;                       /* frames held, <= max */
} VBMDelayRing;

static inline void vbm_delay_reset(VBMDelayRing *ring, int max, int delay)
{
    ring->head = 0;
    ring->count = 0;
    ring->max = max < 0 ? 0 : max > VBM_DELAY_MAX_FRAMES - 1
                                   ? VBM_DELAY_MAX_FRAMES - 1 : max;
    ring->delay = delay < 0 ? 0 : delay > ring->max ? ring->max : delay;
}

static inline int vbm_delay_at(const VBMDelayRing *ring, int n)
{
    return ring->idx[(ring->head + n) % VBM_DELAY_MAX_FRAMES];
}

/* A captured frame (pool index idx) enters the FIFO.  The frames that
 * leave it, oldest first, are written to out[] (at most 2: the regular one
 * and one more after SetDelay lowered the delay); returns their number,
 * 0 while the FIFO is still filling. */
static inline int vbm_delay_push(VBMDelayRing *ring, int idx, int out[2])
{
    int n = 0;

    if (ring->count == VBM_DELAY_MAX_FRAMES) {
        /* cannot happen with max < VBM_DELAY_MAX_FRAMES; pass it through */
        out[0] = idx;
        return 1;
    }
    ring->idx[(ring->head + ring->count) % VBM_DELAY_MAX_FRAMES] = idx;
    ring->count++;
    while (ring->count > ring->delay && n < 2) {
        out[n++] = ring->idx[ring->head];
        ring->head = (ring->head + 1) % VBM_DELAY_MAX_FRAMES;
        ring->count--;
    }
    return n;
}

/* The libimp choice for GetTimedFrame: walking from the oldest frame, the
 * first one at or after ts and the one before it, whichever is nearer (the
 * later one on a tie).  ts[i] is the timestamp of FIFO entry i (oldest 0).
 * Returns the entry, -1 when ts is older than every held frame (libimp
 * fails then; an exact match of the oldest is returned), -2 when every
 * held frame is older than ts (not captured yet). */
static inline int vbm_delay_pick(const int64_t *ts, int count, int64_t target)
{
    int i;

    for (i = 0; i < count; i++) {
        if (ts[i] < target)
            continue;
        if (i == 0)
            return ts[0] == target ? 0 : -1;
        return (uint64_t)(target - ts[i - 1]) < (uint64_t)(ts[i] - target)
                   ? i - 1 : i;
    }
    return -2;
}

/* Copies a frame into a caller's buffer the way libimp does: NV12/NV21
 * packed (luma, then chroma right after it, from a source whose chroma
 * starts after the 16-line aligned luma); any other format as it is.
 * Returns the bytes written (the size to report), 0 when the source is
 * smaller than its format says. */
static inline uint32_t vbm_delay_copy(uint8_t *dst, const uint8_t *src,
                                      uint32_t width, uint32_t height,
                                      uint32_t fourcc, uint32_t size)
{
    if (fourcc == 0x3231564eu /* NV12 */ || fourcc == 0x3132564eu /* NV21 */) {
        uint32_t luma = width * height;
        uint32_t chroma_src = width * ((height + 15u) & ~15u);

        if ((uint64_t)chroma_src + luma / 2u > size)
            return 0;
        if (dst) {
            memcpy(dst, src, luma);
            memcpy(dst + luma, src + chroma_src, luma / 2u);
        }
        return luma + luma / 2u;
    }
    if (dst)
        memcpy(dst, src, size);
    return size;
}

#endif
