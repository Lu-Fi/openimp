/* T40 frame wait through the DQBUF worker (#149) with a non-blocking frame
 * channel: a call never outlasts the time it asked for (a stalled sensor
 * must not stretch WaitFrame(20) or GetFrame(1000)), a DisableChn ends a
 * wait at once, and a sensor at 20 fps costs a few waits per frame. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define ioctl test_ioctl
#include "../../src/t40/openimp_p1.c"
#undef ioctl

extern int OpenIMP_T40_FrameSource_WaitFrame(int channel, void **frame, uint32_t wait_ms);

static volatile int dq_calls;
static volatile int sensor_on;          /* 0: DQBUF answers ENODATA always */
static uint64_t next_frame_us;
static uint64_t frame_interval_us = 50000;
static int next_idx;
static unsigned char *pic[4];

volatile uint32_t openimp_video_drop_frames;
int openimp_video_drop_set(void (*cb)(void)) { (void)cb; return 0; }
int OpenIMP_P2_DMAState(uint32_t *b, uint32_t *u) { (void)b; (void)u; return 0; }
int DMA_AllocDescriptor(IMPDMABufferInfo *i, int sz, const char *t) { (void)i; (void)sz; (void)t; return -1; }
int DMA_AllocDescriptorTop(IMPDMABufferInfo *i, int sz, const char *t) { (void)i; (void)sz; (void)t; return -1; }
int DMA_FreePhys(uint32_t p) { (void)p; return 0; }
int DMA_RmemFlushCache(void *v, uint32_t s, int d) { (void)v; (void)s; (void)d; return 0; }
int OpenIMP_P2_DMARegion(uint32_t *b, uint32_t *s, void **m) { (void)b; (void)s; (void)m; return -1; }
int openimp_t31_ivs_source_active(int fs_chn) { (void)fs_chn; return 0; }
void openimp_t31_ivs_capture(int fs_chn, const void *frame) { (void)fs_chn; (void)frame; }
int64_t IMP_System_GetTimeStamp(void) { return 1; }
int64_t OpenIMP_P0_NormalizeMonotonicTimeStamp(uint64_t t) { return (int64_t)t; }

int test_ioctl(int fd, unsigned long command, ...)
{
    va_list a; uint32_t *w;
    assert(fd == 42);
    va_start(a, command); w = va_arg(a, uint32_t *); va_end(a);
    if (command == TISP_VIDIOC_DQBUF) {
        uint64_t now = p1_monotonic_us();

        __atomic_add_fetch(&dq_calls, 1, __ATOMIC_RELAXED);
        if (!sensor_on || now < next_frame_us) {
            errno = ENODATA;            /* the open driver: no wait */
            return -1;
        }
        next_frame_us += frame_interval_us;
        w[0] = (uint32_t)(next_idx++ % 4);
        return 0;
    }
    assert(command == TISP_VIDIOC_QBUF);
    return 0;
}

static double ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e3 +
           (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

static void *stop_later(void *arg)
{
    struct timespec nap = { 0, 100 * 1000000L };

    (void)arg;
    nanosleep(&nap, NULL);
    p1_dqw_stop(1);
    return NULL;
}

int main(void)
{
    struct openimp_fs_channel *chn;
    struct timespec t0;
    IMPFrameInfo *f = NULL;
    void *wf = NULL;
    pthread_t th;
    double dt;
    int i, got = 0, waits = 0, probes;

    setenv("OPENIMP_FALLBACK_FRAME", "1", 1);
    setenv("OPENIMP_FALLBACK_TIMEOUT_MS", "60000", 1);   /* no synthetic frame */
    prepare_p1();
    chn = &p1.channels[1];
    memset(chn, 0, sizeof(*chn));
    chn->fd = 42; chn->created = 1; chn->pool_id = -1; chn->enabled = 1;
    chn->attr.picWidth = 8; chn->attr.picHeight = 8;
    for (i = 0; i < 4; i++) {
        pic[i] = calloc(1, 96);
        chn->buffers[i].physical = 0x6000000u + (uint32_t)i * 0x1000u;
        chn->buffers[i].virtual_address = pic[i];
        chn->buffers[i].size = 96;
        chn->buffers[i].queued = 1;
    }
    chn->buffer_count = 4;
    lock_p1(); p1_fb_init_locked(); unlock_p1();
    assert(p1_fb[1].enabled);           /* the worker path is on */

    /* stalled sensor: WaitFrame(20) is back after 20 ms, not 20 x 21 ms */
    for (i = 0; i < 3; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        assert(OpenIMP_T40_FrameSource_WaitFrame(1, &wf, 20) != 0 && !wf);
        dt = ms_since(&t0);
        printf("stalled WaitFrame(20): %.1f ms\n", dt);
        assert(dt >= 15.0 && dt < 30.0);
    }
    /* GetFrame(1000): one second, not 1000 s */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(IMP_FrameSource_GetFrame(1, &f) == -1 && !f);
    dt = ms_since(&t0);
    printf("stalled GetFrame(1000): %.1f ms\n", dt);
    assert(dt >= 900.0 && dt < 1050.0);

    /* DisableChn during a wait: the consumer is back at once */
    assert(pthread_create(&th, NULL, stop_later, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(IMP_FrameSource_GetFrame(1, &f) == -1);
    dt = ms_since(&t0);
    pthread_join(th, NULL);
    printf("wait ended by the channel stop: %.1f ms\n", dt);
    assert(dt >= 80.0 && dt < 160.0);

    /* sensor at 20 fps: a few waits and bounded probes per frame */
    sensor_on = 1;
    next_frame_us = p1_monotonic_us() + frame_interval_us;
    dq_calls = 0;
    while (got < 20) {
        waits++;
        if (OpenIMP_T40_FrameSource_WaitFrame(1, &wf, 20) == 0) {
            assert(wf);
            assert(IMP_FrameSource_ReleaseFrame(1, wf) == 0);
            wf = NULL;
            got++;
        }
        assert(waits < 200);
    }
    probes = dq_calls;
    printf("20 fps: %d frames, %d waits (%.1f per frame), %d DQBUF probes (%.1f per frame)\n",
           got, waits, waits / (double)got, probes, probes / (double)got);
    assert(waits <= got * 4);           /* 50 ms interval / 20 ms slices */
    assert(probes <= got * 60);         /* 1 ms probes inside the wait only */
    printf("framesource_wait_test OK\n");
    return 0;
}
