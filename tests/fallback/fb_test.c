/* Fallback frame: timing/state machine and message picture on the host
 * (src/openimp_fb_logic.h).  Simulated clock, no sleeping. */
#include <stdio.h>
#include <stdlib.h>
#include "openimp_fb_logic.h"
#include <openimp_fallback.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS(x) ((uint64_t)(x) * 1000u)

/* A consumer that polls every `poll_ms` while no real frame comes; returns the
 * number of synthetic frames and (via t) the end time. */
static unsigned run_dead(OpenIMPFb *f, uint64_t *t, uint64_t until, uint64_t interval, unsigned poll_ms,
                         uint64_t *first_gen)
{
    unsigned gen = 0;

    while (*t < until) {
        uint64_t wait;
        int entered, d;

        openimp_fb_poll(f, *t);
        d = openimp_fb_quiet(f, *t, interval, &wait, &entered);
        if (d == OPENIMP_FB_GEN) {
            if (!gen && first_gen)
                *first_gen = *t;
            gen++;
        }
        *t += MS(poll_ms);
    }
    return gen;
}

int main(void)
{
    OpenIMPFb f;
    uint64_t t = MS(1000), wait, first = 0;
    int entered, d;
    unsigned gen;

    /* disabled: never */
    openimp_fb_init(&f, 0);
    openimp_fb_arm(&f, t);
    gen = run_dead(&f, &t, t + MS(10000), 40000, 5, NULL);
    CHECK(gen == 0 && !f.active, "disabled channel generated %u frames", gen);

    /* enabled but not armed (channel not streaming): never */
    openimp_fb_init(&f, 1);
    gen = run_dead(&f, &t, t + MS(5000), 40000, 5, NULL);
    CHECK(gen == 0 && !f.active, "unarmed channel generated %u frames", gen);

    /* enabled: nothing before the dwell time, then 25 fps */
    openimp_fb_init(&f, 1);
    openimp_fb_arm(&f, t);
    {
        uint64_t t0 = t;

        gen = run_dead(&f, &t, t0 + MS(1990), 40000, 5, NULL);
        CHECK(gen == 0 && !f.active, "fallback before the dwell time (%u frames)", gen);
        gen = run_dead(&f, &t, t0 + MS(2000) + MS(1000), 40000, 5, &first);
        CHECK(f.active && f.activations == 1, "not active after the dwell time");
        CHECK(first >= t0 + MS(2000) && first <= t0 + MS(2010), "first frame at %llu ms after arm",
              (unsigned long long)((first - t0) / 1000));
        CHECK(gen >= 24 && gen <= 26, "%u frames in the first second at 25 fps", gen);
    }

    /* coarse consumer (poll every 100 ms): the rate holds, no burst */
    {
        unsigned g2 = run_dead(&f, &t, t + MS(2000), 40000, 100, NULL);

        CHECK(g2 >= 18 && g2 <= 21, "%u frames in 2 s with a 100 ms poll (one per poll expected)", g2);
    }

    /* the first real frame ends it */
    openimp_fb_poll(&f, t);
    CHECK(openimp_fb_real(&f, t) == 1 && !f.active, "real frame did not end the fallback");
    CHECK(openimp_fb_real(&f, t + MS(40)) == 0, "second real frame reported a switch");
    d = openimp_fb_quiet(&f, t + MS(1000), 40000, &wait, &entered);
    CHECK(d == OPENIMP_FB_NO, "fallback again 1 s after a real frame");
    {
        uint64_t t0 = t + MS(40);

        t += MS(40);
        gen = run_dead(&f, &t, t0 + MS(3000), 40000, 5, &first);
        CHECK(gen > 0 && f.activations == 2 && first >= t0 + MS(2000), "second outage: %u frames, %u activations",
              gen, f.activations);
    }

    /* switched off while active */
    f.enabled = 0;
    d = openimp_fb_quiet(&f, t, 40000, &wait, &entered);
    CHECK(d == OPENIMP_FB_NO && !f.active, "still active after switching off");

    /* a consumer that was away does not trigger it (polls resume after a pause) */
    openimp_fb_init(&f, 1);
    openimp_fb_arm(&f, t);
    t += MS(30000);                           /* nobody asked for frames for 30 s */
    openimp_fb_poll(&f, t);
    d = openimp_fb_quiet(&f, t, 40000, &wait, &entered);
    CHECK(d == OPENIMP_FB_NO && !f.active, "fallback right after the consumer came back");
    gen = run_dead(&f, &t, t + MS(2500), 40000, 20, &first);
    CHECK(gen > 0, "no fallback although the consumer waited 2.5 s");

    /* a blocking GetFrame (1.05 s per call, as the T41 one) in a dead-sensor loop: start-to-start
     * gaps of about the dwell time/2 must not keep resetting the clock */
    {
        uint64_t t0;
        unsigned calls = 0;
        int got = 0;

        openimp_fb_init(&f, 1);
        openimp_fb_arm(&f, t);
        t0 = t;
        while (t < t0 + MS(6000) && !got) {
            openimp_fb_poll(&f, t);
            t += MS(1050);                      /* the call waits for a frame */
            d = openimp_fb_quiet(&f, t, 40000, &wait, &entered);
            calls++;
            if (d == OPENIMP_FB_GEN)
                got = 1;
        }
        CHECK(got && calls <= 3, "blocking-call loop: fallback after %u calls (got %d)", calls, got);
    }

    /* timeout setting */
    CHECK(openimp_fb_clamp_timeout(5) == 2000u && openimp_fb_clamp_timeout(500) == 500u &&
          openimp_fb_clamp_timeout(900000) == 2000u, "timeout clamp");
    openimp_fb_init(&f, 1);
    f.timeout_ms = 500;
    openimp_fb_arm(&f, t);
    gen = run_dead(&f, &t, t + MS(1000), 40000, 5, &first);
    CHECK(gen > 0 && first < t - MS(400), "500 ms dwell time not honoured");

    /* a long gap between polls in fallback: next frame at once, no catch-up burst */
    {
        unsigned burst = 0;
        int i;

        t += MS(1000);
        for (i = 0; i < 10; i++) {
            d = openimp_fb_quiet(&f, t, 40000, &wait, &entered);
            if (d == OPENIMP_FB_GEN)
                burst++;
        }
        CHECK(burst == 1, "%u frames due at one instant after a gap", burst);
    }

    /* the message picture */
    {
        enum { W = 640, H = 360, P = 640, L = 368 };
        static uint8_t buf[P * L * 3 / 2];
        uint32_t s, by, bh, tx, r, c, white = 0, outside = 0;

        memset(buf, 0, sizeof(buf));
        openimp_fb_render_message(buf, W, H, P, L);
        openimp_fb_band(W, H, &s, &by, &bh, &tx);
        CHECK(s == 5 && bh % 2 == 0 && by % 2 == 0 && tx % 2 == 0 && by + bh < H / 3, "band geometry s=%u by=%u bh=%u",
              s, by, bh);
        for (r = by; r < by + bh; r++)
            for (c = 0; c < W; c++)
                if (buf[(size_t)r * P + c] == 235)
                    white++;
        CHECK(white > 1000, "message text pixels: %u", white);
        CHECK(buf[(size_t)(by + 1) * P + 1] == 16, "band background not black");
        /* the pattern outside the band is the test pattern */
        for (r = by + bh; r < H; r++)
            for (c = 0; c < W; c++)
                if (buf[(size_t)r * P + c] == 0)
                    outside++;
        CHECK(outside < 300, "%u black-zero pixels below the band (pattern missing)", outside);
        CHECK(buf[(size_t)(P * L) + (size_t)(by / 2 + 1) * P + 2] == 128, "band chroma not neutral");

        /* tick: counter cells and blinking square, no residue between frames */
        openimp_fb_render_tick(buf, W, H, P, L, 0xA5C3u);
        {
            int b, ok = 1;

            for (b = 0; b < 16; b++) {
                uint8_t want = ((0xA5C3u >> (15 - b)) & 1u) ? 235 : 16;

                if (buf[8 * P + b * 16 + 8] != want)
                    ok = 0;
            }
            CHECK(ok, "counter cells do not encode 0xA5C3");
        }
        CHECK(buf[5 * P + W - 48 + 5] == 16 || buf[5 * P + W - 48 + 5] == 235, "blink square");
        openimp_fb_render_tick(buf, W, H, P, L, 0);
        CHECK(buf[8 * P + 8] == 16 && buf[8 * P + 15 * 16 + 8] == 16, "counter residue after 0xA5C3 -> 0");
        {
            uint8_t on, off;

            openimp_fb_render_tick(buf, W, H, P, L, 4);
            on = buf[5 * P + W - 48 + 5];
            openimp_fb_render_tick(buf, W, H, P, L, 0);
            off = buf[5 * P + W - 48 + 5];
            CHECK(on == 235 && off == 16, "square does not blink (%u/%u)", on, off);
        }
        /* the message band is untouched by the tick */
        CHECK(buf[(size_t)(by + 1) * P + 1] == 16, "tick overwrote the band");
    }

    if (failures) {
        printf("fallback: %d check(s) failed\n", failures);
        return 1;
    }
    printf("fallback tests passed\n");
    return 0;
}
