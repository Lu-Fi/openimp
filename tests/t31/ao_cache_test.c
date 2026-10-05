/*
 * ao_cache_test - IMP_AO_CacheSwitch against a fake OSS2 /dev/dsp.
 *
 * Same fake driver as ao_sendframe_test plus a fake monotonic clock, so an
 * underrun can be produced without sleeping. Checks the vendor semantics:
 *   - only device 0 / channel 0, any value stored, no-op before AO Enable;
 *   - default (OPENIMP_AO_CACHE unset) is cache off: SendFrame writes at once;
 *   - cache on: filling holds data until 2 periods are there, then all of it
 *     is written in order; afterwards writes go straight through;
 *   - an underrun (driver queue ran dry) starts filling again;
 *   - FlushChnBuf plays held data, ClearChnBuf drops it, switching the cache
 *     off or disabling the channel writes it out;
 *   - EnableChn resets the switch to its default; OPENIMP_AO_CACHE=1 makes
 *     "on" the default.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <imp/imp_audio.h>

#define FAKE_FD  77
#define FRAGMENT 320u
#define PERIOD   1280u          /* 640 samples, 40 ms at 16 kHz mono S16 */
#define T31_AO_SET_STREAM 0x40085069UL

typedef struct {
    void *data;
    uint32_t size;
} FakeOutputStream;

static uint8_t sink[65536];
static size_t sink_len;
static int set_calls;
static int stuck_writes;
static int64_t fake_now_ns = 1000000000LL;

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, void *arg);
int __real_close(int fd);
int __real_clock_gettime(clockid_t id, struct timespec *ts);

int __wrap_open(const char *path, int flags, ...)
{
    return !strcmp(path, "/dev/dsp") ? FAKE_FD : __real_open(path, flags, 0);
}

int __wrap_close(int fd)
{
    return fd == FAKE_FD ? 0 : __real_close(fd);
}

int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC)
        return __real_clock_gettime(id, ts);
    ts->tv_sec = fake_now_ns / 1000000000LL;
    ts->tv_nsec = fake_now_ns % 1000000000LL;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, void *arg)
{
    if (fd != FAKE_FD)
        return __real_ioctl(fd, request, arg);
    if (request == T31_AO_SET_STREAM) {
        FakeOutputStream *stream = arg;

        set_calls++;
        if (!stream->size || stream->size % FRAGMENT) {
            stuck_writes++;
            return -1;
        }
        memcpy(sink + sink_len, stream->data, stream->size);
        sink_len += stream->size;
        return 0;
    }
    return 0;
}

int64_t IMP_System_GetTimeStamp(void)
{
    return 0;
}

static int failures;

#define CHECK(cond, ...) do {                                         \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

static void reset_sink(void)
{
    sink_len = 0;
    set_calls = 0;
}

/* one period whose samples all carry the sequence number tag */
static int send_period(int tag)
{
    static int16_t pcm[PERIOD / 2];
    IMPAudioFrame frame;
    size_t i;

    for (i = 0; i < PERIOD / 2; i++)
        pcm[i] = (int16_t)tag;
    memset(&frame, 0, sizeof(frame));
    frame.virAddr = (uint32_t *)(void *)pcm;
    frame.len = (int)PERIOD;
    return IMP_AO_SendFrame(0, 0, &frame, BLOCK);
}

/* the sink holds tags first, first+1, ... one period each */
static int sink_is_run(int first, int count)
{
    int n;

    if (sink_len != (size_t)count * PERIOD)
        return 0;
    for (n = 0; n < count; n++) {
        int16_t v;

        memcpy(&v, sink + (size_t)n * PERIOD, sizeof(v));
        if (v != first + n)
            return 0;
    }
    return 1;
}

static void open_ao(void)
{
    IMPAudioIOAttr attr;

    memset(&attr, 0, sizeof(attr));
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 640;
    attr.chnCnt = 1;
    CHECK(IMP_AO_SetPubAttr(0, &attr) == 0, "SetPubAttr");
    CHECK(IMP_AO_Enable(0) == 0, "Enable");
    CHECK(IMP_AO_EnableChn(0, 0) == 0, "EnableChn");
}

static void close_ao(void)
{
    CHECK(IMP_AO_DisableChn(0, 0) == 0, "DisableChn");
    CHECK(IMP_AO_Disable(0) == 0, "Disable");
}

int main(void)
{
    unsetenv("OPENIMP_AO_CACHE");

    CHECK(IMP_AO_CacheSwitch(0, 0, 1) == 0, "before Enable is a no-op, 0");
    open_ao();
    CHECK(IMP_AO_CacheSwitch(1, 0, 1) == -1, "device 1");
    CHECK(IMP_AO_CacheSwitch(0, 1, 1) == -1, "channel 1");

    /* default: direct write */
    reset_sink();
    CHECK(send_period(1) == 0 && set_calls == 1 && sink_is_run(1, 1),
          "default is cache off: %d calls", set_calls);

    /* cache on: fill to 2 periods, then play, then straight through */
    CHECK(IMP_AO_CacheSwitch(0, 0, 1) == 0, "cache on");
    reset_sink();
    CHECK(send_period(10) == 0 && set_calls == 0, "1st period held");
    CHECK(send_period(11) == 0 && set_calls == 2 && sink_is_run(10, 2),
          "2nd period releases both in order (%d calls)", set_calls);
    CHECK(send_period(12) == 0 && set_calls == 3 && sink_is_run(10, 3),
          "after the fill the next period goes straight through");

    /* underrun: the queue (3 x 40 ms) ran dry */
    fake_now_ns += 1000000000LL;
    reset_sink();
    CHECK(send_period(20) == 0 && set_calls == 0, "after underrun: held");
    CHECK(send_period(21) == 0 && sink_is_run(20, 2), "refilled, resumes");

    /* no underrun while the queue is still playing (40 ms < 80 ms queued) */
    fake_now_ns += 40000000LL;
    reset_sink();
    CHECK(send_period(22) == 0 && sink_is_run(22, 1), "still playing: direct");

    /* flush plays held data */
    fake_now_ns += 2000000000LL;
    reset_sink();
    CHECK(send_period(30) == 0 && set_calls == 0, "held before flush");
    CHECK(IMP_AO_FlushChnBuf(0, 0) == 0 && sink_is_run(30, 1),
          "FlushChnBuf plays the held period");

    /* clear drops it */
    reset_sink();
    CHECK(send_period(40) == 0 && set_calls == 0, "held before clear");
    CHECK(IMP_AO_ClearChnBuf(0, 0) == 0 && set_calls == 0,
          "ClearChnBuf drops the held period");
    CHECK(IMP_AO_FlushChnBuf(0, 0) == 0 && set_calls == 0, "nothing left");

    /* switching off writes out, then direct */
    reset_sink();
    CHECK(send_period(50) == 0 && set_calls == 0, "held before switch-off");
    CHECK(IMP_AO_CacheSwitch(0, 0, 0) == 0 && sink_is_run(50, 1),
          "cache off plays what was held");
    reset_sink();
    CHECK(send_period(51) == 0 && sink_is_run(51, 1), "direct again");

    /* any non-zero value means on (vendor stores it raw) */
    CHECK(IMP_AO_CacheSwitch(0, 0, 5) == 0, "value 5");
    reset_sink();
    CHECK(send_period(60) == 0 && set_calls == 0, "5 means on");
    /* channel disable plays held data */
    CHECK(IMP_AO_DisableChn(0, 0) == 0 && sink_is_run(60, 1),
          "DisableChn writes out the held period");
    CHECK(IMP_AO_Disable(0) == 0, "Disable");

    /* EnableChn resets the switch to the default (off) */
    open_ao();
    reset_sink();
    CHECK(send_period(70) == 0 && sink_is_run(70, 1),
          "EnableChn resets the cache to the default (off)");
    close_ao();

    /* OPENIMP_AO_CACHE=1: on by default, like the vendor */
    setenv("OPENIMP_AO_CACHE", "1", 1);
    open_ao();
    reset_sink();
    CHECK(send_period(80) == 0 && set_calls == 0, "env default on: held");
    CHECK(send_period(81) == 0 && sink_is_run(80, 2), "env default on: played");
    close_ao();
    unsetenv("OPENIMP_AO_CACHE");

    CHECK(stuck_writes == 0, "%d writes not made of whole fragments",
          stuck_writes);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("T31 AO CacheSwitch: all checks passed\n");
    return 0;
}
