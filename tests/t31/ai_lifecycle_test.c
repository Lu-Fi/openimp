/*
 * ai_lifecycle_test - the AI/AO device lifecycle of openimp_t31_audio.c
 * (T31 / T21 / T20 / T30 OSS2 path) against a fake /dev/dsp and the fake
 * libaudioProcess.so (fake_audio_process.c, found through LD_LIBRARY_PATH).
 *
 * Checks:
 *   - device/channel numbers out of range are refused;
 *   - Enable/EnableChn/DisableChn/Disable cycles leak no descriptor and
 *     work again after an injected open or configuration failure;
 *   - the AEC reference queue follows a capture queue that grew since the
 *     reference was first used (larger frames on a later EnableChn);
 *   - the microphone effects (HPF, NS, AGC) switched on and off from one
 *     thread while another one pulls frames;
 *   - an AO HPF cut-off beyond any sample rate is refused;
 *   - every GET_STREAM asks for whole 10 ms driver fragments (the OSS2
 *     driver never completes a partial one), also at 48 kHz and 44.1 kHz.
 * Meant for ASan/UBSan and TSan as well (make sanitize).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_audio.h>

#define FAKE_FD 77

#define T31_DSP_SPEED       0xc0045002UL
#define T31_AI_DISABLE_AEC  0x40045064UL
#define T31_AI_ENABLE_AEC   0x40045065UL
#define T31_AI_GET_STREAM   0x400c5068UL
#define T31_AO_SET_STREAM   0x40085069UL

typedef struct {
    void *data;
    void *aec;
    uint32_t size;
} FakeInputStream;

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static int open_count;          /* /dev/dsp descriptors open */
static int fail_open;
static int fail_speed;
static int aec_in_driver;
static uint32_t chunk_bytes = 320;  /* bytes per GET_STREAM */
static int fake_rate = 16000;       /* last SNDCTL_DSP_SPEED */
static int partial_requests;        /* GET_STREAM sizes the driver hangs on */

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, void *arg);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/dsp"))
        return __real_open(path, flags, 0);
    pthread_mutex_lock(&fake_lock);
    if (fail_open) {
        pthread_mutex_unlock(&fake_lock);
        errno = EBUSY;
        return -1;
    }
    open_count++;
    pthread_mutex_unlock(&fake_lock);
    return FAKE_FD;
}

int __wrap_close(int fd)
{
    if (fd != FAKE_FD)
        return __real_close(fd);
    pthread_mutex_lock(&fake_lock);
    open_count--;
    pthread_mutex_unlock(&fake_lock);
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, void *arg)
{
    if (fd != FAKE_FD)
        return __real_ioctl(fd, request, arg);
    if (request == T31_DSP_SPEED && fail_speed)
        return -1;
    if (request == T31_DSP_SPEED) {
        fake_rate = *(int *)arg;
        return 0;
    }
    if (request == T31_AI_ENABLE_AEC) {
        aec_in_driver = 1;
        return 0;
    }
    if (request == T31_AI_DISABLE_AEC) {
        aec_in_driver = 0;
        return 0;
    }
    if (request == T31_AI_GET_STREAM) {
        FakeInputStream *stream = arg;
        uint32_t n = chunk_bytes < stream->size ? chunk_bytes : stream->size;
        uint32_t fragment = (uint32_t)(fake_rate / 100) * 2U;

        /* xb_snd_dsp.c ai_copy_to_user copies whole fragments only: a
         * request with a partial one left never finishes in the driver */
        if (fragment && stream->size % fragment) {
            __atomic_add_fetch(&partial_requests, 1, __ATOMIC_RELAXED);
            stream->size = 0;
            usleep(500);
            return 0;
        }

        memset(stream->data, 0x11, n);
        if (stream->aec)
            memset(stream->aec, 0x22, n);
        stream->size = n;
        usleep(500);
        return 0;
    }
    if (request == T31_AO_SET_STREAM)
        return 0;
    return 0;                   /* CHANNELS/SETFMT/ENABLE/DISABLE/GAIN... */
}

int64_t IMP_System_GetTimeStamp(void)
{
    return 0;
}

static IMPAudioIOAttr attr_for(int rate, int samples, int frames)
{
    IMPAudioIOAttr attr;

    memset(&attr, 0, sizeof(attr));
    attr.samplerate = rate;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = frames;
    attr.numPerFrm = samples;
    attr.chnCnt = 1;
    return attr;
}

static int pull_frames(int count)
{
    int got = 0;

    while (count-- > 0) {
        IMPAudioFrame frame;

        if (IMP_AI_PollingFrame(0, 0, 1000) != 0 ||
            IMP_AI_GetFrame(0, 0, &frame, BLOCK) != 0)
            break;
        IMP_AI_ReleaseFrame(0, 0, &frame);
        got++;
    }
    return got;
}

static void test_bad_numbers(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    int value;

    CHECK(IMP_AI_SetPubAttr(-1, &attr) != 0, "AI SetPubAttr -1");
    CHECK(IMP_AI_SetPubAttr(2, &attr) != 0, "AI SetPubAttr 2");
    CHECK(IMP_AI_Enable(-1) != 0 && IMP_AI_Enable(2) != 0, "AI Enable");
    CHECK(IMP_AI_EnableChn(0, 1) != 0 && IMP_AI_EnableChn(0, -1) != 0,
          "AI EnableChn channel");
    CHECK(IMP_AI_GetVol(0, 0, &value) == 0 && value == 0,
          "AI GetVol before EnableChn reads %d, vendor 0", value);
    /* vendor: out-of-range volume is clamped to -30..120, the call succeeds */
    CHECK(IMP_AI_SetVol(0, 0, 121) == 0 && IMP_AI_GetVol(0, 0, &value) == 0 &&
          value == 120, "AI SetVol 121 -> %d", value);
    CHECK(IMP_AI_SetVol(0, 0, -31) == 0 && IMP_AI_GetVol(0, 0, &value) == 0 &&
          value == -30, "AI SetVol -31 -> %d", value);
    CHECK(IMP_AI_SetVol(0, 1, 60) != 0 && IMP_AI_SetVol(2, 0, 60) != 0 &&
          IMP_AI_GetVol(0, 0, NULL) != 0, "AI SetVol device/channel");
    CHECK(IMP_AI_SetGain(0, 0, 32) != 0 && IMP_AI_SetGain(0, 0, -1) != 0,
          "AI SetGain range");
    CHECK(IMP_AI_GetVol(0, 1, &value) != 0, "AI GetVol channel");
    CHECK(IMP_AO_SetPubAttr(1, &attr) != 0, "AO SetPubAttr 1");
    CHECK(IMP_AO_Enable(1) != 0 && IMP_AO_Enable(-1) != 0, "AO Enable");
    /* vendor: AO SetVol/GetVol need the AO enabled (here: not yet) */
    CHECK(IMP_AO_SetVol(0, 0, 60) != 0 && IMP_AO_GetVol(0, 0, &value) != 0,
          "AO SetVol/GetVol with the AO disabled");
    CHECK(open_count == 0, "%d descriptors open", open_count);
}

/* #592: EnableChn resets the volume to 60, the AI record is 0 before the
 * first EnableChn (vendor aiDev), AO volume needs an enabled AO. */
static void test_volume(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    int value = -1;

    CHECK(IMP_AO_SetPubAttr(0, &attr) == 0 && IMP_AO_Enable(0) == 0,
          "AO enable");
    CHECK(IMP_AO_SetVol(0, 0, 500) == 0 && IMP_AO_GetVol(0, 0, &value) == 0 &&
          value == 120, "AO SetVol 500 -> %d", value);
    CHECK(IMP_AO_SetVol(0, 0, -99) == 0 && IMP_AO_GetVol(0, 0, &value) == 0 &&
          value == -30, "AO SetVol -99 -> %d", value);
    CHECK(IMP_AO_EnableChn(0, 0) == 0 && IMP_AO_GetVol(0, 0, &value) == 0 &&
          value == 60, "AO EnableChn resets to 60, got %d", value);
    CHECK(IMP_AO_SetVol(0, 0, 80) == 0, "AO SetVol 80");
    CHECK(IMP_AO_DisableChn(0, 0) == 0 && IMP_AO_EnableChn(0, 0) == 0 &&
          IMP_AO_GetVol(0, 0, &value) == 0 && value == 60,
          "AO re-EnableChn resets to 60, got %d", value);
    IMP_AO_DisableChn(0, 0);
    IMP_AO_Disable(0);
    CHECK(IMP_AO_GetVol(0, 0, &value) != 0, "AO GetVol after Disable");

    CHECK(IMP_AI_SetPubAttr(0, &attr) == 0 && IMP_AI_Enable(0) == 0,
          "AI enable");
    CHECK(IMP_AI_SetVol(0, 0, 90) == 0, "AI SetVol 90");
    CHECK(IMP_AI_EnableChn(0, 0) == 0 && IMP_AI_GetVol(0, 0, &value) == 0 &&
          value == 60, "AI EnableChn resets to 60, got %d", value);
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);

    /* #660: libimp keeps the volume per device; device 1 reads 0 after
     * Enable even though device 0 was set before (T30X b94: vendor 0) */
    CHECK(IMP_AI_SetPubAttr(1, &attr) == 0 && IMP_AI_Enable(1) == 0 &&
          IMP_AI_GetVol(1, 0, &value) == 0 && value == 0,
          "AI device 1 GetVol after Enable reads %d, vendor 0", value);
    CHECK(IMP_AI_EnableChn(1, 0) == 0 && IMP_AI_GetVol(1, 0, &value) == 0 &&
          value == 60, "AI device 1 EnableChn resets to 60, got %d", value);
    CHECK(IMP_AI_GetVol(0, 0, &value) == 0 && value == 60,
          "AI device 0 keeps its own volume, got %d", value);
    IMP_AI_DisableChn(1, 0);
    IMP_AI_Disable(1);
}

/* #272: EnableAec runs libaudioProcess's AEC like libimp: created with the
 * AI rate and the WebRTC profile path, called per frame with {reference,
 * mic, out = mic, bytes}; OPENIMP_AEC=aecm keeps OpenIMP's AECM. */
static void test_lib_aec(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    void *lib = dlopen("libaudioProcess.so", RTLD_NOW);
    void (*info)(int *, int *, const char **) =
        lib ? (void (*)(int *, int *, const char **))dlsym(lib, "fake_aec_info")
            : NULL;
    int calls0 = 0, calls = 0, rate = 0;
    const char *profile = "";
    IMPAudioFrame frame;

    CHECK(info != NULL, "fake library with AEC");
    if (!info)
        return;
    info(&calls0, &rate, &profile);
    CHECK(IMP_AI_SetPubAttr(0, &attr) == 0 && IMP_AI_Enable(0) == 0 &&
          IMP_AI_EnableChn(0, 0) == 0, "channel");
    CHECK(IMP_AI_EnableAec(0, 0, 0, 0) == 0, "EnableAec");
    CHECK(pull_frames(3) == 3, "frames with AEC");
    CHECK(IMP_AI_PollingFrame(0, 0, 1000) == 0 &&
          IMP_AI_GetFrame(0, 0, &frame, BLOCK) == 0, "frame");
    /* mic 0x1111 minus reference 0x2222 */
    CHECK(((int16_t *)(void *)frame.virAddr)[0] == (int16_t)(0x1111 - 0x2222),
          "libaudioProcess AEC output, got %d",
          ((int16_t *)(void *)frame.virAddr)[0]);
    IMP_AI_ReleaseFrame(0, 0, &frame);
    info(&calls, &rate, &profile);
    CHECK(calls - calls0 >= 4 && rate == 16000 &&
          strcmp(profile, "/etc/webrtc_profile.ini") == 0,
          "aec calls %d rate %d profile '%s'", calls - calls0, rate, profile);
    CHECK(IMP_AI_DisableAec(0, 0) == 0, "DisableAec");
    setenv("OPENIMP_AEC", "aecm", 1);
    CHECK(IMP_AI_EnableAec(0, 0, 0, 0) == 0, "EnableAec AECM");
    info(&calls0, &rate, &profile);
    CHECK(pull_frames(3) == 3, "frames with AECM");
    info(&calls, &rate, &profile);
    CHECK(calls == calls0, "OPENIMP_AEC=aecm: library AEC not called");
    IMP_AI_DisableAec(0, 0);
    unsetenv("OPENIMP_AEC");
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
}

static void test_cycles(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    int i;

    for (i = 0; i < 30; i++) {
        CHECK(IMP_AI_SetPubAttr(0, &attr) == 0, "SetPubAttr");
        CHECK(IMP_AI_Enable(0) == 0, "Enable");
        CHECK(IMP_AI_EnableChn(0, 0) == 0, "EnableChn");
        CHECK(pull_frames(2) == 2, "frames (cycle %d)", i);
        CHECK(IMP_AI_DisableChn(0, 0) == 0, "DisableChn");
        CHECK(IMP_AI_Disable(0) == 0, "Disable");
    }
    /* failures on the way up leave nothing open, the next Enable works */
    fail_open = 1;
    CHECK(IMP_AI_Enable(0) != 0, "Enable with open failing");
    fail_open = 0;
    fail_speed = 1;
    CHECK(IMP_AI_Enable(0) != 0, "Enable with configuration failing");
    fail_speed = 0;
    CHECK(open_count == 0, "%d descriptors left after failed Enable",
          open_count);
    CHECK(IMP_AI_Enable(0) == 0 && IMP_AI_EnableChn(0, 0) == 0 &&
          pull_frames(2) == 2, "Enable after failures");
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
    CHECK(open_count == 0, "%d descriptors left", open_count);
}

/* The reference queue was sized for 10 ms frames; the next channel runs
 * 100 ms frames with a deeper queue, and the reference is still kept. */
static void test_reference_grows(void)
{
    IMPAudioIOAttr small = attr_for(16000, 160, 2);
    IMPAudioIOAttr large = attr_for(16000, 1600, 20);

    CHECK(IMP_AI_SetPubAttr(0, &small) == 0 && IMP_AI_Enable(0) == 0 &&
          IMP_AI_EnableChn(0, 0) == 0, "small channel");
    CHECK(IMP_AI_EnableAecRefFrame(0, 0, 0, 0) == 0, "EnableAecRefFrame");
    CHECK(pull_frames(3) == 3, "small frames");
    CHECK(IMP_AI_DisableAecRefFrame(0, 0, 0, 0) == 0, "DisableAecRefFrame");
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);

    chunk_bytes = 1280;
    CHECK(IMP_AI_SetPubAttr(0, &large) == 0 && IMP_AI_Enable(0) == 0 &&
          IMP_AI_EnableChn(0, 0) == 0, "large channel");
    usleep(60000);              /* let the queue fill well past 10 ms frames */
    CHECK(pull_frames(25) == 25, "large frames");
    CHECK(IMP_AI_EnableAecRefFrame(0, 0, 0, 0) == 0, "EnableAecRefFrame 2");
    CHECK(pull_frames(25) == 25, "large frames with reference");
    IMP_AI_DisableAecRefFrame(0, 0, 0, 0);
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
    chunk_bytes = 320;
}

static int effects_stop;

static void *effects_toggler(void *arg)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    IMPAudioAgcConfig agc = { .TargetLevelDbfs = 10, .CompressionGaindB = 9 };

    (void)arg;
    while (!__atomic_load_n(&effects_stop, __ATOMIC_RELAXED)) {
        IMP_AI_EnableNs(&attr, 2);
        IMP_AI_EnableAgc(&attr, agc);
        IMP_AI_EnableHpf(&attr);
        usleep(200);
        IMP_AI_DisableNs();
        IMP_AI_DisableAgc();
        IMP_AI_DisableHpf();
    }
    return NULL;
}

static void test_effects_while_capturing(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    pthread_t thread;
    int got;

    CHECK(IMP_AI_SetPubAttr(0, &attr) == 0 && IMP_AI_Enable(0) == 0 &&
          IMP_AI_EnableChn(0, 0) == 0, "channel");
    if (IMP_AI_EnableNs(&attr, 2) != 0) {
        CHECK(0, "fake libaudioProcess.so not found (LD_LIBRARY_PATH)");
        return;
    }
    IMP_AI_DisableNs();
    pthread_create(&thread, NULL, effects_toggler, NULL);
    got = pull_frames(300);
    __atomic_store_n(&effects_stop, 1, __ATOMIC_RELAXED);
    pthread_join(thread, NULL);
    CHECK(got == 300, "%d of 300 frames", got);
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
}

/* 48 kHz and 44.1 kHz mono: 960 and 882 byte fragments, neither divides
 * the 1280 bytes the capture thread used to ask for. */
static void test_whole_fragments(void)
{
    static const int rates[] = { 48000, 44100, 8000, 16000 };
    size_t i;

    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        IMPAudioIOAttr attr = attr_for(rates[i], rates[i] / 50, 8);

        partial_requests = 0;
        chunk_bytes = (uint32_t)(rates[i] / 100) * 2U;
        CHECK(IMP_AI_SetPubAttr(0, &attr) == 0 && IMP_AI_Enable(0) == 0 &&
              IMP_AI_EnableChn(0, 0) == 0, "channel at %d Hz", rates[i]);
        CHECK(pull_frames(5) == 5, "frames at %d Hz", rates[i]);
        CHECK(__atomic_load_n(&partial_requests, __ATOMIC_RELAXED) == 0,
              "%d GET_STREAM request(s) with a partial fragment at %d Hz",
              partial_requests, rates[i]);
        IMP_AI_DisableChn(0, 0);
        IMP_AI_Disable(0);
    }
    chunk_bytes = 320;
}

/* IMP_AI_SetHpfCoFrequency reaches the filter: no value = the default
 * pointer form, a cut-off = a float biquad at the stream's rate (neo) */
static void test_ai_hpf_cutoff(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);
    void *lib = dlopen("libaudioProcess.so", RTLD_NOW);
    void (*last)(unsigned char *) =
        lib ? (void (*)(unsigned char *))dlsym(lib, "fake_hpf_last_state") : NULL;
    unsigned char st[32];
    float bq[7];

    CHECK(last != NULL, "fake library");
    if (!last)
        return;
    CHECK(IMP_AI_SetPubAttr(0, &attr) == 0 && IMP_AI_Enable(0) == 0 &&
          IMP_AI_EnableChn(0, 0) == 0, "channel");
    CHECK(IMP_AI_SetHpfCoFrequency(-5) == 0, "SetHpfCoFrequency -5");
    CHECK(IMP_AI_EnableHpf(&attr) != 0, "AI HPF negative cut-off accepted");
    CHECK(IMP_AI_SetHpfCoFrequency(16001) == 0 &&
          IMP_AI_EnableHpf(&attr) != 0, "AI HPF above the rate accepted");
    CHECK(IMP_AI_SetHpfCoFrequency(0) == 0 &&
          IMP_AI_EnableHpf(&attr) == 0, "AI HPF default refused");
    CHECK(pull_frames(3) == 3, "frames, default");
    last(st);
    memcpy(bq, st, 4);
    CHECK(bq[0] == 0.0f, "default HPF carries a biquad");
    IMP_AI_DisableHpf();
    CHECK(IMP_AI_SetHpfCoFrequency(1000) == 0 &&
          IMP_AI_EnableHpf(&attr) == 0, "AI HPF 1000 Hz refused");
    CHECK(pull_frames(3) == 3, "frames, 1000 Hz");
    last(st);
    memcpy(bq, st, sizeof(bq));
    /* RBJ high pass, Q 0.7071, 1 kHz at 16 kHz: b0 0.757, a1 -1.454 */
    CHECK(bq[0] > 0.7f && bq[0] < 0.8f && bq[1] < -1.4f && bq[1] > -1.6f &&
          bq[2] == bq[0] && bq[3] < -1.4f && bq[3] > -1.5f &&
          bq[4] > 0.4f && bq[4] < 0.6f && bq[5] == 0.0f && bq[6] == 0.0f,
          "biquad %f %f %f %f %f", bq[0], bq[1], bq[2], bq[3], bq[4]);
    CHECK(fabsf(bq[1] + 2.0f * bq[0]) < 1e-5f, "b1 = -2 b0");
    IMP_AI_DisableHpf();
    IMP_AI_SetHpfCoFrequency(0);
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
}

static void test_ao_hpf_cutoff(void)
{
    IMPAudioIOAttr attr = attr_for(16000, 160, 8);

    CHECK(IMP_AO_SetHpfCoFrequency(INT_MAX) == 0, "SetHpfCoFrequency");
    CHECK(IMP_AO_EnableHpf(&attr) != 0, "AO HPF cut-off INT_MAX accepted");
    CHECK(IMP_AO_SetHpfCoFrequency(8000) == 0 &&
          IMP_AO_EnableHpf(&attr) != 0, "AO HPF at Nyquist accepted");
    CHECK(IMP_AO_SetHpfCoFrequency(300) == 0 &&
          IMP_AO_EnableHpf(&attr) == 0, "AO HPF 300 Hz refused");
    IMP_AO_DisableHpf();
    IMP_AO_SetHpfCoFrequency(0);
}

int main(void)
{
    test_bad_numbers();
    test_volume();
    test_cycles();
    test_reference_grows();
    test_effects_while_capturing();
    test_whole_fragments();
    test_ai_hpf_cutoff();
    test_ao_hpf_cutoff();
    test_lib_aec();
    CHECK(open_count == 0, "%d descriptors left at exit", open_count);
    if (failures) {
        fprintf(stderr, "AI lifecycle: %d check(s) failed\n", failures);
        return 1;
    }
    printf("AI lifecycle tests passed\n");
    return 0;
}
