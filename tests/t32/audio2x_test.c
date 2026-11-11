/* T32 2.x audio semantics (#329), host test: the vendor software volume
 * curve and clipping, gain/volume ioctl use, AI record thread start, the AO
 * block queue (busy/free with the cache, flush, clear, NOBLOCK), algorithm
 * order, and the extra exports.  /dev/dsp is faked; nothing is played. */
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int64_t IMP_System_GetTimeStamp(void) { return 0; }

static int fake_ioctl(int fd, unsigned long request, long arg);
#define ioctl(fd, req, arg) fake_ioctl((fd), (req), (long)(arg))
#define open(path, flags) fake_open(path)
static int fake_open(const char *path) { (void)path; return 77; }
/* the target ABI asserts (32-bit) do not hold on the 64-bit host */
#define _Static_assert(...) extern int t32_audio2x_test_unused
#include "../../src/t40/openimp_p3_audio.c"
#undef _Static_assert
#undef ioctl
#undef open

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t requests[256];
static unsigned request_count;
static uint32_t last_volume[3];
static long ao_bytes;
static int ao_writes;
static int16_t ao_first[4];
static int16_t ai_level = 3277;

static int fake_ioctl(int fd, unsigned long wide, long arg)
{
    /* p3_audio_parameter passes the request as an int (sign-extended here) */
    uint32_t request = (uint32_t)wide;

    (void)fd;
    pthread_mutex_lock(&fake_lock);
    if (request != AMIC_AI_GET_STREAM && request_count < 256)
        requests[request_count++] = request;
    if (request == AMIC_AI_SET_GAIN || request == AMIC_SPK_SET_GAIN)
        memcpy(last_volume, (const void *)arg, sizeof(last_volume));
    if (request == AMIC_AO_SET_STREAM) {
        const P3AudioOutputStream *s = (const P3AudioOutputStream *)arg;

        if (!ao_writes)
            memcpy(ao_first, s->data, sizeof(ao_first));
        ao_bytes += s->size;
        ao_writes++;
    }
    pthread_mutex_unlock(&fake_lock);
    if (request == AMIC_AI_GET_STREAM) {
        const P3AudioInputStream *s = (const P3AudioInputStream *)arg;
        int16_t *d = s->data;
        uint32_t i;

        for (i = 0; i < s->size / 2; i++)
            d[i] = (int16_t)((i & 1) ? -ai_level : ai_level);
        usleep(2000);
    }
    return 0;
}

static unsigned count_request(uint32_t request)
{
    unsigned i, n = 0;

    pthread_mutex_lock(&fake_lock);
    for (i = 0; i < request_count; i++)
        n += requests[i] == request;
    pthread_mutex_unlock(&fake_lock);
    return n;
}

static int16_t vendor_scale(int16_t x, int vol)
{
    double f = vol <= -30 ? 0.0 : pow(10.0, (vol * 0.5 - 30.0) / 20.0);
    int v = (int)((double)x * f);

    return (int16_t)(v >= 0x8000 ? 0x7fff : v < -0x7fff ? -0x7fff : v);
}

static void test_volume_curve(void)
{
    int16_t in[4] = { 3277, -3277, 32767, -32768 }, out[4];
    int v = 70;

    assert(fabs(t32_vol_factor(&v) - pow(10.0, 0.25)) < 1e-12 && v == 70);
    v = -31;
    assert(t32_vol_factor(&v) == 0.0 && v == -30);
    v = 121;
    assert(fabs(t32_vol_factor(&v) - pow(10.0, 1.5)) < 1e-9 && v == 120);
    /* vendor QEMU run: tone peak 3277 at volume 70 -> 5827 */
    t32_apply_volume(in, out, 4, pow(10.0, 0.25));
    assert(out[0] == 5827 && out[1] == -5827 && out[2] == 32767 && out[3] == -32767);
    t32_apply_volume(in, out, 4, 1.0);
    assert(out[0] == 3277 && out[3] == -32767);   /* clipped also at unity */
}

static void test_ai(void)
{
    IMPAudioIOAttr attr;
    IMPAudioIChnParam cp;
    IMPAudioFrame f;
    int value = -1, i;
    const int16_t *s;
    AudioAI_Type order[3];
    AudioAI_Type mine[3] = { AI_MODULE_HPF, AI_MODULE_NS, AI_MODULE_AGC };

    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    attr.chnCnt = 2;
    assert(IMP_AI_SetPubAttr(0, &attr) == -1);      /* mono needs chnCnt 1 */
    attr.chnCnt = 1;
    assert(IMP_AI_SetPubAttr(2, &attr) == -1);
    assert(IMP_AI_SetPubAttr(0, &attr) == 0);
    assert(request_count == 0);                     /* no ioctl in SetPubAttr */
    assert(IMP_AI_Enable(0) == 0);
    assert(count_request(AMIC_AI_SET_PARAM) == 1 &&
           count_request(AMIC_AI_ENABLE_STREAM) == 1);
    memset(&cp, 0, sizeof cp);
    assert(IMP_AI_SetChnParam(0, 0, &cp) == 0);
    assert(IMP_AI_EnableChn(0, 0) == -1);           /* usrFrmDepth 0 */
    cp.usrFrmDepth = 20;
    assert(IMP_AI_SetChnParam(0, 0, &cp) == 0);
    assert(IMP_AI_EnableChn(0, 0) == 0);
    assert(p3_audio.cap_running && p3_audio.cap_depth == 20);
    assert(IMP_AI_GetVol(0, 0, &value) == 0 && value == 60);

    request_count = 0;
    assert(IMP_AI_SetVol(0, 0, 70) == 0 && IMP_AI_GetVol(0, 0, &value) == 0 &&
           value == 70);
    assert(request_count == 0);                     /* software volume */
    assert(IMP_AI_SetGain(0, 0, 40) == 0 && IMP_AI_GetGain(0, 0, &value) == 0 &&
           value == 31);
    assert(request_count == 1 && requests[0] == AMIC_AI_SET_GAIN &&
           last_volume[0] == 1 && last_volume[1] == 31 && last_volume[2] == 31);
    assert(IMP_AI_SetVol(1, 1, 60) == -1 && IMP_AI_GetGain(2, 0, &value) == -1);

    assert(IMP_AI_PollingFrame(0, 0, 1000) == 0);
    assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0 && f.len == 640);
    s = (const int16_t *)(const void *)f.virAddr;
    assert(s[0] == vendor_scale(3277, 70) && s[1] == vendor_scale(-3277, 70));
    assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
    assert(IMP_AI_SetVolMute(0, 0, 1) == 0);
    for (i = 0; i < 3; i++) {                       /* frames already queued */
        assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0);
        assert(((const int16_t *)(const void *)f.virAddr)[0] == 0);
        IMP_AI_ReleaseFrame(0, 0, &f);
    }
    assert(IMP_AI_SetVolMute(0, 0, 0) == 0 && IMP_AI_GetVol(0, 0, &value) == 0 &&
           value == 70);
    assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0 &&
           ((const int16_t *)(const void *)f.virAddr)[0] == 5827);
    IMP_AI_ReleaseFrame(0, 0, &f);
    assert(IMP_AI_SetVolMute(0, 0, 2) == -1);

    assert(IMP_AI_GetAlgoOrder(0, 0, order) == 0 && order[0] == AI_MODULE_NS &&
           order[1] == AI_MODULE_AGC && order[2] == AI_MODULE_HPF);
    assert(IMP_AI_SetAlgoOrder(0, 0, mine) == 0 && IMP_AI_GetAlgoOrder(0, 0, order) == 0 &&
           order[0] == AI_MODULE_HPF && order[2] == AI_MODULE_AGC);
    assert(IMP_AI_SetAlgoOrder(2, 0, mine) == -1);
    assert(IMP_AI_SetLpfCoFrequency(3000) == 0 && t32_ai.lpf_cutoff == 3000);

    assert(IMP_AI_DisableChn(0, 0) == 0 && IMP_AI_Disable(0) == 0);
    assert(IMP_AI_SetLpfCoFrequency(1000) == 0 && t32_ai.lpf_cutoff == 3000);
}

static void send(int n, int16_t level, IMPBlock block, int expect)
{
    static int16_t pcm[320];
    IMPAudioFrame f;
    int i;

    for (i = 0; i < 320; i++)
        pcm[i] = level;
    memset(&f, 0, sizeof f);
    f.bitwidth = AUDIO_BIT_WIDTH_16;
    f.soundmode = AUDIO_SOUND_MODE_MONO;
    f.virAddr = (uint32_t *)(void *)pcm;
    f.len = sizeof pcm;
    for (i = 0; i < n; i++)
        assert(IMP_AO_SendFrame(0, 0, &f, block) == expect);
}

static void stat(int total, int free_n, int busy)
{
    IMPAudioOChnState st;

    assert(IMP_AO_QueryChnStat(0, 0, &st) == 0);
    if (st.chnTotalNum != total || st.chnFreeNum != free_n || st.chnBusyNum != busy) {
        fprintf(stderr, "stat %d %d %d, expected %d %d %d\n", st.chnTotalNum,
                st.chnFreeNum, st.chnBusyNum, total, free_n, busy);
        assert(0);
    }
}

static void test_ao(void)
{
    IMPAudioIOAttr attr;
    IMPAudioFrame big;
    int value = -1, i;
    int16_t pcm[400];
    AudioAO_Type order[3];

    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    request_count = 0;
    assert(IMP_AO_SetVol(0, 0, 50) == -1);          /* device not enabled */
    assert(IMP_AO_SetPubAttr(0, &attr) == 0 && request_count == 0);
    assert(attr.numPerFrm == 320 && t32_ao.node_samples == 640);   /* 40 ms */
    assert(IMP_AO_GetPubAttr(0, &attr) == 0 && attr.numPerFrm == 320);
    assert(IMP_AO_Enable(0) == 0);
    assert(count_request(AMIC_AO_SET_PARAM) == 1 &&
           count_request(AMIC_AO_ENABLE_STREAM) == 1);
    assert(IMP_AO_EnableChn(0, 0) == 0);
    stat(20, 20, 0);
    assert(IMP_AO_GetVol(0, 0, &value) == 0 && value == 60);
    request_count = 0;
    assert(IMP_AO_SetVol(0, 0, 50) == 0 && IMP_AO_GetVol(0, 0, &value) == 0 &&
           value == 50 && request_count == 0);
    assert(IMP_AO_SetGain(0, 0, -4) == 0 && IMP_AO_GetGain(0, 0, &value) == 0 &&
           value == 0 && request_count == 1 && requests[0] == AMIC_SPK_SET_GAIN &&
           last_volume[0] == 1);

    /* vendor QEMU run: 10 x 20 ms frames -> 5 blocks of 40 ms held (cache on) */
    send(10, 1000, BLOCK, 0);
    usleep(20000);
    stat(20, 15, 5);
    assert(ao_writes == 0);
    memset(&big, 0, sizeof big);
    big.virAddr = (uint32_t *)(void *)pcm;
    big.len = sizeof pcm;                           /* > numPerFrm samples */
    assert(IMP_AO_SendFrame(0, 0, &big, BLOCK) == -1);
    send(1, 1000, BLOCK, 0);                        /* half a block open */
    assert(IMP_AO_FlushChnBuf(0, 0) == 0);
    stat(20, 20, 0);
    assert(ao_writes == 6 && ao_bytes == 6 * 1280);
    assert(ao_first[0] == vendor_scale(1000, 50));
    assert(count_request(AMIC_AO_SYNC_STREAM) == 1);

    /* filling again: 10 blocks (busy 10 >= free 10) start the playback */
    ao_writes = 0;
    send(20, 0, BLOCK, 0);
    for (i = 0; i < 100 && ao_writes < 10; i++)
        usleep(1000);
    assert(ao_writes == 10);
    stat(20, 20, 0);

    /* paused: the queue fills, NOBLOCK fails when full, Clear empties it */
    assert(IMP_AO_PauseChn(0, 0) == 0);
    send(40, 0, NOBLOCK, 0);
    stat(20, 0, 20);
    send(1, 0, NOBLOCK, 0);                         /* half a block */
    send(1, 0, NOBLOCK, -1);                        /* no free block */
    assert(IMP_AO_ClearChnBuf(0, 0) == 0 && count_request(AMIC_AO_CLEAR_STREAM) == 1);
    stat(20, 20, 0);
    assert(IMP_AO_ResumeChn(0, 0) == 0);

    /* cache off: blocks go out at once */
    assert(IMP_AO_CacheSwitch(0, 0, 0) == 0);
    ao_writes = 0;
    send(2, 0, BLOCK, 0);
    for (i = 0; i < 100 && ao_writes < 1; i++)
        usleep(1000);
    assert(ao_writes == 1);
    assert(IMP_AO_SetVolMute(0, 0, 1) == 0 && t32_ao.vol_live == 0.0);
    assert(IMP_AO_SetVolMute(0, 0, 0) == 0 && t32_ao.vol_live == t32_ao.vol_saved);

    assert(IMP_AO_GetAlgoOrder(0, 0, order) == 0 && order[0] == AO_MODULE_HPF &&
           order[1] == AO_MODULE_AGC && order[2] == AO_MODULE_NS);
    assert(IMP_AO_SetAlgoOrder(1, 0, order) == -1);

    assert(IMP_AO_DisableChn(0, 0) == 0 && t32_ao.nodes == NULL);
    assert(IMP_AO_Disable(0) == 0 && count_request(AMIC_AO_DISABLE_STREAM) == 1);
    assert(IMP_AO_QueryChnStat(0, 0, NULL) == -1);
}

int main(void)
{
    /* the 20 vendor 2.0.0 extras exist (DMIC ones in openimp_t31_dmic.c) */
    void *extras[] = {
        (void *)IMP_AI_SetAlgoOrder, (void *)IMP_AI_GetAlgoOrder,
        (void *)IMP_AO_SetAlgoOrder, (void *)IMP_AO_GetAlgoOrder,
        (void *)IMP_AI_EnableLpf, (void *)IMP_AI_DisableLpf,
        (void *)IMP_AI_SetLpfCoFrequency, (void *)IMP_AI_EnableDrcAndEq,
        (void *)IMP_AI_DisableDrcAndEq, (void *)IMP_AO_EnableDrcAndEq,
        (void *)IMP_AO_DisableDrcAndEq, (void *)IMP_AO_EnableNs,
        (void *)IMP_AO_DisableNs,
    };
    unsigned i;

    for (i = 0; i < sizeof extras / sizeof extras[0]; i++)
        assert(extras[i]);
    test_volume_curve();
    test_ai();
    test_ao();
    printf("t32 audio2x: ok (volume curve, AI vol/gain/mute/order, AO queue 20/15/5, "
           "flush 6 blocks, cache, pause/clear)\n");
    return 0;
}
