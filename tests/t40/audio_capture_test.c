/* #409: the AI capture thread reads the driver from IMP_AI_EnableChn on, like
 * the vendor _ai_record_thread (libimp T40 1.3.1 / T41 1.2.5), into a FIFO
 * usrFrmDepth deep.  Stub OSS3 driver: one frame per FRAME_US after
 * AI_ENABLE_STREAM, a record ring of DRV_FRAMES (older frames overwritten),
 * GET_STREAM returns the oldest buffered frame at once or waits for the next.
 * Before the fix the thread only started at the first GetFrame(NOBLOCK): the
 * stale-flush drain got -1 and the driver backlog came out as extra audio. */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FRAME_US 40000      /* 640 samples at 16 kHz */
#define DRV_FRAMES 12       /* stub driver record ring */
#define DEPTH 4             /* usrFrmDepth as timps sets it */

static int64_t now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t IMP_System_GetTimeStamp(void) { return now_us(); }

static int fake_ioctl(int fd, unsigned long request, long arg);
#define ioctl(fd, req, arg) fake_ioctl((fd), (req), (long)(arg))
#define _Static_assert(...) extern int audio_capture_test_unused
#include "../../src/t40/openimp_p3_audio.c"
#undef _Static_assert
#undef ioctl

static volatile int64_t drv_start;               /* AI_ENABLE_STREAM time, 0 = off */
static volatile long drv_read;          /* frames handed out */
static volatile int drv_reads;          /* GET_STREAM calls */

static int fake_ioctl(int fd, unsigned long request, long arg)
{
    (void)fd;
    if (request == AMIC_AI_ENABLE_STREAM) {
        drv_start = now_us();
        drv_read = 0;
    } else if (request == AMIC_AI_DISABLE_STREAM) {
        drv_start = 0;
    } else if (request == AMIC_AI_GET_STREAM) {
        P3AudioInputStream *s = (P3AudioInputStream *)arg;
        long made;

        drv_reads++;
        if (!drv_start)
            return -1;
        for (;;) {
            if (!drv_start)
                return -1;
            made = (long)((now_us() - drv_start) / FRAME_US);
            if (made - drv_read > DRV_FRAMES)
                drv_read = made - DRV_FRAMES;   /* ring overwritten */
            if (made > drv_read)
                break;
            usleep(2000);
        }
        memset(s->data, 0, s->size);
        ((int32_t *)s->data)[0] = (int32_t)drv_read;    /* frame number */
        drv_read++;
    }
    return 0;
}

int main(void)
{
    IMPAudioFrame f;
    int drained = 0, frames = 0, reads_before;
    int64_t t0;

    p3_audio.fd = open("/dev/null", O_RDONLY);
    assert(p3_audio.fd >= 0);
    p3_audio.ai_attr.samplerate = 16000;
    p3_audio.ai_attr.bitwidth = AUDIO_BIT_WIDTH_16;
    p3_audio.ai_attr.soundmode = AUDIO_SOUND_MODE_MONO;
    p3_audio.ai_attr.frmNum = 20;
    p3_audio.ai_attr.numPerFrm = 640;
    p3_audio.ai_channel.usrFrmDepth = DEPTH;

    assert(IMP_AI_Enable(0) == 0);
    /* vendor: nothing reads before EnableChn; the driver gets a backlog */
    usleep(3 * FRAME_US);
    assert(IMP_AI_EnableChn(0, 0) == 0);
    assert(p3_audio.cap_running && p3_audio.cap_depth == DEPTH);
    /* idle, no client: the thread keeps the driver drained */
    usleep(20 * FRAME_US);
    reads_before = drv_reads;
    assert(reads_before >= 10);
    /* timps "flush stale": NOBLOCK until -1; at most the FIFO depth */
    while (IMP_AI_GetFrame(0, 0, &f, NOBLOCK) == 0) {
        assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
        drained++;
        assert(drained <= DEPTH + 1);
    }
    assert(drained >= 1);
    /* live: no burst, about one frame per FRAME_US */
    t0 = now_us();
    while (now_us() - t0 < 25 * FRAME_US) {
        assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0);
        assert(f.len == 1280);
        assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
        frames++;
        assert(frames <= 25 + 2);
    }
    assert(frames >= 15);       /* loose: loaded host */
    /* depth unset: fallback depth; channel restart works */
    assert(IMP_AI_DisableChn(0, 0) == 0 && !p3_audio.cap_running);
    p3_audio.ai_channel.usrFrmDepth = 0;
    assert(IMP_AI_EnableChn(0, 0) == 0);
    assert(p3_audio.cap_running && p3_audio.cap_depth == P3_CAPTURE_FRAMES);
    assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0);
    assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
    assert(IMP_AI_Disable(0) == 0 && !p3_audio.cap_running);
    printf("T40/T41 AI capture from EnableChn: all checks passed "
           "(drained %d, %d frames in %d frame times)\n", drained, frames, 25);
    return 0;
}
