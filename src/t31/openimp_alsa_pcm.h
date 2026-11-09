/* Minimal raw ALSA PCM backend for the T31 audio code (issue #258).
 *
 * Mainline kernels (thingino kernel 7) have no /dev/dsp: audio is an ASoC
 * card that only exposes /dev/snd/pcmC<card>D<dev>{c,p}.  This header drives
 * those nodes with the PCM ioctls of <sound/asound.h> directly, so no
 * alsa-lib is needed.  It is header-only (static functions) so every SoC
 * build that compiles openimp_t31_audio.c gets it without a build-script
 * change.
 *
 * Only the read/write-ioctl transfer path is used (SNDRV_PCM_IOCTL_READI_FRAMES /
 * WRITEI_FRAMES): none of the ioctls used here carries a struct timespec, so
 * the 64-bit time_t of the kernel-7 uClibc cannot change an ioctl size, and
 * the mmap status/control page (which does) is never mapped.
 *
 * Stream setup: interleaved S16_LE, the requested rate and channel count,
 * 20 ms periods and 8 periods of buffer.  Capture starts by itself with the
 * first read (start_threshold 1) so a stream that is enabled long before the
 * capture thread runs does not overrun.  Playback starts when one period has
 * been written.  Overruns/underruns (-EPIPE) are recovered by PREPARE. */
#ifndef OPENIMP_ALSA_PCM_H
#define OPENIMP_ALSA_PCM_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<sound/asound.h>)
#define OPENIMP_ALSA_AVAILABLE 1
#include <sound/asound.h>
#endif
#endif

#define OPENIMP_ALSA_PERIOD_DIV 50U   /* 20 ms periods */
#define OPENIMP_ALSA_PERIODS    8U

#if defined(OPENIMP_ALSA_AVAILABLE)

static __attribute__((unused)) void openimp_alsa_mask_set(struct snd_mask *mask, unsigned int bit)
{
    memset(mask, 0, sizeof(*mask));
    mask->bits[bit >> 5] = 1U << (bit & 31U);
}

static __attribute__((unused)) void openimp_alsa_interval_set(struct snd_interval *interval,
                                      unsigned int min, unsigned int max)
{
    memset(interval, 0, sizeof(*interval));
    interval->min = min;
    interval->max = max;
    interval->integer = 1;
}

/* An "any" hw_params, like snd_pcm_hw_params_any(): every mask and interval
 * wide open, everything marked for refinement. */
static __attribute__((unused)) void openimp_alsa_params_any(struct snd_pcm_hw_params *params)
{
    unsigned int index;

    memset(params, 0, sizeof(*params));
    for (index = 0; index <= SNDRV_PCM_HW_PARAM_LAST_MASK -
                                 SNDRV_PCM_HW_PARAM_FIRST_MASK; index++)
        memset(params->masks[index].bits, 0xff,
               sizeof(params->masks[index].bits));
    for (index = 0; index <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL -
                                 SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; index++) {
        params->intervals[index].min = 0;
        params->intervals[index].max = UINT_MAX;
    }
    params->rmask = ~0U;
    params->info = ~0U;
}

static __attribute__((unused)) struct snd_mask *openimp_alsa_mask(struct snd_pcm_hw_params *params,
                                          int which)
{
    return &params->masks[which - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static __attribute__((unused)) struct snd_interval *
openimp_alsa_interval(struct snd_pcm_hw_params *params, int which)
{
    return &params->intervals[which - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Fill the hw_params for the S16_LE stream; period_frames/periods 0 lets the
 * driver choose. */
static __attribute__((unused)) void openimp_alsa_fill_params(struct snd_pcm_hw_params *params,
                                     unsigned int rate, unsigned int channels,
                                     unsigned int period_frames,
                                     unsigned int periods)
{
    openimp_alsa_params_any(params);
    openimp_alsa_mask_set(openimp_alsa_mask(params, SNDRV_PCM_HW_PARAM_ACCESS),
                          SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    openimp_alsa_mask_set(openimp_alsa_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
                          SNDRV_PCM_FORMAT_S16_LE);
    openimp_alsa_mask_set(
        openimp_alsa_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT),
        SNDRV_PCM_SUBFORMAT_STD);
    openimp_alsa_interval_set(
        openimp_alsa_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS), channels,
        channels);
    openimp_alsa_interval_set(
        openimp_alsa_interval(params, SNDRV_PCM_HW_PARAM_RATE), rate, rate);
    if (period_frames && periods) {
        openimp_alsa_interval_set(
            openimp_alsa_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
            period_frames, period_frames);
        openimp_alsa_interval_set(
            openimp_alsa_interval(params, SNDRV_PCM_HW_PARAM_PERIODS), periods,
            periods);
    }
}

/* Open /dev/snd/pcmC<card>D0{c,p} (card from OPENIMP_ALSA_CARD, default 0)
 * and configure it.  Returns the descriptor, or -1 with errno set. */
static __attribute__((unused)) int openimp_alsa_open(int capture, unsigned int rate,
                             unsigned int channels, unsigned int *wire_channels)
{
    struct snd_pcm_hw_params params;
    struct snd_pcm_sw_params sw;
    char path[64];
    const char *card = getenv("OPENIMP_ALSA_CARD");
    unsigned int period_frames = rate / OPENIMP_ALSA_PERIOD_DIV;
    unsigned long boundary;
    unsigned long buffer_frames;
    unsigned int wire;
    int fd;
    int saved;

    if (!rate || (channels != 1 && channels != 2)) {
        errno = EINVAL;
        return -1;
    }
    snprintf(path, sizeof(path), "/dev/snd/pcmC%uD0%c",
             card ? (unsigned int)strtoul(card, NULL, 10) : 0U,
             capture ? 'c' : 'p');
    fd = open(path, (capture ? O_RDONLY : O_WRONLY) | O_CLOEXEC);
    if (fd < 0)
        return -1;
    /* the t31-aic I2S controller only does stereo (hw_refine on kernel
     * 7.3.3: channels 2..2), so a mono stream runs as stereo on the wire and
     * openimp_alsa_xfer converts */
    wire = channels;
    openimp_alsa_fill_params(&params, rate, wire, period_frames,
                             OPENIMP_ALSA_PERIODS);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) != 0 && channels == 1) {
        wire = 2;
        openimp_alsa_fill_params(&params, rate, wire, period_frames,
                                 OPENIMP_ALSA_PERIODS);
    }
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) != 0) {
        /* the sizes are only a wish: retry with the driver's own */
        openimp_alsa_fill_params(&params, rate, wire, 0, 0);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) != 0)
            goto fail;
    }
    if (wire_channels)
        *wire_channels = wire;
    period_frames = openimp_alsa_interval(&params,
                                          SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    buffer_frames = openimp_alsa_interval(&params,
                                          SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    if (!period_frames || !buffer_frames) {
        errno = EINVAL;
        goto fail;
    }
    /* boundary: the largest multiple of the buffer that fits a long, as
     * alsa-lib computes it */
    boundary = buffer_frames;
    while (boundary * 2 <= (unsigned long)LONG_MAX - buffer_frames)
        boundary *= 2;
    memset(&sw, 0, sizeof(sw));
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period_frames;
    sw.xfer_align = 1;
    sw.start_threshold = capture ? 1 : period_frames;
    sw.stop_threshold = boundary;
    sw.boundary = boundary;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) != 0 ||
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) != 0)
        goto fail;
    return fd;
fail:
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

static __attribute__((unused)) int openimp_alsa_prepare(int fd)
{
    return ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
}

/* Stop at once and discard queued audio; wakes a blocked transfer.  The
 * stream must be prepared again before the next transfer. */
static __attribute__((unused)) int openimp_alsa_drop(int fd)
{
    return ioctl(fd, SNDRV_PCM_IOCTL_DROP);
}

/* Play out what is queued (blocks), then return to the prepared state. */
static __attribute__((unused)) int openimp_alsa_drain(int fd)
{
    int result = ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);

    if (result != 0 && errno == EPIPE)
        result = 0;
    (void)openimp_alsa_prepare(fd);
    return result;
}

/* One transfer of whole frames straight on the wire format. */
static __attribute__((unused)) long openimp_alsa_xfer_raw(int fd, int capture,
                                  void *buffer, size_t bytes,
                                  size_t frame_bytes)
{
    unsigned char *cursor = buffer;
    size_t done = 0;
    int recoveries = 0;

    if (!frame_bytes)
        return -1;
    bytes -= bytes % frame_bytes;
    while (done < bytes) {
        struct snd_xferi transfer;

        memset(&transfer, 0, sizeof(transfer));
        transfer.buf = cursor + done;
        transfer.frames = (bytes - done) / frame_bytes;
        if (ioctl(fd, capture ? SNDRV_PCM_IOCTL_READI_FRAMES
                              : SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                  &transfer) != 0) {
            if (errno == EINTR)
                continue;
            if ((errno == EPIPE || errno == ESTRPIPE) && recoveries++ < 4 &&
                openimp_alsa_prepare(fd) == 0)
                continue;
            return done ? (long)done : -1;
        }
        if (transfer.result > 0)
            done += (size_t)transfer.result * frame_bytes;
        else if (transfer.result < 0)
            return done ? (long)done : -1;
    }
    return (long)done;
}

#define OPENIMP_ALSA_CONV_FRAMES 256U

/* Blocking read (capture) or write (playback) of whole frames of
 * `channels` S16 channels while the stream runs with `wire` channels.
 * Returns the bytes moved in the caller's format (a whole number of frames,
 * the full request unless an error cut it short) or -1 with nothing moved.
 * An over/underrun is recovered with PREPARE and the transfer goes on.
 * Mono on a stereo wire: capture keeps the average of both channels
 * (`capture_channel` 2) or one of them (0 left, 1 right); playback sends the
 * sample on both channels. */
static __attribute__((unused)) long openimp_alsa_xfer(int fd, int capture,
                              void *buffer, size_t bytes,
                              unsigned int channels, unsigned int wire,
                              int capture_channel)
{
    size_t frame = (size_t)channels * sizeof(int16_t);
    int16_t *user = buffer;
    int16_t wirebuf[OPENIMP_ALSA_CONV_FRAMES * 2];
    size_t done = 0;

    if (channels == wire)
        return openimp_alsa_xfer_raw(fd, capture, buffer, bytes, frame);
    if (channels != 1 || wire != 2)
        return -1;
    bytes -= bytes % frame;
    while (done < bytes) {
        size_t count = (bytes - done) / frame;
        size_t index;
        long moved;

        if (count > OPENIMP_ALSA_CONV_FRAMES)
            count = OPENIMP_ALSA_CONV_FRAMES;
        if (!capture)
            for (index = 0; index < count; index++)
                wirebuf[2 * index] = wirebuf[2 * index + 1] =
                    user[done / frame + index];
        moved = openimp_alsa_xfer_raw(fd, capture, wirebuf, count * 2 * frame,
                                      2 * frame);
        if (moved <= 0)
            return done ? (long)done : -1;
        count = (size_t)moved / (2 * frame);
        if (capture)
            for (index = 0; index < count; index++) {
                int left = wirebuf[2 * index];
                int right = wirebuf[2 * index + 1];

                user[done / frame + index] = (int16_t)(
                    capture_channel == 0 ? left
                    : capture_channel == 1 ? right : (left + right) / 2);
            }
        done += count * frame;
    }
    return (long)done;
}

#else /* no <sound/asound.h> in this toolchain: the backend is absent */

static __attribute__((unused)) int openimp_alsa_open(int capture, unsigned int rate,
                             unsigned int channels, unsigned int *wire_channels)
{
    (void)capture;
    (void)rate;
    (void)channels;
    (void)wire_channels;
    errno = ENODEV;
    return -1;
}
static __attribute__((unused)) int openimp_alsa_prepare(int fd) { (void)fd; return -1; }
static __attribute__((unused)) int openimp_alsa_drop(int fd) { (void)fd; return -1; }
static __attribute__((unused)) int openimp_alsa_drain(int fd) { (void)fd; return -1; }
static __attribute__((unused)) long openimp_alsa_xfer(int fd, int capture,
                              void *buffer, size_t bytes,
                              unsigned int channels, unsigned int wire,
                              int capture_channel)
{
    (void)fd; (void)capture; (void)buffer; (void)bytes; (void)channels;
    (void)wire; (void)capture_channel;
    return -1;
}

#endif

#endif
