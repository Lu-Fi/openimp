/*
 * IMP_AO_CacheSwitch - the vendor AO play cache, as a pure helper.
 *
 * Vendor libimp (T31 1.1.6 disassembly, IMP_AO_CacheSwitch / _ao_chn_enable /
 * _ao_play_thread): the switch is one int per AO channel, only device 0 /
 * channel 0 are valid (anything else returns -1), the value is stored as given
 * (non-zero = on), _ao_chn_enable resets it to 1, so the vendor default is
 * "cache on". With it on the play thread starts in a "filling" state (flag set
 * when the channel is enabled and again after an underrun), does not hand
 * data to the driver until the queued amount reaches a threshold taken from
 * the channel attributes, and then plays. With it off the thread writes what
 * it has at once and never waits. The vendor threshold is a buffer size from
 * the channel attributes; the exact value was not recovered, OpenIMP uses
 * OPENIMP_AO_CACHE_PERIODS (2) periods.
 *
 * OpenIMP has no play thread: SendFrame writes to the driver in the caller's
 * thread, which equals "cache off" and is still the default here (stability;
 * set OPENIMP_AO_CACHE=1 in the environment to make "on" the default like the
 * vendor). With the cache on, data is held back until the threshold is
 * reached, then written in one go. An underrun is detected from the wall
 * clock: the driver is assumed to play what was written back to back, so a
 * write arriving after that queue ran dry starts filling again. Held data is
 * written by Flush (the vendor flush plays everything), dropped by Clear,
 * and written when the cache is switched off or the channel is disabled.
 */
#ifndef OPENIMP_AO_CACHE_H
#define OPENIMP_AO_CACHE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OPENIMP_AO_CACHE_PERIODS 2
#define OPENIMP_AO_CACHE_MAX_HELD (1024u * 1024u)

typedef int (*openimp_ao_write_fn)(void *ctx, const void *data, size_t len);

typedef struct {
    int enabled;            /* vendor offset 200 */
    int filling;            /* vendor offset 196 */
    uint8_t *held;
    size_t held_len;
    size_t held_cap;
    size_t threshold;       /* bytes to collect before playing */
    int64_t busy_until_ns;  /* when the driver queue runs dry */
} openimp_ao_cache;

static inline int openimp_ao_cache_env_default(void)
{
    const char *value = getenv("OPENIMP_AO_CACHE");

    return value && value[0] == '1';
}

/* channel enable: vendor sets the switch to its default and starts filling */
static inline void openimp_ao_cache_reset(openimp_ao_cache *c, size_t threshold)
{
    c->enabled = openimp_ao_cache_env_default();
    c->filling = 1;
    c->held_len = 0;
    c->threshold = threshold;
    c->busy_until_ns = 0;
}

static inline void openimp_ao_cache_free(openimp_ao_cache *c)
{
    free(c->held);
    c->held = NULL;
    c->held_len = c->held_cap = 0;
}

static inline int64_t openimp_ao_cache_duration_ns(size_t len,
                                                   unsigned bytes_per_sec)
{
    return bytes_per_sec ? (int64_t)((uint64_t)len * 1000000000ull /
                                     bytes_per_sec)
                         : 0;
}

static inline int openimp_ao_cache_play(openimp_ao_cache *c, const void *data,
                                        size_t len, int64_t now_ns,
                                        unsigned bytes_per_sec,
                                        openimp_ao_write_fn write, void *ctx)
{
    int result = write(ctx, data, len);

    if (c->busy_until_ns < now_ns)
        c->busy_until_ns = now_ns;
    c->busy_until_ns += openimp_ao_cache_duration_ns(len, bytes_per_sec);
    return result;
}

/* write out whatever is held (flush, switch off, disable); the queue is
 * assumed to drain afterwards, so the next data fills again */
static inline int openimp_ao_cache_release(openimp_ao_cache *c, int64_t now_ns,
                                           unsigned bytes_per_sec,
                                           openimp_ao_write_fn write, void *ctx)
{
    int result = 0;

    if (c->held_len) {
        size_t len = c->held_len;

        c->held_len = 0;
        result = openimp_ao_cache_play(c, c->held, len, now_ns, bytes_per_sec,
                                       write, ctx);
    }
    c->filling = 1;
    return result;
}

static inline void openimp_ao_cache_drop(openimp_ao_cache *c)
{
    c->held_len = 0;
    c->filling = 1;
    c->busy_until_ns = 0;
}

static inline int openimp_ao_cache_set(openimp_ao_cache *c, int enable,
                                       int64_t now_ns, unsigned bytes_per_sec,
                                       openimp_ao_write_fn write, void *ctx)
{
    int result = 0;

    if (!enable && c->enabled)
        result = openimp_ao_cache_release(c, now_ns, bytes_per_sec, write, ctx);
    c->enabled = enable != 0;
    c->filling = 1;
    return result;
}

/* one chunk from SendFrame (T31: a whole period) */
static inline int openimp_ao_cache_submit(openimp_ao_cache *c, const void *data,
                                          size_t len, int64_t now_ns,
                                          unsigned bytes_per_sec,
                                          openimp_ao_write_fn write, void *ctx)
{
    if (!c->enabled)
        return write(ctx, data, len);
    if (!c->filling && now_ns > c->busy_until_ns)
        c->filling = 1;                         /* underrun */
    if (!c->filling)
        return openimp_ao_cache_play(c, data, len, now_ns, bytes_per_sec,
                                     write, ctx);
    if (c->held_len + len > c->held_cap) {
        size_t cap = c->held_len + len;
        uint8_t *grown;

        if (cap > OPENIMP_AO_CACHE_MAX_HELD ||
            !(grown = realloc(c->held, cap))) {
            /* cannot hold more: play what we have, then this chunk */
            int result = openimp_ao_cache_release(c, now_ns, bytes_per_sec,
                                                  write, ctx);

            c->filling = 0;
            return result ? result
                          : openimp_ao_cache_play(c, data, len, now_ns,
                                                  bytes_per_sec, write, ctx);
        }
        c->held = grown;
        c->held_cap = cap;
    }
    memcpy(c->held + c->held_len, data, len);
    c->held_len += len;
    if (c->held_len < c->threshold)
        return 0;
    {
        int result = openimp_ao_cache_release(c, now_ns, bytes_per_sec,
                                              write, ctx);

        c->filling = 0;
        return result;
    }
}

#endif
