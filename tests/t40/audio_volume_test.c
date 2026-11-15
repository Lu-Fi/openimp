/* T40 IMP_AI/AO_SetVol / GetVol (vendor 1.3.1 ai.c.o 0x54c8, ao.c.o 0x3338,
 * #518/#519): software volume, factor pow(10, (0.5 * vol - 30) / 20), no
 * codec ioctl; clamp -30..120; Get answers the kept value; EnableChn resets
 * to 60; samples scaled with trunc and clipped to +-32767.  The driver ioctl
 * is faked. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

int64_t IMP_System_GetTimeStamp(void) { return 0; }
static unsigned long fake_req;
static int fake_calls, fake_streams;
static int16_t fake_out[64];

static int fake_ioctl(int fd, unsigned long request, long arg);
#define ioctl(fd, req, arg) fake_ioctl((fd), (req), (long)(arg))
#define _Static_assert(...) extern int audio_volume_test_unused
#include "../../src/t40/openimp_p3_audio.c"
#undef _Static_assert
#undef ioctl

static int fake_ioctl(int fd, unsigned long request, long arg)
{
    (void)fd;
    fake_req = request;
    if ((uint32_t)request == (uint32_t)AMIC_AO_SET_STREAM) {
        const P3AudioOutputStream *st = (const P3AudioOutputStream *)arg;
        assert(st->size <= sizeof(fake_out));
        memcpy(fake_out, st->data, st->size);
        fake_streams++;
        return 0;
    }
    fake_calls++;
    return 0;
}

static double vendor_factor(int v)
{
    return v == -30 ? 0.0 : pow(10.0, (0.5 * v - 30.0) / 20.0);
}

int main(void)
{
    int v, i;
    int16_t pcm[8] = { 1000, -1000, 30000, -30000, 32767, -32768, 1, 0 };

    p3_audio.fd = 3;
    /* factor = vendor pow() curve */
    for (v = -30; v <= 120; v++)
        assert(fabs(p3_volume_factor(v) - vendor_factor(v)) <=
               1e-12 * (vendor_factor(v) + 1e-9));
    assert(p3_volume_factor(60) == 1.0 && p3_volume_factor(-30) == 0.0);

    /* AI: 60 before any Set, no codec ioctl, clamp, Get = kept value */
    assert(IMP_AI_GetVol(0, 0, &v) == 0 && v == 60);
    assert(IMP_AI_SetVol(0, 0, 90) == 0 && IMP_AI_GetVol(0, 0, &v) == 0 && v == 90);
    assert(IMP_AI_SetVol(1, 0, 200) == 0 && IMP_AI_GetVol(0, 0, &v) == 0 && v == 120);
    assert(IMP_AI_SetVol(0, 0, -99) == 0 && IMP_AI_GetVol(0, 0, &v) == 0 && v == -30);
    assert(IMP_AI_SetVol(2, 0, 60) == -1 && IMP_AI_SetVol(0, 1, 60) == -1);
    assert(IMP_AI_GetVol(0, 0, NULL) == -1);
    assert(fake_calls == 0);
    /* AI EnableChn resets to 60 (frame size 0: no capture thread here) */
    p3_audio.ai_enabled = 1;
    assert(IMP_AI_SetVol(0, 0, 80) == 0);
    assert(IMP_AI_EnableChn(0, 0) == 0 && IMP_AI_GetVol(0, 0, &v) == 0 && v == 60);
    assert(p3_audio.ai_vgain == 1.0);

    /* AO: needs device 0 enabled, channel 0; SetVol(90) reads back 90 (#518) */
    assert(IMP_AO_SetVol(0, 0, 90) == -1 && IMP_AO_GetVol(0, 0, &v) == -1);
    p3_audio.ao_enabled = 1;
    for (i = 30; i <= 120; i += 30) {
        assert(IMP_AO_SetVol(0, 0, i) == 0);
        assert(IMP_AO_GetVol(0, 0, &v) == 0 && v == i);
    }
    assert(IMP_AO_SetVol(0, 0, 500) == 0 && IMP_AO_GetVol(0, 0, &v) == 0 && v == 120);
    assert(IMP_AO_SetVol(1, 0, 60) == -1 && IMP_AO_SetVol(0, 1, 60) == -1);
    assert(fake_calls == 0);
    assert(IMP_AO_EnableChn(0, 0) == 0 && IMP_AO_GetVol(0, 0, &v) == 0 && v == 60);

    /* SendFrame: unity passes the samples unchanged, else trunc + clip */
    {
        IMPAudioFrame f;
        memset(&f, 0, sizeof(f));
        f.virAddr = (uint32_t *)(void *)pcm;
        f.len = (int)sizeof(pcm);
        assert(IMP_AO_SendFrame(0, 0, &f, BLOCK) == 0 && fake_streams == 1);
        assert(memcmp(fake_out, pcm, sizeof(pcm)) == 0);
        assert(IMP_AO_SetVol(0, 0, 72) == 0);       /* +6 dB */
        assert(IMP_AO_SendFrame(0, 0, &f, BLOCK) == 0 && fake_streams == 2);
        for (i = 0; i < 8; i++) {
            double x = pcm[i] * vendor_factor(72);
            int e = (int)x;
            if (e > 32767) e = 32767;
            if (e < -32767) e = -32767;
            assert(fake_out[i] == e);
        }
        assert(fake_out[0] == 1995 && fake_out[1] == -1995 &&
               fake_out[2] == 32767 && fake_out[3] == -32767);
        assert(pcm[0] == 1000);                     /* caller buffer untouched */
        assert(IMP_AO_SetVol(0, 0, -30) == 0);
        assert(IMP_AO_SendFrame(0, 0, &f, BLOCK) == 0);
        for (i = 0; i < 8; i++)
            assert(fake_out[i] == 0);
    }
    puts("T40 audio volume: all checks passed");
    return 0;
}
