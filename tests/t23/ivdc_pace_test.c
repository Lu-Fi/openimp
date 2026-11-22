/* #807: vendor frame-count mask (libimp 1.3.0 do_channel_process,
 * libimp_all.c:33612-33640) and the input rate from frame-start times. */
#include <assert.h>
#include <stdio.h>
#include "../../src/t23/openimp_t23_ivdc_pace.h"

static int count_kept(T23IvdcPace *p, int n, int *maxgap)
{
    int i, kept = 0, gap = 0;

    *maxgap = 0;
    for (i = 0; i < n; i++) {
        if (t23_ivdc_pace_take(p)) {
            kept++;
            gap = 0;
        } else if (++gap > *maxgap)
            *maxgap = gap;
    }
    return kept;
}

int main(void)
{
    T23IvdcPace p = {0};
    int gap, i;
    unsigned int fps = 0;

    /* 30 -> 25: kept positions 0,1,2,4,5,6,7,8,10,... one of six skipped */
    t23_ivdc_pace_build(&p, 30, 25);
    assert(p.len == 30);
    assert(((p.mask >> 29) & 1) && ((p.mask >> 28) & 1) && ((p.mask >> 27) & 1) &&
           !((p.mask >> 26) & 1) && ((p.mask >> 25) & 1));
    assert(count_kept(&p, 300, &gap) == 250 && gap == 1);
    /* 25 -> 15, 30 -> 10, equal and faster output keep all */
    t23_ivdc_pace_build(&p, 25, 15);
    assert(count_kept(&p, 250, &gap) == 150 && gap <= 2);
    t23_ivdc_pace_build(&p, 30, 10);
    assert(count_kept(&p, 300, &gap) == 100 && gap == 2);
    t23_ivdc_pace_build(&p, 25, 25);
    assert(count_kept(&p, 100, &gap) == 100);
    t23_ivdc_pace_build(&p, 25, 30);
    assert(count_kept(&p, 100, &gap) == 100);
    /* input rate from 33.333 ms frame starts with a skipped event in between */
    p = (T23IvdcPace){0};
    for (i = 0; i < 60; i++) {
        if (i == 20)
            continue;               /* a gap of two periods is ignored */
        fps = t23_ivdc_pace_input(&p, 1000000ull + (uint64_t)i * 33333u, 25);
    }
    assert(fps == 30 && p.len == 30);
    assert(count_kept(&p, 600, &gap) == 500);
    puts("ivdc_pace_test: ok");
    return 0;
}
