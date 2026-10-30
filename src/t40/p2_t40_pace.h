#ifndef OPENIMP_P2_T40_PACE_H
#define OPENIMP_P2_T40_PACE_H

#include <stdint.h>

/* T40 encoder pacing (#149).  The pacing clock advances by exactly one frame
 * interval per delivered frame (due += interval): that fixes the output
 * rate and the synthetic time stamps.  The margin only lets the clock follow
 * the capture phase: when the frame arrived more than twice the margin after
 * the point the poll was waiting for (the capture runs later than the pacing
 * clock), the clock is moved to the arrival so the next wait begins a
 * margin before the next capture instead of probing for half an interval.
 * A frame that was already queued (arrival at the wake-up) leaves the clock
 * untouched, so the cadence is the plain accumulation. */
static inline uint64_t p2_t40_next_due(uint64_t due_next, uint64_t interval_us,
                                       uint64_t arrival_us, uint64_t margin_us)
{
    uint64_t waited_for;

    if (interval_us <= margin_us || due_next < interval_us)
        return due_next;
    waited_for = due_next - interval_us;        /* the due time of this frame */
    if (arrival_us > waited_for + 2u * margin_us)
        return arrival_us + interval_us - margin_us;
    return due_next;
}

#endif /* OPENIMP_P2_T40_PACE_H */
