/* T40 / T41 AI record chain order (#591): vendor T40 1.3.1 _ai_record_thread
 * (0xa503c) and T41 1.2.5 run NS over all 10 ms blocks of a frame, then AGC
 * over all blocks, then the HPF (_ai_HPF_Filter), then the howling
 * suppression, then the software volume (_audio_set_volume) last.  The NS
 * gets int16-range floats, a zeroed output block and its output is written
 * back.  The effects, the howling suppression and the driver ioctl are
 * stubbed; the order is logged. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

int64_t IMP_System_GetTimeStamp(void) { return 0; }

static int fake_ioctl(int fd, unsigned long request, long arg);
#define ioctl(fd, req, arg) fake_ioctl((fd), (req), (long)(arg))
#define _Static_assert(...) extern int audio_chain_test_unused
#include "../../src/t40/openimp_p3_audio.c"
#undef _Static_assert
#undef ioctl

#define N 320 /* 20 ms at 16 kHz: two 10 ms blocks */
static char order[64];
static int ns_calls, agc_calls, hpf_calls, hs_calls;

static void log_step(char c)
{
    size_t n = strlen(order);

    assert(n + 1 < sizeof(order));
    order[n] = c;
    order[n + 1] = 0;
}

static int fake_ioctl(int fd, unsigned long request, long arg)
{
    (void)fd;
    if ((uint32_t)request == (uint32_t)AMIC_AI_GET_STREAM) {
        P3AudioInputStream *st = (P3AudioInputStream *)arg;
        int16_t *pcm = (int16_t *)st->data;
        int i;

        assert(st->size == N * sizeof(int16_t));
        for (i = 0; i < N; i++)
            pcm[i] = 1000;
    }
    return 0;
}

static void ns_stub(void *h, const float *const *in, int bands,
                    float *const *out)
{
    int i;

    (void)h;
    assert(bands == 1);
    /* NS first: sees the captured samples, a zeroed output block */
    for (i = 0; i < 160; i++) {
        assert(in[0][i] == 1000.0f);
        assert(out[0][i] == 0.0f);
        out[0][i] = 500.0f;
    }
    if (!ns_calls || order[strlen(order) - 1] != 'N')
        log_step('N');
    ns_calls++;
}

static int agc_stub(void *h, const int16_t *const *in, size_t bands,
                    size_t samples, int16_t *const *out, int32_t level,
                    int32_t *out_level, int16_t echo, uint8_t *sat)
{
    size_t i;

    (void)h; (void)bands; (void)level; (void)out_level; (void)echo; (void)sat;
    assert(samples == 160);
    /* AGC after the NS of ALL blocks */
    assert(ns_calls == N / 160);
    for (i = 0; i < samples; i++) {
        assert(in[0][i] == 500);
        out[0][i] = 400;
    }
    if (!agc_calls)
        log_step('A');
    agc_calls++;
    return 0;
}

static int hpf_stub(int16_t *state, int16_t *data, int count)
{
    int i;

    (void)state;
    assert(count == N && agc_calls == N / 160);
    for (i = 0; i < count; i++) {
        assert(data[i] == 400);
        data[i] = 300;
    }
    log_step('H');
    hpf_calls++;
    return 0;
}

void openimp_t23_ai_pre_effects(int16_t *samples, int count, int sample_rate)
{
    int i;

    assert(count == N && sample_rate == 16000 && hpf_calls == 1);
    for (i = 0; i < count; i++) {
        assert(samples[i] == 300);
        samples[i] = 100;
    }
    log_step('S');
    hs_calls++;
}

int main(void)
{
    IMPAudioFrame frame;
    const int16_t *pcm;
    int i;

    p3_audio.fd = 3;
    p3_audio.ai_attr.samplerate = 16000;
    p3_audio.ai_attr.numPerFrm = N;
    p3_audio.ai_attr.soundmode = AUDIO_SOUND_MODE_MONO;
    p3_audio.ai_attr.bitwidth = AUDIO_BIT_WIDTH_16;
    p3_audio.ai_channel_enabled = 1;
    p3_audio.ns_process = ns_stub;
    p3_audio.ns_enabled = 1;
    p3_audio.agc_process = agc_stub;
    p3_audio.agc_enabled = 1;
    p3_audio.hpf_process = hpf_stub;
    p3_audio.hpf_enabled = 1;
    /* software volume last: +6.02 dB on the howling-suppression output */
    p3_audio.ai_vgain = 2.0;

    assert(IMP_AI_GetFrame(0, 0, &frame, BLOCK) == 0);
    assert(strcmp(order, "NAHS") == 0);
    assert(ns_calls == 2 && agc_calls == 2 && hpf_calls == 1 && hs_calls == 1);
    pcm = (const int16_t *)(const void *)frame.virAddr;
    for (i = 0; i < N; i++)
        assert(pcm[i] == 200);
    assert(IMP_AI_ReleaseFrame(0, 0, &frame) == 0);
    printf("audio chain test: PASS (NS, AGC, HPF, HS, volume)\n");
    return 0;
}
