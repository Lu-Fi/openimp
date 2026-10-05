/*
 * enc_rest_preload - LD_PRELOAD shim for the T23/T31 encoder items of
 * claude/t23-enc-rest, for a streamer that does not call them itself
 * (timps, prudynt).  Nothing vendor is linked; build it for the camera's
 * SoC from the repository's include/ directory:
 *
 *   T23:  $CC -std=gnu99 -O2 -Wall -fPIC -shared -DPLATFORM_T23 -I../../include \
 *             -o enc_rest_preload.so enc_rest_preload.c -ldl
 *   T31:  the same with -DPLATFORM_T31
 *
 * Environment (all optional):
 *   ENCREST_POOL_KB=<n>      before CreateChn: IMP_System_MemPoolRequest(
 *                            ENCREST_POOL_ID (default 7), n KiB) and
 *                            IMP_Encoder_SetPool(chn, id).  The encoder's own
 *                            buffers must fit: T23 640x360 about 2 MB, 1080p
 *                            about 6 MB; T31 1080p 25-35 MB.  Too small: the
 *                            encoder logs "memory pool N has no room" and does
 *                            not start.  The shim prints the pool's rmem use
 *                            from the OpenIMP log.
 *   ENCREST_MAXPIC_KBIT=<n>  after CreateChn: IMP_Encoder_SetChnMaxPictureSize
 *                            (chn, n, n) = n * 128 bytes (T23 native Helix
 *                            drops a picture of that size or more and re-codes
 *                            an IDR at a higher QP).
 *   ENCREST_EVAL=1           T31: after each GetStream call
 *                            IMP_Encoder_GetChnEvalInfo and log the record.
 *   ENCREST_EVERY=<n>        log every n-th picture (default 50) plus every
 *                            picture over the limit
 *   ENCREST_LOG=<file>       log file (default stderr)
 *
 * The shim logs, per channel and window of ENCREST_EVERY pictures: pictures,
 * IDRs, largest IDR and P picture in bytes, and the number of GetStream
 * calls that returned nothing (a dropped picture appears as a missing
 * picture: timps polls again).
 *
 * Run on the camera (stop the streamer first):
 *   /etc/init.d/S95timps stop
 *   ENCREST_MAXPIC_KBIT=300 LD_PRELOAD=/tmp/enc_rest_preload.so \
 *       /usr/bin/timpsd -c /etc/timps.conf
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>

#define CHANNELS 9

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf_;
static int every = 50;

static struct {
    uint32_t pictures, idrs, empty, over;
    uint32_t max_idr, max_p;
} win[CHANNELS];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lock);
    if (!logf_) {
        const char *path = getenv("ENCREST_LOG");

        logf_ = path ? fopen(path, "a") : NULL;
        if (!logf_)
            logf_ = stderr;
        setvbuf(logf_, NULL, _IOLBF, 0);
    }
    fprintf(logf_, "[encrest] %s\n", line);
    pthread_mutex_unlock(&lock);
}

static long envl(const char *name, long def)
{
    const char *v = getenv(name);

    return v && *v ? strtol(v, NULL, 0) : def;
}

int IMP_Encoder_CreateChn(int chn, IMPEncoderCHNAttr *attr)
{
    static int (*real)(int, IMPEncoderCHNAttr *);
    long pool_kb = envl("ENCREST_POOL_KB", 0);
    long maxpic = envl("ENCREST_MAXPIC_KBIT", 0);
    int ret;

    if (!real)
        real = dlsym(RTLD_NEXT, "IMP_Encoder_CreateChn");
    if (!real)
        return -1;
    if (every == 50)
        every = (int)envl("ENCREST_EVERY", 50);
    if (pool_kb > 0) {
        int id = (int)envl("ENCREST_POOL_ID", 7);
        static int requested = -1;

        if (requested != id) {
            int r = IMP_System_MemPoolRequest(id, (size_t)pool_kb << 10,
                                              "encrest");
            say("MemPoolRequest(%d, %ld KiB) = %d", id, pool_kb, r);
            if (r == 0)
                requested = id;
        }
        say("IMP_Encoder_SetPool(%d, %d) = %d (GetPool %d)", chn, id,
            IMP_Encoder_SetPool(chn, id), IMP_Encoder_GetPool(chn));
    }
    ret = real(chn, attr);
    say("CreateChn(%d) = %d", chn, ret);
    if (ret == 0 && maxpic > 0) {
        int r = IMP_Encoder_SetChnMaxPictureSize(chn, (uint32_t)maxpic,
                                                 (uint32_t)maxpic);
#if defined(PLATFORM_T23)
        uint32_t i = 0, p = 0;

        (void)IMP_Encoder_GetChnMaxPictureSize(chn, &i, &p);
        say("SetChnMaxPictureSize(%d, %ld kbit) = %d, get %u/%u kbit "
            "(limit %ld bytes)", chn, maxpic, r, i, p, maxpic * 128);
#else
        say("SetChnMaxPictureSize(%d, %ld kbit) = %d", chn, maxpic, r);
#endif
    }
    return ret;
}

static int is_idr(const IMPEncoderStream *s)
{
    uint32_t k;

    for (k = 0; k < s->packCount; k++) {
#if defined(PLATFORM_T31)
        const uint8_t *d = (const uint8_t *)(uintptr_t)s->virAddr +
                           s->pack[k].offset;
#else
        const uint8_t *d = (const uint8_t *)(uintptr_t)s->pack[k].virAddr;
#endif
        uint32_t n = s->pack[k].length, j;

        if (!d)
            continue;
        for (j = 0; j + 4 < n && j < 64; j++)
            if (d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1 &&
                (d[j + 3] & 0x1f) == 5)
                return 1;
    }
    return 0;
}

int IMP_Encoder_GetStream(int chn, IMPEncoderStream *stream, int block)
{
    static int (*real)(int, IMPEncoderStream *, int);
    long maxpic = envl("ENCREST_MAXPIC_KBIT", 0);
    int ret;

    if (!real)
        real = dlsym(RTLD_NEXT, "IMP_Encoder_GetStream");
    if (!real)
        return -1;
    ret = real(chn, stream, block);
    if (chn < 0 || chn >= CHANNELS)
        return ret;
    pthread_mutex_lock(&lock);
    if (ret != 0) {
        win[chn].empty++;
    } else {
        uint32_t bytes = 0, k;
        int idr = is_idr(stream);

        for (k = 0; k < stream->packCount; k++)
            bytes += stream->pack[k].length;
        win[chn].pictures++;
        if (idr) {
            win[chn].idrs++;
            if (bytes > win[chn].max_idr)
                win[chn].max_idr = bytes;
        } else if (bytes > win[chn].max_p) {
            win[chn].max_p = bytes;
        }
        if (maxpic > 0 && bytes >= (uint32_t)(maxpic * 128))
            win[chn].over++;
        if (win[chn].pictures >= (uint32_t)every) {
            pthread_mutex_unlock(&lock);
            say("ch%d window: %u pictures, %u IDR (largest %u bytes), "
                "largest P %u bytes, %u over the limit, %u empty polls",
                chn, win[chn].pictures, win[chn].idrs, win[chn].max_idr,
                win[chn].max_p, win[chn].over, win[chn].empty);
            pthread_mutex_lock(&lock);
            memset(&win[chn], 0, sizeof(win[chn]));
        }
    }
    pthread_mutex_unlock(&lock);
#if defined(PLATFORM_T31)
    if (ret == 0 && envl("ENCREST_EVAL", 0)) {
        static unsigned int n[CHANNELS];
        uint8_t rec[36];

        if (n[chn]++ % (unsigned int)every == 0) {
            memset(rec, 0xee, sizeof(rec));
            if (IMP_Encoder_GetChnEvalInfo(chn, rec) == 0) {
                uint32_t v[6];
                uint16_t h[3];

                memcpy(v, rec, 24);
                memcpy(h, rec + 24, 6);
                say("ch%d eval: intra %u skip %u cu8 %u cu16 %u cu32 %u "
                    "cu64 %u | slice qp %u min qp %u max qp %u | pad "
                    "%02x%02x%02x%02x%02x%02x", chn, v[0], v[1], v[2],
                    v[3], v[4], v[5], h[0], h[1], h[2], rec[30], rec[31],
                    rec[32], rec[33], rec[34], rec[35]);
            } else {
                say("ch%d GetChnEvalInfo failed", chn);
            }
        }
    }
#endif
    return ret;
}
