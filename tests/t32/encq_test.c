/*
 * T32/T33 encoder stream queue (#691): host model of the channel's stream
 * slots against the vendor libimp behaviour (src/t32/openimp_t32_encq.h).
 *
 * - full stream buffer: the new frame is dropped, the queued ones stay
 *   (vendor get_empty_framestream_noblock);
 * - StopRecvPic drains the queued streams (vendor PollingStream / GetStream /
 *   ReleaseStream loop), a stream the application holds stays with it;
 * - so the first GetStream after StopRecvPic + StartRecvPic returns a frame
 *   coded after the start (the #688 stale snapshot: before the fix it was the
 *   frame right after the last fetch, seconds old).
 */
#include <stdio.h>
#include <stdlib.h>

#include "t32/openimp_t32_encq.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

struct chn {
    struct t32_encq q;
    int frame_of[T32_ENCQ_MAX];         /* the source frame each slot holds */
    int recv, dropped;
};

/* the sink: one source frame, coded at once when a slot is free */
static void sink(struct chn *c, int frame)
{
    int si;

    if (!c->recv)
        return;
    si = t32_encq_free(&c->q);
    if (si < 0) {
        c->dropped++;                   /* vendor: "the bitstream buf is full" */
        return;
    }
    c->frame_of[si] = frame;
    t32_encq_ready(&c->q, si);
}

/* GetStream + ReleaseStream: the frame number, -1 when nothing is queued */
static int fetch(struct chn *c)
{
    int si = t32_encq_take(&c->q), f;

    if (si < 0)
        return -1;
    f = c->frame_of[si];
    CHECK(t32_encq_release(&c->q, si) == 0, "release of slot %d", si);
    return f;
}

static void stop(struct chn *c)
{
    c->recv = 0;
    t32_encq_stop(&c->q);
}

int main(void)
{
    struct chn c = { .recv = 1 };
    int f, si, held;

    t32_encq_init(&c.q, 3);

    /* streaming: every frame fetched in time */
    for (f = 0; f < 10; f++) {
        sink(&c, f);
        CHECK(fetch(&c) == f, "live frame %d", f);
    }

    /* the application stops fetching (idle linger): 3 slots fill with 10..12,
     * 13..19 are dropped, the queued oldest stay (vendor drop-new) */
    for (f = 10; f < 20; f++)
        sink(&c, f);
    CHECK(c.q.rcount == 3 && c.dropped == 7, "full buffer: %d queued, %d dropped", c.q.rcount, c.dropped);
    CHECK(c.frame_of[t32_encq_peek(&c.q, 0)] == 10, "oldest queued frame %d", c.frame_of[t32_encq_peek(&c.q, 0)]);

    /* StopRecvPic, pause, StartRecvPic: the first stream is the current frame */
    stop(&c);
    CHECK(c.q.rcount == 0 && !t32_encq_busy(&c.q), "StopRecvPic left %d queued streams", c.q.rcount);
    for (f = 20; f < 30; f++)
        sink(&c, f);                    /* not receiving: no stream */
    c.recv = 1;
    sink(&c, 30);
    f = fetch(&c);
    CHECK(f == 30, "first stream after Stop/StartRecvPic is frame %d, want 30 (stale stream, #691)", f);

    /* a stream the application holds survives the stop, Release still works */
    sink(&c, 31);
    sink(&c, 32);
    held = t32_encq_take(&c.q);
    CHECK(held >= 0 && c.frame_of[held] == 31, "held frame");
    stop(&c);
    CHECK(c.q.rcount == 0 && t32_encq_busy(&c.q), "held stream after stop");
    CHECK(t32_encq_release(&c.q, held) == 0 && !t32_encq_busy(&c.q), "release after stop");
    CHECK(t32_encq_release(&c.q, held) != 0, "double release");

    /* FlushStream: queued ones dropped, order of later streams kept */
    c.recv = 1;
    sink(&c, 40);
    sink(&c, 41);
    CHECK(t32_encq_drop_ready(&c.q) == 2, "flush count");
    sink(&c, 42);
    sink(&c, 43);
    CHECK(fetch(&c) == 42 && fetch(&c) == 43 && fetch(&c) == -1, "order after flush");

    /* FIFO wrap over many cycles */
    for (f = 100; f < 140; f++) {
        sink(&c, f);
        if (f & 1) {
            CHECK(fetch(&c) == f - 1, "wrap %d", f);
            CHECK(fetch(&c) == f, "wrap %d", f);
        }
    }
    si = t32_encq_free(&c.q);
    CHECK(si >= 0 && c.q.rcount == 0, "empty at the end");

    if (fails) {
        printf("t32 encoder stream queue: %d failures\n", fails);
        return 1;
    }
    printf("t32 encoder stream queue (#691: full buffer drops new, StopRecvPic drains): ok\n");
    return 0;
}
