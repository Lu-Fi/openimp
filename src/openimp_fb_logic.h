/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Fallback frame when the ISP frames stop (OpenIMP_FrameSource_SetFallback,
 * include/openimp_fallback.h): the timing/state machine and the message
 * picture.  Header only so that tests/fallback runs it on the host; the
 * FrameSource (src/t40/openimp_p1.c) owns the per-channel state, the buffer
 * and the hook.
 *
 * State machine, one per FrameSource channel, driven by the consumer's
 * GetFrame (so a consumer is bound by definition):
 *
 *   real    a real frame arrives; the last-real time is taken.  When the
 *           channel was in fallback this is the switch back.
 *   quiet   GetFrame found no real frame (it already waited for one).  Once
 *           the last real frame is `timeout_ms` old the channel goes into
 *           fallback; from then on a synthetic frame is due every frame
 *           interval of the channel rate.
 *
 * While in fallback every GetFrame still tries the real queue first
 * (single probe), so the first real frame wins and ends the fallback.
 */
#ifndef OPENIMP_FB_LOGIC_H
#define OPENIMP_FB_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "openimp_testpattern_gen.h"

#define OPENIMP_FB_TIMEOUT_MS_DEFAULT 2000u
#define OPENIMP_FB_TIMEOUT_MS_MIN 200u
#define OPENIMP_FB_TIMEOUT_MS_MAX 60000u

typedef struct {
    uint32_t enabled;           /* API / OPENIMP_FALLBACK_FRAME */
    uint32_t active;            /* synthetic frames are being delivered */
    uint32_t timeout_ms;
    uint64_t last_real_us;      /* 0 = not armed (channel not enabled) */
    uint64_t last_poll_us;      /* the consumer's previous GetFrame */
    uint64_t next_due_us;       /* next synthetic frame */
    uint32_t generated;         /* synthetic frames since the channel was armed */
    uint32_t activations;       /* switches into fallback */
    uint32_t counter;           /* frame counter of the picture, restarts per activation */
} OpenIMPFb;

enum { OPENIMP_FB_NO = 0, OPENIMP_FB_WAIT = 1, OPENIMP_FB_GEN = 2 };

static inline void openimp_fb_init(OpenIMPFb *f, uint32_t enabled)
{
    memset(f, 0, sizeof(*f));
    f->enabled = enabled != 0;
    f->timeout_ms = OPENIMP_FB_TIMEOUT_MS_DEFAULT;
}

static inline uint32_t openimp_fb_clamp_timeout(long ms)
{
    if (ms < (long)OPENIMP_FB_TIMEOUT_MS_MIN || ms > (long)OPENIMP_FB_TIMEOUT_MS_MAX)
        return OPENIMP_FB_TIMEOUT_MS_DEFAULT;
    return (uint32_t)ms;
}

/* The channel starts streaming (EnableChn): the real-frame clock starts. */
static inline void openimp_fb_arm(OpenIMPFb *f, uint64_t now_us)
{
    f->last_real_us = now_us ? now_us : 1u;
    f->last_poll_us = 0;
    f->active = 0;
    f->next_due_us = 0;
    f->generated = 0;
    f->activations = 0;
}

/* The channel stops streaming (DisableChn). */
static inline void openimp_fb_disarm(OpenIMPFb *f)
{
    f->last_real_us = 0;
    f->active = 0;
    f->next_due_us = 0;
}

/* The consumer calls GetFrame (the end of a call that waited counts as a poll too,
 * see openimp_fb_quiet: a blocking GetFrame takes about as long as the dwell time).  A consumer that was away for more than half the
 * dwell time (not yet started, paused, idle channel) restarts the clock: the
 * sensor is judged only while somebody is actually waiting for frames. */
static inline void openimp_fb_poll(OpenIMPFb *f, uint64_t now_us)
{
    uint64_t since = f->last_poll_us ? f->last_poll_us : f->last_real_us;

    if (f->last_real_us && !f->active && now_us > since &&
        now_us - since > (uint64_t)f->timeout_ms * 500u)
        f->last_real_us = now_us;
    f->last_poll_us = now_us ? now_us : 1u;
}

/* A real frame was dequeued.  Returns 1 when this ends a fallback. */
static inline int openimp_fb_real(OpenIMPFb *f, uint64_t now_us)
{
    int back = f->active != 0;

    f->last_real_us = now_us ? now_us : 1u;
    f->last_poll_us = f->last_real_us;
    f->active = 0;
    return back;
}

/*
 * GetFrame found no real frame.  Returns OPENIMP_FB_NO (hand the caller its
 * -1), OPENIMP_FB_WAIT (a synthetic frame is due in *wait_us) or
 * OPENIMP_FB_GEN (generate one now).  *entered is set to 1 on the call that
 * switches into fallback.
 */
static inline int openimp_fb_quiet(OpenIMPFb *f, uint64_t now_us, uint64_t interval_us,
                                   uint64_t *wait_us, int *entered)
{
    *entered = 0;
    *wait_us = 0;
    f->last_poll_us = now_us ? now_us : 1u;     /* the call that waited ends here */
    if (!f->enabled || !f->last_real_us) {
        f->active = 0;
        return OPENIMP_FB_NO;
    }
    if (!f->active) {
        if (now_us < f->last_real_us ||
            now_us - f->last_real_us < (uint64_t)f->timeout_ms * 1000u)
            return OPENIMP_FB_NO;
        f->active = 1;
        f->activations++;
        f->counter = 0;
        f->next_due_us = now_us;
        *entered = 1;
    }
    if (now_us < f->next_due_us) {
        *wait_us = f->next_due_us - now_us;
        return OPENIMP_FB_WAIT;
    }
    f->next_due_us += interval_us;
    if (f->next_due_us < now_us)        /* a long gap: no burst to catch up */
        f->next_due_us = now_us + interval_us;
    f->generated++;
    return OPENIMP_FB_GEN;
}

/* ---- the message picture ---- */

/* 5x7 glyphs for "NO SENSOR SIGNAL" */
static inline const char *openimp_fb_glyph(char c)
{
    switch (c) {
    case 'N': return "10001" "11001" "10101" "10101" "10011" "10001" "10001";
    case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'I': return "01110" "00100" "00100" "00100" "00100" "00100" "01110";
    case 'G': return "01110" "10001" "10000" "10111" "10001" "10001" "01111";
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    default:  return NULL;
    }
}

#define OPENIMP_FB_TEXT "NO SENSOR SIGNAL"

/* Geometry of the text band: scale, top row, height (all even/aligned). */
static inline void openimp_fb_band(uint32_t w, uint32_t h, uint32_t *scale, uint32_t *y0,
                                   uint32_t *bh, uint32_t *x0)
{
    uint32_t s = (w * 8u / 10u) / 95u;     /* 16 glyphs of 5 + 15 gaps of 1 */
    uint32_t th;

    if (s < 1u)
        s = 1u;
    th = 7u * s;
    *scale = s;
    *bh = (th + 4u * s + 1u) & ~1u;
    *y0 = (h / 6u > *bh / 2u ? h / 6u - *bh / 2u : 0u) & ~1u;
    if (*y0 < 16u)
        *y0 = 16u;
    *x0 = ((w > 95u * s ? (w - 95u * s) / 2u : 0u)) & ~1u;
}

/* Static part: pattern, logo, black band with the message.  buf is NV12 with
 * luma pitch*lines rows, the chroma plane after it. */
static inline void openimp_fb_render_message(uint8_t *buf, uint32_t w, uint32_t h,
                                             uint32_t pitch, uint32_t lines)
{
    uint8_t *y = buf, *uv = buf + (size_t)pitch * lines;
    uint32_t s, by, bh, tx, r, c, i, gx, gy;
    const char *t = OPENIMP_FB_TEXT;

    if (w < 64u || h < 64u)
        return;
    openimp_testpattern_nv12_logo(y, uv, w, h, pitch, OPENIMP_TP_LOGO_OPENIMP);
    openimp_fb_band(w, h, &s, &by, &bh, &tx);
    if (by + bh > h)
        return;
    for (r = 0; r < bh; r++)
        memset(y + (size_t)(by + r) * pitch, 16, w);
    for (r = 0; r < bh / 2u; r++)
        for (c = 0; c < w / 2u; c++) {
            uv[(size_t)(by / 2u + r) * pitch + 2u * c] = 128;
            uv[(size_t)(by / 2u + r) * pitch + 2u * c + 1u] = 128;
        }
    for (i = 0; t[i]; i++) {
        const char *g = openimp_fb_glyph(t[i]);

        if (!g)
            continue;
        for (gy = 0; gy < 7u; gy++)
            for (gx = 0; gx < 5u; gx++) {
                uint32_t px, py, dx, dy;

                if (g[gy * 5u + gx] != '1')
                    continue;
                px = tx + (i * 6u + gx) * s;
                py = by + (bh - 7u * s) / 2u + gy * s;
                for (dy = 0; dy < s; dy++)
                    for (dx = 0; dx < s && px + dx < w; dx++)
                        y[(size_t)(py + dy) * pitch + px + dx] = 235;
            }
    }
}

/* Per frame: 16 bit counter cells (top left, 16x16 px, MSB first, as test
 * pattern mode 2) and a blinking 32x32 square (top right); both are
 * overwritten completely, so no residue stays behind. */
static inline void openimp_fb_render_tick(uint8_t *buf, uint32_t w, uint32_t h,
                                          uint32_t pitch, uint32_t lines, uint32_t n)
{
    uint8_t *y = buf, *uv = buf + (size_t)pitch * lines;
    uint32_t r, c, b;

    if (w < 320u || h < 96u)
        return;
    for (r = 0; r < 16u; r++)
        for (b = 0; b < 16u; b++)
            for (c = 0; c < 16u; c++)
                y[(size_t)r * pitch + b * 16u + c] = ((n >> (15u - b)) & 1u) ? 235 : 16;
    for (r = 0; r < 32u; r++)
        memset(y + (size_t)r * pitch + (w - 48u), ((n >> 2) & 1u) ? 235 : 16, 32u);
    for (r = 0; r < 16u; r++)
        for (c = 0; c < 16u; c++) {
            uv[(size_t)r * pitch + (w - 48u) + 2u * c] = 128;
            uv[(size_t)r * pitch + (w - 48u) + 2u * c + 1u] = 128;
        }
    (void)h;
}

#endif
