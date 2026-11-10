/*
 * alsa_pcm_test - the ALSA PCM backend of openimp_t31_audio.c (issue #258)
 * against a fake /dev/snd/pcmC0D0{c,p} (open/ioctl/close wrapped; /dev/dsp
 * does not exist, as on a mainline kernel).
 *
 * Checks:
 *   - IMP_AI_Enable falls back to the capture PCM, asks for interleaved
 *     S16_LE at the attribute's rate and retries as stereo when the fake
 *     I2S (like the t31-aic controller) refuses one channel;
 *   - mono capture is the average of the two wire channels, the frame
 *     reaches IMP_AI_GetFrame with the right length;
 *   - IMP_AO_Enable opens the playback PCM, IMP_AO_SendFrame duplicates mono
 *     samples to both wire channels, a SendFrame that hits an underrun
 *     (-EPIPE) is recovered by PREPARE;
 *   - AEC is refused (no reference on ALSA), gain calls succeed;
 *   - Disable drops the streams and closes both descriptors.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#include <imp/imp_audio.h>

#define FAKE_CAP_FD 81
#define FAKE_PLAY_FD 82

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

static int open_count;
static int hw_params_calls;
static int hw_params_mono_refused;
static unsigned int last_rate;
static unsigned int last_channels;
static int last_access = -1, last_format = -1;
static int prepare_calls, drop_calls;
static int inject_underrun;         /* next WRITEI fails with EPIPE once */
static int16_t written[4096];       /* what the fake playback PCM received */
static size_t written_count;

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, void *arg);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/dsp")) {
        errno = ENOENT;
        return -1;
    }
    if (!strcmp(path, "/dev/snd/pcmC0D0c") ||
        !strcmp(path, "/dev/snd/pcmC0D0p")) {
        open_count++;
        return path[17] == 'c' ? FAKE_CAP_FD : FAKE_PLAY_FD;
    }
    return __real_open(path, flags, 0);
}

int __wrap_close(int fd)
{
    if (fd != FAKE_CAP_FD && fd != FAKE_PLAY_FD)
        return __real_close(fd);
    open_count--;
    return 0;
}

static int mask_bit(const struct snd_mask *mask, unsigned int bit)
{
    return (mask->bits[bit >> 5] >> (bit & 31U)) & 1U;
}

int __wrap_ioctl(int fd, unsigned long request, void *arg)
{
    if (fd != FAKE_CAP_FD && fd != FAKE_PLAY_FD)
        return __real_ioctl(fd, request, arg);
    if (request == (unsigned long)SNDRV_PCM_IOCTL_HW_PARAMS) {
        struct snd_pcm_hw_params *p = arg;
        const struct snd_interval *channels =
            &p->intervals[SNDRV_PCM_HW_PARAM_CHANNELS -
                          SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const struct snd_interval *rate =
            &p->intervals[SNDRV_PCM_HW_PARAM_RATE -
                          SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        const struct snd_mask *access =
            &p->masks[SNDRV_PCM_HW_PARAM_ACCESS -
                      SNDRV_PCM_HW_PARAM_FIRST_MASK];
        const struct snd_mask *format =
            &p->masks[SNDRV_PCM_HW_PARAM_FORMAT -
                      SNDRV_PCM_HW_PARAM_FIRST_MASK];

        hw_params_calls++;
        last_access = mask_bit(access, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        last_format = mask_bit(format, SNDRV_PCM_FORMAT_S16_LE);
        /* the t31-aic I2S is stereo only */
        if (channels->min != 2) {
            hw_params_mono_refused++;
            errno = EINVAL;
            return -1;
        }
        last_rate = rate->min;
        last_channels = channels->min;
        p->intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE -
                     SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 320;
        p->intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE -
                     SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 2560;
        return 0;
    }
    if (request == (unsigned long)SNDRV_PCM_IOCTL_SW_PARAMS)
        return 0;
    if (request == (unsigned long)SNDRV_PCM_IOCTL_PREPARE) {
        prepare_calls++;
        return 0;
    }
    if (request == (unsigned long)SNDRV_PCM_IOCTL_DROP) {
        drop_calls++;
        return 0;
    }
    if (request == (unsigned long)SNDRV_PCM_IOCTL_DRAIN)
        return 0;
    if (request == (unsigned long)SNDRV_PCM_IOCTL_READI_FRAMES) {
        struct snd_xferi *x = arg;
        int16_t *buf = x->buf;
        snd_pcm_uframes_t i;

        for (i = 0; i < x->frames; i++) {
            buf[2 * i] = 1000;          /* left */
            buf[2 * i + 1] = 3000;      /* right */
        }
        x->result = (snd_pcm_sframes_t)x->frames;
        usleep(500);
        return 0;
    }
    if (request == (unsigned long)SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
        struct snd_xferi *x = arg;
        const int16_t *buf = x->buf;
        snd_pcm_uframes_t i;

        if (inject_underrun) {
            inject_underrun = 0;
            errno = EPIPE;
            return -1;
        }
        for (i = 0; i < x->frames; i++) {
            if (written_count + 2 > sizeof(written) / sizeof(written[0]))
                break;
            written[written_count++] = buf[2 * i];
            written[written_count++] = buf[2 * i + 1];
        }
        x->result = (snd_pcm_sframes_t)x->frames;
        return 0;
    }
    return 0;
}

int64_t IMP_System_GetTimeStamp(void)
{
    return 0;
}

int main(void)
{
    IMPAudioIOAttr attr;
    IMPAudioFrame frame;
    int16_t pcm[320];
    int16_t *samples;
    int i;
    int ok = 1;

    memset(&attr, 0, sizeof(attr));
    attr.samplerate = 16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 4;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;

    /* ---- capture ---- */
    CHECK(IMP_AI_SetPubAttr(0, &attr) == 0, "AI SetPubAttr");
    CHECK(IMP_AI_Enable(0) == 0, "AI Enable without /dev/dsp");
    CHECK(open_count == 1, "capture PCM opened (%d)", open_count);
    CHECK(last_access == 1 && last_format == 1,
          "interleaved S16_LE requested (access %d format %d)", last_access,
          last_format);
    CHECK(hw_params_mono_refused == 1 && last_channels == 2 &&
              last_rate == 16000,
          "mono refused once then stereo at 16 kHz (refused %d ch %u rate %u)",
          hw_params_mono_refused, last_channels, last_rate);
    CHECK(prepare_calls == 1, "PREPARE after setup (%d)", prepare_calls);
    CHECK(IMP_AI_EnableChn(0, 0) == 0, "AI EnableChn");
    CHECK(IMP_AI_PollingFrame(0, 0, 1000) == 0, "AI PollingFrame");
    memset(&frame, 0, sizeof(frame));
    CHECK(IMP_AI_GetFrame(0, 0, &frame, BLOCK) == 0, "AI GetFrame");
    CHECK(frame.len == 640, "frame length %d", frame.len);
    samples = (int16_t *)(void *)frame.virAddr;
    for (i = 0; samples && i < frame.len / 2; i++)
        if (samples[i] != 2000)
            ok = 0;
    CHECK(ok, "mono capture is the average of both wire channels");
    CHECK(IMP_AI_ReleaseFrame(0, 0, &frame) == 0, "AI ReleaseFrame");
    CHECK(IMP_AI_SetGain(0, 0, 10) == 0, "AI SetGain without a hw control");
    CHECK(IMP_AI_EnableAec(0, 0, 0, 0) != 0, "AEC refused on ALSA");

    /* ---- playback ---- */
    CHECK(IMP_AO_SetPubAttr(0, &attr) == 0, "AO SetPubAttr");
    CHECK(IMP_AO_Enable(0) == 0, "AO Enable without /dev/dsp");
    CHECK(open_count == 2, "playback PCM opened (%d)", open_count);
    CHECK(IMP_AO_EnableChn(0, 0) == 0, "AO EnableChn");
    for (i = 0; i < 320; i++)
        pcm[i] = (int16_t)(i + 1);
    memset(&frame, 0, sizeof(frame));
    frame.virAddr = (uint32_t *)(void *)pcm;
    frame.len = sizeof(pcm);
    frame.bitwidth = AUDIO_BIT_WIDTH_16;
    frame.soundmode = AUDIO_SOUND_MODE_MONO;
    inject_underrun = 1;
    CHECK(IMP_AO_SendFrame(0, 0, &frame, BLOCK) == 0,
          "AO SendFrame (underrun recovered)");
    CHECK(prepare_calls == 3, "underrun recovery prepared again (%d)",
          prepare_calls);
    CHECK(written_count == 640, "wire samples %zu", written_count);
    ok = written_count == 640;
    for (i = 0; ok && i < 320; i++)
        if (written[2 * i] != i + 1 || written[2 * i + 1] != i + 1)
            ok = 0;
    CHECK(ok, "mono playback duplicated to both channels");
    CHECK(IMP_AO_SetGain(0, 0, 10) == 0, "AO SetGain without a hw control");
    CHECK(IMP_AO_ClearChnBuf(0, 0) == 0, "AO ClearChnBuf");
    CHECK(IMP_AO_FlushChnBuf(0, 0) == 0, "AO FlushChnBuf");

    /* ---- teardown ---- */
    CHECK(IMP_AO_DisableChn(0, 0) == 0, "AO DisableChn");
    CHECK(IMP_AO_Disable(0) == 0, "AO Disable");
    CHECK(IMP_AI_DisableChn(0, 0) == 0, "AI DisableChn");
    CHECK(IMP_AI_Disable(0) == 0, "AI Disable");
    CHECK(open_count == 0, "descriptors left open: %d", open_count);
    CHECK(drop_calls >= 3, "streams dropped (%d)", drop_calls);

    if (failures) {
        fprintf(stderr, "alsa_pcm_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("alsa_pcm_test: ok");
    return 0;
}
