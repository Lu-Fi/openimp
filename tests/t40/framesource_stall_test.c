/* T40 frame gate (openimp_p1.c p1_edge_take): with an ISP that delivers
 * nothing, GetFrame and the encoder pull must time out without a DQBUF
 * (the vendor driver would never return from it), frames left done while
 * nobody consumed must still be handed out, and DisableChn
 * must finish while the edge thread sleeps in WAIT_FRAME. */
#include <assert.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define ioctl test_ioctl
#include "../../src/t40/openimp_p1.c"
#undef ioctl

static int dq_calls, streamoff_calls, next_idx;
static volatile int done_frames;        /* buffers the "ISP" completed */
static sem_t isp_edge;                  /* WAIT_FRAME completions */
static unsigned char *pic[4];

volatile uint32_t openimp_video_drop_frames;
int openimp_video_drop_set(void (*cb)(void)) { (void)cb; return 0; }
int openimp_log_level;
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
    if (command == TISP_VIDIOC_WAIT_FRAME) {
        while (sem_wait(&isp_edge) != 0)
            ;
        return 0;
    }
    if (command == TISP_VIDIOC_DQBUF) {
        dq_calls++;
        assert(done_frames > 0 && "DQBUF without a done buffer never returns on the vendor driver");
        __sync_sub_and_fetch(&done_frames, 1);
        w[0] = (uint32_t)(next_idx++ % 4);
        return 0;
    }
    if (command == TISP_VIDIOC_STREAMOFF) { streamoff_calls++; return 0; }
    if (command == TISP_VIDIOC_REQBUFS) return 0;
    assert(command == TISP_VIDIOC_QBUF);
    return 0;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void isp_frame(void)
{
    __sync_add_and_fetch(&done_frames, 1);
    sem_post(&isp_edge);
}

int main(void)
{
    struct openimp_fs_channel *chn;
    IMPFrameInfo *f = NULL;
    double t0, dt;
    int i;

    sem_init(&isp_edge, 0, 0);
    prepare_p1();
    chn = &p1.channels[1];
    memset(chn, 0, sizeof(*chn));
    chn->fd = 42; chn->created = 1; chn->pool_id = -1; chn->enabled = 1;
    chn->attr.picWidth = 8; chn->attr.picHeight = 8;
    chn->attr.outFrmRateNum = 25; chn->attr.outFrmRateDen = 1;
    for (i = 0; i < 4; i++) {
        pic[i] = calloc(1, 96);
        chn->buffers[i].physical = 0x6000000u + (uint32_t)i * 0x1000u;
        chn->buffers[i].virtual_address = pic[i];
        chn->buffers[i].size = 96;
        chn->buffers[i].queued = 1;
    }
    chn->buffer_count = 4;
    p1_edge_start(1);
    assert(chn->edge_running);

    /* 1. stalled ISP: the encoder pull and the public GetFrame time out */
    t0 = now_s();
    assert(OpenIMP_P1_GetFrameTimed(1, (void *)&f, 300) == -1 && !f);
    dt = now_s() - t0;
    assert(dt >= 0.25 && dt < 0.8);
    t0 = now_s();
    assert(IMP_FrameSource_GetFrame(1, &f) == -1 && !f);
    dt = now_s() - t0;
    assert(dt >= 1.9 && dt < 3.0);         /* vendor: 2 s */
    assert(dq_calls == 0);

    /* 2. frames flow: one edge, one frame */
    isp_frame();
    assert(IMP_FrameSource_GetFrame(1, &f) == 0 && f);
    assert(dq_calls == 1);
    assert(IMP_FrameSource_ReleaseFrame(1, f) == 0);
    for (i = 0; i < 20; i++) {
        f = NULL;
        isp_frame();
        assert(OpenIMP_P1_GetFrameTimed(1, (void *)&f, 200) == 0 && f);
        assert(IMP_FrameSource_ReleaseFrame(1, f) == 0);
    }
    assert(dq_calls == 21);

    /* 3. no consumer for 400 ms: the ISP fills the queued buffers and
     * stops signalling; the done frames are still handed out afterwards,
     * then the stall times out again without a DQBUF */
    for (i = 0; i < 3; i++)
        isp_frame();
    usleep(400000);
    for (i = 0; i < 3; i++) {
        f = NULL;
        assert(OpenIMP_P1_GetFrameTimed(1, (void *)&f, 50) == 0 && f);
        assert(IMP_FrameSource_ReleaseFrame(1, f) == 0);
    }
    f = NULL;
    assert(OpenIMP_P1_GetFrameTimed(1, (void *)&f, 200) == -1 && !f);
    assert(dq_calls == 24);

    /* 4. teardown while the edge thread sleeps in WAIT_FRAME */
    t0 = now_s();
    assert(IMP_FrameSource_DisableChn(1) == 0);
    dt = now_s() - t0;
    assert(dt < 1.0);
    assert(!chn->edge_running && streamoff_calls == 1);
    f = NULL;
    assert(IMP_FrameSource_GetFrame(1, &f) == -1);
    printf("framesource_stall_test OK (stalled GetFrame -1 after 2 s, no DQBUF; DisableChn %.3f s)\n", dt);
    return 0;
}
