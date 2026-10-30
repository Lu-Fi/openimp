/* T40 encoder pacing (#149): the pacing clock keeps the plain cadence
 * (due += interval), the early wake-up only follows the capture phase. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../src/t40/p2_t40_pace.h"

#define MARGIN 2000u

/* the poll as p2_polling_stream runs it: the due time before the frame
 * wait is advanced by one interval (or reset to now + interval) */
static uint64_t advance(uint64_t due, uint64_t now, uint64_t interval)
{
    if (!due || now > due + interval)
        return now + interval;
    return due + interval;
}

int main(void)
{
    static const uint32_t fps[] = { 15, 20, 25, 30 };
    unsigned f;

    for (f = 0; f < 4; f++) {
        uint64_t interval = 1000000u / fps[f];
        uint64_t old_due = 0, new_due = 0, now_old, now_new;
        uint64_t sel_old = 0, sel_new = 0;
        int i;

        /* encode rate below the capture rate: frames are queued, the poll
         * returns at the wake-up (arrival == the due time slept to) */
        now_old = now_new = 1000000u;
        for (i = 0; i < 600; i++) {
            old_due = advance(old_due, now_old, interval);
            new_due = advance(new_due, now_new, interval);
            now_old = old_due - interval > now_old ? old_due - interval : now_old;
            now_new = new_due - interval > now_new ? new_due - interval : now_new;
            new_due = p2_t40_next_due(new_due, interval, now_new + 150u, MARGIN);
            assert(new_due == old_due);         /* same cadence, to the us */
            sel_old = old_due; sel_new = new_due;
        }
        assert(sel_old == sel_new);
        /* 600 frames take 600 x interval, not 600 x (interval - 2 ms) */
        assert(new_due - 1000000u <= 601u * interval);
        assert(new_due - 1000000u >= 600u * interval);

        /* capture at the nominal rate with a phase 30 ms after the clock:
         * the clock follows once, then the wait starts a margin before the
         * capture and the cadence is the interval */
        {
            uint64_t due = 0, cap = 5000000u + 30000u, now = 5000000u;
            int shifts = 0;

            for (i = 0; i < 300; i++) {
                uint64_t after;

                due = advance(due, now, interval);
                now = due - interval > now ? due - interval : now;   /* slept */
                if (cap > now)
                    now = cap;                  /* the frame arrives */
                after = p2_t40_next_due(due, interval, now, MARGIN);
                if (after != due)
                    shifts++;
                due = after;
                if (i > 3) {
                    assert(now - (due - interval) <= 2u * MARGIN);
                }
                cap += interval;
            }
            assert(shifts <= 2);                /* a phase step, no drift */
        }
    }
    printf("p2_pace_test OK\n");
    return 0;
}
