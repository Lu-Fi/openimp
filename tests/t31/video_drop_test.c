/*
 * video_drop_test - IMP_ISP_Tuning_SetVideoDrop monitor (src/core/
 * video_drop.c, all SoCs).
 *
 * - the step rule: callback after two stale ticks, at most three times
 *   per stall, again after frames came back, never without demand;
 * - the thread (built with a 50 ms tick): starts with the first callback,
 *   reports a stalled stream, stays quiet while frames arrive or nothing
 *   is streaming, and exits when the callback is cleared.
 */

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "video_drop.h"

static int failures;

#define CHECK(cond, ...) do {                                         \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* environment of video_drop.c */
int IMP_Log_Get_Option(void) { return 0; }
int imp_log_fun(int level, int option, int type, ...)
{
    (void)level; (void)option; (void)type;
    return 0;
}
static volatile int demand;
int openimp_fs_video_demand(void) { return demand; }

static volatile int cb_calls;
static void drop_cb(void) { __atomic_add_fetch(&cb_calls, 1, __ATOMIC_RELAXED); }

static void sleep_ms(unsigned int ms)
{
    struct timespec t = { ms / 1000u, (long)(ms % 1000u) * 1000000L };

    while (nanosleep(&t, &t) != 0)
        ;
}

static void test_step(void)
{
    OpenIMPVideoDropState st = { 0, 0, 0 };
    int calls = 0, tick;

    /* streaming: frames move, nothing reported */
    for (tick = 1; tick <= 5; tick++)
        calls += openimp_video_drop_step(&st, (uint32_t)tick * 25u, 1);
    CHECK(calls == 0, "report while frames arrive (%d)", calls);
    /* stall: reports at stale ticks 2, 4 and 6, then quiet */
    for (tick = 1; tick <= 12; tick++) {
        int due = openimp_video_drop_step(&st, 125u, 1);

        CHECK(due == (tick == 2 || tick == 4 || tick == 6),
              "stale tick %d: due %d", tick, due);
    }
    /* frames come back, stall again: reported again */
    CHECK(!openimp_video_drop_step(&st, 130u, 1), "report on recovery");
    CHECK(!openimp_video_drop_step(&st, 130u, 1) &&
          openimp_video_drop_step(&st, 130u, 1), "second stall not reported");
    /* no demand (stream stopped on purpose): never reported */
    calls = 0;
    for (tick = 0; tick < 10; tick++)
        calls += openimp_video_drop_step(&st, 130u, 0);
    CHECK(calls == 0, "report without demand (%d)", calls);
    /* counter wrap is just another change */
    st.last_frames = 0xffffffffu;
    CHECK(!openimp_video_drop_step(&st, 0u, 1), "wrap");
}

static void test_thread(void)
{
    int i;

    /* streaming: the counter moves every 5 ms, no report */
    demand = 1;
    CHECK(openimp_video_drop_set(drop_cb) == 0, "set");
    for (i = 0; i < 40; i++) {
        openimp_video_drop_note_frame();
        sleep_ms(5);
    }
    CHECK(cb_calls == 0, "report while streaming (%d)", cb_calls);
    /* stall: three reports (100, 200, 300 ms), no more */
    sleep_ms(500);
    CHECK(cb_calls == 3, "%d reports for one stall, want 3", cb_calls);
    /* nothing streaming: quiet */
    demand = 0;
    openimp_video_drop_note_frame();
    cb_calls = 0;
    sleep_ms(300);
    CHECK(cb_calls == 0, "report without demand (%d)", cb_calls);
    /* cleared: the thread ends and no callback runs any more */
    CHECK(openimp_video_drop_set(NULL) == 0, "clear");
    demand = 1;
    sleep_ms(300);
    CHECK(cb_calls == 0, "report after clearing (%d)", cb_calls);
    /* registering again restarts the monitor */
    CHECK(openimp_video_drop_set(drop_cb) == 0, "set again");
    sleep_ms(500);
    CHECK(cb_calls == 3, "%d reports after restart, want 3", cb_calls);
    CHECK(openimp_video_drop_set(NULL) == 0, "clear again");
    sleep_ms(100);
}

int main(void)
{
    test_step();
    test_thread();
    if (failures) {
        fprintf(stderr, "video drop: %d check(s) failed\n", failures);
        return 1;
    }
    printf("video drop monitor tests passed\n");
    return 0;
}
