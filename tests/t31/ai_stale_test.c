/*
 * ai_stale_test - audio/openimp_ai_stale.h, the filter for cleared fragments
 * the OSS2 capture driver hands out a second time (Lu-Fi/openimp#266).
 *
 * A model of the reader side of xb_snd_dsp.c feeds it: 10 ms fragments, the
 * worker every 20 ms, a read completes when all its fragments are there.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio/openimp_ai_stale.h"

#define FRAGMENT 320U
#define FRAGMENT_US 10000

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

/* the stream the driver hands out: fragment n is signal (its number in
 * every sample) or cleared */
typedef struct {
    openimp_ai_stale state;
    int64_t now;
    unsigned int next_signal;   /* number of the next recorded fragment */
    unsigned int expect;        /* next signal fragment the output must show */
    unsigned int out_signal;    /* signal fragments that came out */
    unsigned int out_zero;      /* zero fragments that came out */
    int order_ok;
    int64_t max_lag;
} Model;

static void model_init(Model *model)
{
    memset(model, 0, sizeof(*model));
    model->now = 1000000;
    model->next_signal = 1;
    model->expect = 1;
    model->order_ok = 1;
    openimp_ai_stale_reset(&model->state, model->now);
}

/* one read of `signal_head` recorded fragments, then `zero` cleared ones,
 * then `signal_tail` recorded ones, completed `taken_us` after the last */
static size_t model_read(Model *model, unsigned int signal_head,
                         unsigned int zero, unsigned int signal_tail,
                         int64_t taken_us, int drop)
{
    unsigned char data[64 * FRAGMENT];
    unsigned char ref[64 * FRAGMENT];
    unsigned int count = signal_head + zero + signal_tail;
    unsigned int lead = 0;
    unsigned int i;
    size_t kept;

    for (i = 0; i < count; i++) {
        uint16_t value = 0;
        unsigned int k;

        if (i < signal_head || i >= signal_head + zero)
            value = (uint16_t)model->next_signal++;
        for (k = 0; k < FRAGMENT / 2; k++) {
            memcpy(data + i * FRAGMENT + k * 2, &value, 2);
            memcpy(ref + i * FRAGMENT + k * 2, &value, 2);
        }
    }
    model->now += taken_us;
    kept = openimp_ai_stale_filter(&model->state, model->now, data, ref,
                                   count * FRAGMENT, FRAGMENT, FRAGMENT_US,
                                   drop, &lead);
    CHECK(kept % FRAGMENT == 0, "kept %zu is no whole fragment", kept);
    CHECK(!memcmp(data, ref, kept), "reference not moved with the data");
    model->out_zero += lead;
    for (i = 0; i < kept / FRAGMENT; i++) {
        uint16_t value;

        memcpy(&value, data + i * FRAGMENT, 2);
        if (!value) {
            model->out_zero++;
        } else {
            if (value != model->expect)
                model->order_ok = 0;
            model->expect = value + 1U;
            model->out_signal++;
        }
    }
    if (model->state.lag_us > model->max_lag)
        model->max_lag = model->state.lag_us;
    return kept + (size_t)lead * FRAGMENT;
}

/* steady capture: 4 fragments every 40 ms, with the driver's jitter */
static void model_steady(Model *model, unsigned int reads, int drop)
{
    static const int jitter[] = { 0, 3000, -3000, 20000, -20000, 0, 5000,
                                  -5000 };
    unsigned int i;

    for (i = 0; i < reads; i++)
        model_read(model, 4, 0, 0, 40000 + jitter[i % 8], drop);
}

/* the false overrun: the pending read completes at once with cleared
 * fragments, then 95 of them as fast as they are asked for; the audio
 * recorded meanwhile follows, 2 fragments per 20 ms worker run more */
static unsigned int model_overrun(Model *model, unsigned int head_signal,
                                  int drop)
{
    unsigned int left = OPENIMP_AI_STALE_FRAGMENTS;
    unsigned int reads = 0;
    unsigned int backlog;       /* recorded during the drain, in fragments */
    int64_t start = model->now;
    unsigned int n = 4 - head_signal;

    model_read(model, head_signal, n, 0, head_signal ? 23000 : 4000, drop);
    left -= n;
    reads++;
    while (left) {
        size_t want = openimp_ai_stale_next(&model->state, 4, 12);
        unsigned int zero = left < want ? left : (unsigned int)want;
        unsigned int tail = (unsigned int)want - zero;

        /* the fragments are there: one worker run per read */
        model_read(model, 0, zero, tail, 20000, drop);
        left -= zero;
        reads++;
        CHECK(reads < 60, "drain does not end");
        if (reads >= 60)
            break;
    }
    backlog = (unsigned int)((model->now - start) / FRAGMENT_US);
    if (backlog >= 8) {
        /* the reader thread itself comes late once while it catches up */
        model_read(model, 4, 0, 0, 50000, drop);
        backlog += 1;
        CHECK(!drop || model->state.lag_us >= 50000,
              "lag lost on a late read (%lld us)",
              (long long)model->state.lag_us);
    }
    while (backlog >= 2) {
        /* catching up: 4 ready fragments per run while 2 more are recorded */
        model_read(model, 4, 0, 0, 20000, drop);
        backlog -= 2;
        reads++;
    }
    return reads;
}

static void test_steady(void)
{
    Model model;

    model_init(&model);
    model_steady(&model, 500, 1);
    CHECK(model.out_signal == 2000 && model.out_zero == 0 && model.order_ok,
          "steady capture changed: %u signal, %u zero", model.out_signal,
          model.out_zero);
    CHECK(model.state.events == 0 && model.state.dropped == 0,
          "steady capture counted stale fragments");
    CHECK(model.max_lag == 0, "steady capture has lag %lld",
          (long long)model.max_lag);
}

static void test_overrun(unsigned int head_signal)
{
    Model model;
    unsigned int before;
    int64_t start;

    model_init(&model);
    model_steady(&model, 50, 1);
    before = model.out_signal;
    start = model.now;
    model_overrun(&model, head_signal, 1);
    CHECK(model.out_zero == 0, "head %u: %u cleared fragments got through",
          head_signal, model.out_zero);
    CHECK(model.order_ok && model.out_signal == model.next_signal - 1,
          "head %u: recorded audio lost or reordered (%u of %u)",
          head_signal, model.out_signal, model.next_signal - 1);
    CHECK(model.out_signal > before, "head %u: no audio after the run",
          head_signal);
    /* live again well before the 0.475 s the 4-fragment reads would take */
    CHECK(model.now - start < 500000 && model.max_lag <= 300000,
          "head %u: drain took too long (lag %lld us)", head_signal,
          (long long)model.max_lag);
    CHECK(model.max_lag >= 100000, "head %u: no lag reported (%lld us)",
          head_signal, (long long)model.max_lag);
    model_steady(&model, 50, 1);
    CHECK(model.state.events == 1 &&
          model.state.last_run == OPENIMP_AI_STALE_FRAGMENTS,
          "head %u: %lu events, last run %u", head_signal,
          model.state.events, model.state.last_run);
    CHECK(model.state.lag_us == 0 && !model.state.tracking,
          "head %u: lag %lld left after catching up", head_signal,
          (long long)model.state.lag_us);
    CHECK(model.out_zero == 0 && model.order_ok, "head %u: later damage",
          head_signal);
    CHECK(openimp_ai_stale_next(&model.state, 4, 12) == 4,
          "head %u: large reads after the run", head_signal);
    /* a second one right away is handled the same */
    model_overrun(&model, 0, 1);
    model_steady(&model, 10, 1);
    CHECK(model.out_zero == 0 && model.order_ok && model.state.events == 2,
          "head %u: second run: %u zero, %lu events", head_signal,
          model.out_zero, model.state.events);
}

/* OPENIMP_AI_STALE=0: counted, nothing changed */
static void test_observe(void)
{
    Model model;

    model_init(&model);
    model_steady(&model, 50, 0);
    model_overrun(&model, 0, 0);
    model_steady(&model, 10, 0);
    CHECK(model.out_zero == OPENIMP_AI_STALE_FRAGMENTS && model.order_ok,
          "observe: %u cleared fragments passed on", model.out_zero);
    CHECK(model.state.events == 1, "observe: %lu events",
          model.state.events);
    CHECK(model.max_lag == 0, "observe: lag reported");
}

/* a source that really delivers zeros keeps every fragment, in real time */
static void test_silent_source(void)
{
    Model model;
    unsigned int i;
    size_t most = 0;

    model_init(&model);
    model_steady(&model, 20, 1);
    /* silence begins inside a read, then lasts 20 s, with jitter */
    model_read(&model, 2, 2, 0, 40000, 1);
    for (i = 0; i < 500; i++) {
        static const int jitter[] = { 0, 20000, -20000, 4000, -4000 };
        size_t got = model_read(&model, 0, 4, 0, 40000 + jitter[i % 5], 1);

        if (got > most)
            most = got;
        CHECK(openimp_ai_stale_next(&model.state, 4, 12) == 4,
              "silent source: large read asked for");
    }
    model_read(&model, 0, 2, 2, 40000, 1);
    model_steady(&model, 20, 1);
    CHECK(model.out_zero == 2 + 2000 + 2, "silent source: %u of 2004 zero "
          "fragments came out", model.out_zero);
    CHECK(model.order_ok && model.out_signal == model.next_signal - 1,
          "silent source: signal lost");
    CHECK(model.state.events == 0, "silent source: %lu stale events",
          model.state.events);
    /* held back for a moment only */
    CHECK(most <= (OPENIMP_AI_STALE_HOLD_MAX + 4U) * FRAGMENT,
          "silent source: %zu bytes at once", most);
    CHECK(model.state.lag_us == 0, "silent source: lag left");
}

/* the 5 cleared fragments at stream start stay as they were */
static void test_stream_start(void)
{
    Model model;

    model_init(&model);
    model_read(&model, 0, 4, 0, 20000, 1);
    model_read(&model, 0, 1, 3, 20000, 1);
    model_steady(&model, 20, 1);
    CHECK(model.out_zero == 5 && model.order_ok &&
          model.out_signal == model.next_signal - 1,
          "stream start: %u zero fragments, signal %u of %u",
          model.out_zero, model.out_signal, model.next_signal - 1);
    CHECK(model.state.events == 0, "stream start counted as stale run");
}

/* short digital silence between signal is passed on */
static void test_short_zero(void)
{
    Model model;

    model_init(&model);
    model_steady(&model, 10, 1);
    model_read(&model, 1, 2, 1, 40000, 1);
    model_read(&model, 0, 4, 0, 40000, 1);
    model_read(&model, 0, 1, 3, 40000, 1);
    model_steady(&model, 10, 1);
    CHECK(model.out_zero == 7 && model.order_ok &&
          model.out_signal == model.next_signal - 1,
          "short silence: %u of 7 zero fragments", model.out_zero);
}

/* a reader that fell behind and gets real silence faster than real time:
 * whatever is dropped is silence, the signal stays whole, and the filter
 * lets go when the pace is real time again */
static void test_backlog_silence(void)
{
    Model model;
    unsigned int i;

    model_init(&model);
    model_steady(&model, 10, 1);
    for (i = 0; i < 6; i++)
        model_read(&model, 0, 4, 0, 20000, 1);
    for (i = 0; i < 100; i++)
        model_read(&model, 0, 4, 0, 40000, 1);
    model_read(&model, 0, 0, 4, 40000, 1);
    model_steady(&model, 60, 1);
    CHECK(model.order_ok && model.out_signal == model.next_signal - 1,
          "backlog silence: signal lost");
    CHECK(model.out_zero >= 424 - 40, "backlog silence: only %u of 424 zero "
          "fragments came out", model.out_zero);
    CHECK(model.state.lag_us == 0 && !model.state.tracking,
          "backlog silence: lag %lld left", (long long)model.state.lag_us);
}

int main(void)
{
    test_steady();
    test_overrun(0);
    test_overrun(2);
    test_overrun(3);
    test_observe();
    test_silent_source();
    test_stream_start();
    test_short_zero();
    test_backlog_silence();
    if (failures) {
        fprintf(stderr, "ai_stale_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("ai stale fragment filter tests passed");
    return 0;
}
