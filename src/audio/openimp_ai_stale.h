/*
 * Stale capture fragments of the OSS2 /dev/dsp driver, as a pure helper.
 *
 * The OSS2 audio driver of T10/T20/T21/T30/T31 (ingenic-sdk
 * audio/<soc>/oss2/interface/xb_snd_dsp.c) keeps the capture DMA ring of
 * SND_DSP_DMA_FRAGMENT_MAX_CNT (100) fragments of 10 ms with a DMA index
 * (dma_tracer) and a read index (io_tracer).  Its worker, run from a 20 ms
 * hrtimer, calls ai_dmaaddr_sync_ioaddr() before it copies to the reader:
 *
 *     if (dp->dma_tracer > tmp_io) tmp_io += dp->fragment_cnt;
 *     if (dp->dma_tracer > dma_current) dma_current += dp->fragment_cnt;
 *     if (tmp_io < dma_current + SND_DSP_PIPE_DAM2IO_CNT)
 *         tmp_io = dma_current + SND_DSP_PIPE_DAM2IO_CNT;       (5)
 *
 * This is the overrun guard (reader a whole ring behind), but dma_tracer was
 * set to dma_current just before, so "reader has read everything"
 * (io_tracer == dma_tracer) cannot be told from "ring full".  A reader that
 * keeps up always ends a worker run with io_tracer == dma_tracer.  When the
 * worker then runs again before the DMA has finished another fragment (a
 * late worker followed by one on time, both inside the same 10 ms), the
 * guard moves the read index 5 fragments ahead of the DMA and the driver
 * hands out the other 95 fragments of the ring again.  They were cleared
 * (memset 0) when they were read, so the application gets 0.95 s of digital
 * silence that was never recorded, and all audio after it is 0.95 s late for
 * good (Lu-Fi/openimp#266: 0.9 s of silent AAC frames, then the backlog).
 *
 * Stream start has the same shape in small: the driver starts with the read
 * index 5 fragments before the DMA, so the first 50 ms are cleared fragments.
 *
 * A fragment the codec recorded is practically never all zero (ADC noise), a
 * fragment the driver cleared always is, and the driver hands the cleared
 * ones out as fast as the reader asks while recorded audio cannot arrive
 * faster than real time.  So a run of all-zero fragments is
 *   - held back while it is undecided (at most ~100 ms),
 *   - dropped once it has arrived OPENIMP_AI_STALE_CERT_US ahead of the wall
 *     clock: this run and what follows of the 95, as long as the reads keep
 *     completing faster than real time,
 *   - passed on unchanged when it keeps real-time pace (a source that really
 *     delivers zeros: codec muted, noise gate) or ends before it is decided
 *     (the 50 ms of cleared fragments at stream start stay as they were).
 * Nothing recorded is lost.  While it drops, the reader asks for as many of
 * the remaining stale fragments as one driver task node takes
 * (openimp_ai_stale_next), so the capture is live again after ~0.2 s
 * instead of 0.5 s.  The audio recorded meanwhile arrives late and faster
 * than real time; lag_us tells the caller how late, so that the frame time
 * stamps stay those of consecutive samples.
 *
 * The vendor libimp has no such guard; this is OpenIMP working around the
 * driver.
 */
#ifndef OPENIMP_AI_STALE_H
#define OPENIMP_AI_STALE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* SND_DSP_DMA_FRAGMENT_MAX_CNT - SND_DSP_PIPE_DAM2IO_CNT: the cleared
 * fragments one false overrun hands out */
#define OPENIMP_AI_STALE_FRAGMENTS 95U
/* a zero run this far ahead of the wall clock is stale */
#define OPENIMP_AI_STALE_CERT_US 50000
/* a zero run not stale after this long is real silence */
#define OPENIMP_AI_STALE_WINDOW_US 100000
/* never hold back more than this many fragments */
#define OPENIMP_AI_STALE_HOLD_MAX 32U
/* the reader catches up at twice real time: a lag older than this is void */
#define OPENIMP_AI_STALE_LAG_MAX_US 2000000
/* runs at least this long count as an event */
#define OPENIMP_AI_STALE_EVENT 16U

enum {
    OPENIMP_AI_STALE_NORMAL = 0,    /* signal */
    OPENIMP_AI_STALE_WATCH,         /* in a zero run, undecided or real */
    OPENIMP_AI_STALE_DROP           /* in a stale run */
};

typedef struct {
    int mode;
    int hold;                   /* WATCH: zero fragments are held back */
    unsigned int pending;       /* fragments held back by earlier reads */
    unsigned int window_zero;   /* zero fragments since window_start_us */
    int64_t window_start_us;
    /* a stale run lasts until signal returns, also across a slow read */
    unsigned int stale_left;    /* fragments of the run still to come */
    unsigned int run;           /* stale fragments of the run so far */
    int64_t prev_done_us;       /* when the previous read completed */
    /* While a zero run is held back or dropped the driver goes on recording
     * and hands that audio out late.  lag_us is the age of the newest sample
     * handed out so far (wall time passed minus audio handed out since the
     * run began), for the frame time stamps; it is back at 0 when the reader
     * has caught up. */
    int tracking;
    int64_t lag_us;
    int64_t lag_since_us;       /* when a run was last held back or dropped */
    unsigned int last_run;      /* fragments of the last finished run */
    unsigned long events;       /* finished runs >= OPENIMP_AI_STALE_EVENT */
    unsigned long dropped;      /* stale fragments in total */
} openimp_ai_stale;

static inline void openimp_ai_stale_reset(openimp_ai_stale *state,
                                          int64_t now_us)
{
    memset(state, 0, sizeof(*state));
    state->prev_done_us = now_us;
}

static inline int openimp_ai_stale_zero(const unsigned char *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++)
        if (data[i])
            return 0;
    return 1;
}

static inline void openimp_ai_stale_finish(openimp_ai_stale *state)
{
    if (state->run >= OPENIMP_AI_STALE_EVENT) {
        state->events++;
        state->last_run = state->run;
    }
    state->run = 0;
    state->stale_left = 0;
}

static inline size_t openimp_ai_stale_keep(unsigned char *data,
                                           unsigned char *reference,
                                           size_t out, size_t in, size_t bytes)
{
    if (out != in && bytes) {
        memmove(data + out, data + in, bytes);
        if (reference)
            memmove(reference + out, reference + in, bytes);
    }
    return out + bytes;
}

/* Filters one driver read (whole fragments of fragment_us each, completed
 * at now_us) in place; the reference of the same fragments, if any, with it.
 * Returns the bytes kept; *lead is the number of zero fragments held back
 * from earlier reads that the caller has to queue in front of them.  With
 * drop == 0 nothing is held or removed, stale runs are only counted. */
static inline size_t openimp_ai_stale_filter(openimp_ai_stale *state,
                                             int64_t now_us,
                                             unsigned char *data,
                                             unsigned char *reference,
                                             size_t size, size_t fragment,
                                             int64_t fragment_us, int drop,
                                             unsigned int *lead)
{
    size_t in, out = 0;
    size_t held_from = 0;       /* start of this read's held tail */
    unsigned int held = 0;      /* fragments in it */
    int64_t taken = now_us - state->prev_done_us;
    int active;

    *lead = 0;
    if (!fragment || size < fragment) {
        state->prev_done_us = now_us;
        return size;
    }
    /* stale fragments are ready: after a read that was not clearly faster
     * than real time the zeros are judged anew */
    if (state->mode == OPENIMP_AI_STALE_DROP &&
        taken * 4 >= (int64_t)(size / fragment) * fragment_us * 3)
        state->mode = OPENIMP_AI_STALE_NORMAL;
    for (in = 0; in + fragment <= size; in += fragment) {
        if (openimp_ai_stale_zero(data + in, fragment)) {
            if (state->mode == OPENIMP_AI_STALE_DROP) {
                if (state->stale_left) {
                    state->stale_left--;
                    state->run++;
                    state->dropped++;
                    if (drop)
                        continue;
                } else {
                    /* the whole ring and still zeros: a silent source */
                    openimp_ai_stale_finish(state);
                    state->mode = OPENIMP_AI_STALE_WATCH;
                    state->hold = 0;
                    state->window_start_us = now_us;
                    state->window_zero = 0;
                }
            } else {
                if (state->mode == OPENIMP_AI_STALE_NORMAL) {
                    state->mode = OPENIMP_AI_STALE_WATCH;
                    state->hold = drop;
                    state->pending = 0;
                    /* zeros at the head of a read came in during it */
                    state->window_start_us = in ? now_us : state->prev_done_us;
                    state->window_zero = 0;
                }
                state->window_zero++;
                if (state->hold) {
                    if (!held)
                        held_from = in;
                    held++;
                    continue;
                }
            }
        } else {
            openimp_ai_stale_finish(state);
            /* an undecided run that ends was real: hand it out */
            *lead += state->pending;
            state->pending = 0;
            out = openimp_ai_stale_keep(data, reference, out, held_from,
                                        held * fragment);
            held = 0;
            state->mode = OPENIMP_AI_STALE_NORMAL;
        }
        out = openimp_ai_stale_keep(data, reference, out, in, fragment);
    }
    if (state->mode == OPENIMP_AI_STALE_WATCH) {
        int64_t window = now_us - state->window_start_us;

        if ((int64_t)state->window_zero * fragment_us - window >=
            OPENIMP_AI_STALE_CERT_US) {
            unsigned int gone = state->window_zero;
            unsigned int left = state->run ? state->stale_left
                                           : OPENIMP_AI_STALE_FRAGMENTS;

            state->mode = OPENIMP_AI_STALE_DROP;
            state->stale_left = gone < left ? left - gone : 0;
            state->run += gone;
            state->dropped += gone;
            state->pending = 0;
            state->hold = 0;
            held = 0;
        } else if (window >= OPENIMP_AI_STALE_WINDOW_US ||
                   state->pending + held >= OPENIMP_AI_STALE_HOLD_MAX) {
            *lead += state->pending;
            state->pending = 0;
            out = openimp_ai_stale_keep(data, reference, out, held_from,
                                        held * fragment);
            held = 0;
            state->hold = 0;
            state->window_start_us = now_us;
            state->window_zero = 0;
        } else {
            state->pending += held;
        }
    }
    active = drop && (state->mode == OPENIMP_AI_STALE_DROP ||
                      (state->mode == OPENIMP_AI_STALE_WATCH && state->hold));
    if (state->tracking || active) {
        state->lag_us += taken -
                         (int64_t)(out / fragment + *lead) * fragment_us;
        if (state->lag_us > (int64_t)OPENIMP_AI_STALE_FRAGMENTS * fragment_us)
            state->lag_us = (int64_t)OPENIMP_AI_STALE_FRAGMENTS * fragment_us;
        if (active)
            state->lag_since_us = now_us;
        /* late audio comes faster than real time, so the lag is gone soon;
         * one that stays was not real (silence dropped from a backlog) */
        if (state->lag_us < 0 ||
            now_us - state->lag_since_us >= OPENIMP_AI_STALE_LAG_MAX_US)
            state->lag_us = 0;
        state->tracking = active || state->lag_us > 0;
    }
    state->prev_done_us = now_us;
    return out;
}

/* Fragments to ask the driver for next: `normal`, or while a stale run is
 * dropped as many of its remaining fragments as fit (`most`) - they are
 * ready, so the driver completes the read in its next run. */
static inline size_t openimp_ai_stale_next(const openimp_ai_stale *state,
                                           size_t normal, size_t most)
{
    size_t next;

    if (state->mode != OPENIMP_AI_STALE_DROP || state->stale_left <= normal)
        return normal;
    next = state->stale_left < most ? state->stale_left : most;
    return next > normal ? next : normal;
}

#endif
