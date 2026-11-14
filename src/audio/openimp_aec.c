/* OpenIMP AEC: WebRTC AECM built as one translation unit.
 *
 * The files under src/audio/webrtc are copies from
 * webrtc-audio-processing v0.3.1 (freedesktop.org, commit e882a544),
 * BSD-3-Clause, see src/audio/webrtc/LICENSE, PATENTS and
 * LICENSE_THIRD_PARTY.  The one change, marked "OpenIMP:", is in
 * WebRtcAecm_Create (calloc instead of an unchecked malloc whose error path
 * freed uninitialised pointers).  Only the AECM core and the signal-processing,
 * delay-estimator and ring-buffer helpers it needs are included, all as
 * portable C (no MIPS32_LE/DSP assembly). */

#ifndef NDEBUG
#define NDEBUG
#endif
#ifndef WEBRTC_POSIX
#define WEBRTC_POSIX /* pthread_once in spl_init.c */
#endif

#include "webrtc/common_audio/ring_buffer.c"
#include "webrtc/common_audio/signal_processing/complex_bit_reverse.c"
#include "webrtc/common_audio/signal_processing/complex_fft.c"
#include "webrtc/common_audio/signal_processing/cross_correlation.c"
#include "webrtc/common_audio/signal_processing/division_operations.c"
#include "webrtc/common_audio/signal_processing/downsample_fast.c"
#include "webrtc/common_audio/signal_processing/min_max_operations.c"
#include "webrtc/common_audio/signal_processing/randomization_functions.c"
#include "webrtc/common_audio/signal_processing/real_fft.c"
#include "webrtc/common_audio/signal_processing/spl_init.c"
#include "webrtc/common_audio/signal_processing/spl_sqrt_floor.c"
#include "webrtc/common_audio/signal_processing/vector_scaling_operations.c"
#include "webrtc/modules/audio_processing/utility/delay_estimator.c"
#include "webrtc/modules/audio_processing/utility/delay_estimator_wrapper.c"
#include "webrtc/modules/audio_processing/aecm/aecm_core.c"
#include "webrtc/modules/audio_processing/aecm/aecm_core_c.c"
#include "webrtc/modules/audio_processing/aecm/echo_control_mobile.c"

#include <math.h>

#include "openimp_aec.h"

/* echoMode 0..4 is AECM's suppression strength; 3 is WebRTC's default,
 * 4 the most aggressive.  Comfort noise is off: the AI path has its own
 * noise suppression and a camera microphone should stay silent rather
 * than hiss during double talk. */
#ifndef OPENIMP_AEC_ECHO_MODE
#define OPENIMP_AEC_ECHO_MODE 3
#endif

/* Echo gate (issue #499).  AECM estimates the echo from spectral magnitudes
 * only, so with a far end playing and no echo at the microphone its channel
 * converges on |near| / |far|, the echo estimate equals the near end and
 * WebRtcAecm_CalcSuppressionGain raises the suppression gain to 4..6 x
 * ("no double talk"): the room sound is removed (T30X lab: 15 dB; the
 * vendor's full WebRTC AEC, whose suppressor works on coherence: 0.2 dB).
 * AECM's binary delay estimator knows better: its smoothed bit-count curve
 * over the candidate delays (32 bits per spectrum, Q9) has a valley only
 * when the microphone spectrum follows the far end.  The valley depth used
 * here is mean - best over the candidates: <= 3.1 without echo (noise or a
 * talker on either side), 5..10 with a clear echo, 2..5 with an echo 6 dB
 * under the room noise.  AECM's suppression gain parameters are scaled with
 * it: none up to OPENIMP_AEC_GATE_LOW, full from OPENIMP_AEC_GATE_HIGH.
 * The estimator needs 1..2 s of far end to form the valley, so the gate is
 * open (AECM as it is) for the first OPENIMP_AEC_GATE_WARMUP_BLOCKS
 * far-end-active blocks; after that it opens at once and closes over
 * OPENIMP_AEC_GATE_RELEASE_BLOCKS far-end-active blocks, so double talk
 * does not close it.  Blocks are 10 ms.
 *
 * The estimator only looks at 1.5-5.4 kHz (16 kHz; 0.75-2.7 kHz at 8 kHz),
 * so the echo of a low tone forms no valley.  Second evidence, full band:
 * the correlation of the microphone level with the far-end level (log2 of
 * the block energy, far end delayed by 0..30 ms, running statistics over
 * OPENIMP_AEC_ENV_BLOCKS far-end blocks).  Without echo it stays under 0.5
 * after the warm-up, with a clear echo it is 0.8..1.0 (0.8 in double talk);
 * the gate opens from OPENIMP_AEC_ENV_LOW to OPENIMP_AEC_ENV_HIGH.
 *
 * Known limit: an echo more than about 10 dB under the room noise forms no
 * evidence and is no longer suppressed (before, it was removed together
 * with the room noise). */
#ifndef OPENIMP_AEC_ENV_BLOCKS
#define OPENIMP_AEC_ENV_BLOCKS 384
#endif
#ifndef OPENIMP_AEC_ENV_LOW
#define OPENIMP_AEC_ENV_LOW 0.6f
#endif
#ifndef OPENIMP_AEC_ENV_HIGH
#define OPENIMP_AEC_ENV_HIGH 0.8f
#endif
#ifndef OPENIMP_AEC_GATE_LOW
#define OPENIMP_AEC_GATE_LOW (3 << 9)
#endif
#ifndef OPENIMP_AEC_GATE_HIGH
#define OPENIMP_AEC_GATE_HIGH (5 << 9)
#endif
#ifndef OPENIMP_AEC_GATE_RELEASE_BLOCKS
#define OPENIMP_AEC_GATE_RELEASE_BLOCKS 600
#endif
#ifndef OPENIMP_AEC_GATE_WARMUP_BLOCKS
#define OPENIMP_AEC_GATE_WARMUP_BLOCKS 300
#endif
#define OPENIMP_AEC_GATE_ONE (1 << 16)
#define OPENIMP_AEC_ENV_LAGS 4

struct OpenimpAec {
    void *aecm;
    int32_t gate;               /* 0..OPENIMP_AEC_GATE_ONE */
    int32_t gate_warmup;        /* far-end-active blocks left */
    struct {
        float x[OPENIMP_AEC_ENV_LAGS]; /* far level history, log2 */
        float mean_x[OPENIMP_AEC_ENV_LAGS], var_x[OPENIMP_AEC_ENV_LAGS];
        float cov[OPENIMP_AEC_ENV_LAGS];
        float mean_y, var_y;
        float rho;              /* best correlation over the lags */
        int started;
    } env;
    int16_t sup_gain[4];        /* echoMode values: A, D, A-B, B-D */
    size_t block;
    int16_t *out;               /* processed frame, committed when whole */
    size_t out_capacity;        /* samples */
    int16_t reference_delay_ms; /* msInSndCardBuf */
    unsigned long failed_blocks;
};

static void openimp_aec_gate_update(OpenimpAec *aec);

OpenimpAec *openimp_aec_create(int sample_rate)
{
    OpenimpAec *aec;
    AecmConfig config;

    if (sample_rate != 8000 && sample_rate != 16000)
        return NULL;
    aec = calloc(1, sizeof(*aec));
    if (!aec)
        return NULL;
    aec->block = (size_t)sample_rate / 100U;
    aec->aecm = WebRtcAecm_Create();
    config.cngMode = AecmFalse;
    config.echoMode = OPENIMP_AEC_ECHO_MODE;
    if (!aec->aecm || WebRtcAecm_Init(aec->aecm, sample_rate) != 0 ||
        WebRtcAecm_set_config(aec->aecm, config) != 0) {
        openimp_aec_free(aec);
        return NULL;
    }
    {
        const AecmCore *core = ((const AecMobile *)aec->aecm)->aecmCore;

        aec->sup_gain[0] = core->supGainErrParamA;
        aec->sup_gain[1] = core->supGainErrParamD;
        aec->sup_gain[2] = core->supGainErrParamDiffAB;
        aec->sup_gain[3] = core->supGainErrParamDiffBD;
    }
    aec->gate_warmup = OPENIMP_AEC_GATE_WARMUP_BLOCKS;
    openimp_aec_gate_update(aec);
    return aec;
}

/* Sets AECM's suppression gain parameters from the delay estimator's echo
 * evidence; takes effect with the next block. */
static void openimp_aec_gate_update(OpenimpAec *aec)
{
    AecmCore *core = ((AecMobile *)aec->aecm)->aecmCore;
    const BinaryDelayEstimator *binary =
        ((const DelayEstimator *)core->delay_estimator)->binary_handle;
    int32_t best = INT32_MAX, depth, target, gate;
    int64_t sum = 0;
    int i;

    if (binary->history_size <= 0)
        return;
    for (i = 0; i < binary->history_size; i++) {
        int32_t count = binary->mean_bit_counts[i];

        if (count < best)
            best = count;
        sum += count;
    }
    depth = (int32_t)(sum / binary->history_size) - best;
    if (depth <= OPENIMP_AEC_GATE_LOW)
        target = 0;
    else if (depth >= OPENIMP_AEC_GATE_HIGH)
        target = OPENIMP_AEC_GATE_ONE;
    else
        target = (int32_t)((int64_t)(depth - OPENIMP_AEC_GATE_LOW) *
                           OPENIMP_AEC_GATE_ONE /
                           (OPENIMP_AEC_GATE_HIGH - OPENIMP_AEC_GATE_LOW));
    if (aec->env.rho >= OPENIMP_AEC_ENV_HIGH) {
        target = OPENIMP_AEC_GATE_ONE;
    } else if (aec->env.rho > OPENIMP_AEC_ENV_LOW) {
        int32_t env = (int32_t)((aec->env.rho - OPENIMP_AEC_ENV_LOW) /
                                (OPENIMP_AEC_ENV_HIGH - OPENIMP_AEC_ENV_LOW) *
                                OPENIMP_AEC_GATE_ONE);

        if (env > target)
            target = env;
    }
    gate = aec->gate;
    if (aec->gate_warmup > 0) {
        if (core->currentVADValue)
            aec->gate_warmup--;
        gate = aec->gate_warmup > 0 ? OPENIMP_AEC_GATE_ONE : target;
    } else if (target >= gate) {
        gate = target;
    } else if (core->currentVADValue) {
        /* the evidence only moves while the far end is active */
        gate -= OPENIMP_AEC_GATE_ONE / OPENIMP_AEC_GATE_RELEASE_BLOCKS;
        if (gate < target)
            gate = target;
    }
    aec->gate = gate;
    core->supGainErrParamA = (int16_t)(((int64_t)aec->sup_gain[0] * gate) >> 16);
    core->supGainErrParamD = (int16_t)(((int64_t)aec->sup_gain[1] * gate) >> 16);
    core->supGainErrParamDiffAB =
        (int16_t)(((int64_t)aec->sup_gain[2] * gate) >> 16);
    core->supGainErrParamDiffBD =
        (int16_t)(((int64_t)aec->sup_gain[3] * gate) >> 16);
}

static float openimp_aec_level(const int16_t *x, size_t n)
{
    int64_t e = 0;
    size_t i;

    for (i = 0; i < n; i++)
        e += (int32_t)x[i] * x[i];
    return log2f((float)e / (float)n + 1.0f);
}

static void openimp_aec_env_update(OpenimpAec *aec, const int16_t *far_end,
                                   const int16_t *near_end)
{
    const float a = 1.0f / OPENIMP_AEC_ENV_BLOCKS;
    float x = openimp_aec_level(far_end, aec->block);
    float y = openimp_aec_level(near_end, aec->block), dy, rho = 0;
    int k;

    /* only blocks with far end (above -60 dBFS): its on/off steps would
     * outweigh everything else */
    if (x < 10.0f)
        return;
    memmove(aec->env.x + 1, aec->env.x,
            sizeof(aec->env.x) - sizeof(aec->env.x[0]));
    aec->env.x[0] = x;
    if (!aec->env.started) {
        aec->env.started = 1;
        aec->env.mean_y = y;
        for (k = 0; k < OPENIMP_AEC_ENV_LAGS; k++)
            aec->env.mean_x[k] = aec->env.x[k] = x;
    }
    dy = y - aec->env.mean_y;
    aec->env.mean_y += a * dy;
    aec->env.var_y += a * (dy * (y - aec->env.mean_y) - aec->env.var_y);
    for (k = 0; k < OPENIMP_AEC_ENV_LAGS; k++) {
        float dx = aec->env.x[k] - aec->env.mean_x[k], r;

        aec->env.mean_x[k] += a * dx;
        aec->env.var_x[k] += a * (dx * (aec->env.x[k] - aec->env.mean_x[k]) -
                                  aec->env.var_x[k]);
        aec->env.cov[k] += a * (dx * (y - aec->env.mean_y) - aec->env.cov[k]);
        if (aec->env.var_x[k] < 1.0f || aec->env.var_y <= 0)
            continue;
        r = aec->env.cov[k] / sqrtf(aec->env.var_x[k] * aec->env.var_y + 1e-6f);
        if (r > rho)
            rho = r;
    }
    aec->env.rho = rho;
}

int openimp_aec_echo_gate_percent(const OpenimpAec *aec)
{
    return aec ? (int)(((int64_t)aec->gate * 100) >> 16) : 0;
}

size_t openimp_aec_block_samples(const OpenimpAec *aec)
{
    return aec ? aec->block : 0;
}

int openimp_aec_process(OpenimpAec *aec, const int16_t *far_end,
                        int16_t *near_end, size_t samples)
{
    size_t offset;
    unsigned long failed = 0;

    if (!aec || !far_end || !near_end || samples % aec->block)
        return -1;
    if (samples > aec->out_capacity) {
        int16_t *out = realloc(aec->out, samples * sizeof(*out));

        if (!out)
            return -1;
        aec->out = out;
        aec->out_capacity = samples;
    }
    for (offset = 0; offset < samples; offset += aec->block) {
        openimp_aec_env_update(aec, far_end + offset, near_end + offset);
        /* The reference is sample-aligned with the microphone (the codec
         * records both); reference_delay_ms is what the driver reports
         * beyond that (0 on T31).  Every block is fed, even after a
         * failure, so the far-end buffer stays in step. */
        if (WebRtcAecm_BufferFarend(aec->aecm, far_end + offset,
                                    aec->block) != 0 ||
            WebRtcAecm_Process(aec->aecm, near_end + offset, NULL,
                               aec->out + offset, aec->block,
                               aec->reference_delay_ms) != 0)
            failed++;
        openimp_aec_gate_update(aec);
    }
    if (failed) {
        /* leave the microphone frame as captured, not half-processed */
        aec->failed_blocks += failed;
        return -1;
    }
    memcpy(near_end, aec->out, samples * sizeof(*near_end));
    return 0;
}

void openimp_aec_set_reference_delay_ms(OpenimpAec *aec, int delay_ms)
{
    if (!aec)
        return;
    /* AECM clamps msInSndCardBuf to 0..500 itself */
    if (delay_ms < 0)
        delay_ms = 0;
    if (delay_ms > 500)
        delay_ms = 500;
    aec->reference_delay_ms = (int16_t)delay_ms;
}

unsigned long openimp_aec_failed_blocks(const OpenimpAec *aec)
{
    return aec ? aec->failed_blocks : 0ul;
}

int openimp_aec_delay_ms(const OpenimpAec *aec)
{
    const AecMobile *mobile;

    if (!aec || !aec->aecm)
        return -1;
    int delay;

    mobile = (const AecMobile *)aec->aecm;
    /* PART_LEN-sample partitions; -2 while the estimator is not sure */
    delay = WebRtc_last_delay(mobile->aecmCore->delay_estimator);
    if (delay < 0)
        return -1;
    return delay * PART_LEN * 1000 / mobile->sampFreq;
}

void openimp_aec_free(OpenimpAec *aec)
{
    if (!aec)
        return;
    if (aec->aecm)
        WebRtcAecm_Free(aec->aecm);
    free(aec->out);
    free(aec);
}
