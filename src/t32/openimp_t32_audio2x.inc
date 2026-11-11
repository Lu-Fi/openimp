/* T32 2.x audio: vendor libimp 2.x semantics on the OSS3 /dev/dsp paths (#329).
 *
 * Included at the end of src/t40/openimp_p3_audio.c for PLATFORM_T32 only
 * (shares its static state).  Behaviour taken from the vendor T32 2.0.0
 * libimp (ai.c.o / ao.c.o / audio_common.c.o, read with Ghidra; no code
 * copied) and checked against vendor libimp 2.2.0 in QEMU -M ingenic-t32:
 *
 * - AI/AO volume is software: factor 10^((vol * 0.5 - 30) / 20), vol
 *   clamped to -30..120 (-30 = 0), applied to every sample as the vendor
 *   _audio_set_volume does (double product truncated, then clipped to
 *   -32767..32767; also at unity).  AI: after the record chain; AO: on each
 *   queued block.  SetVol/GetVol/SetVolMute touch no ioctl; EnableChn resets
 *   the volume to 60 (factor 1).  SetGain clamps 0..31 and is the only one
 *   that programs the codec (AI 0x400c505a {chnCnt, g, g}, AO 0x400c5058
 *   {1, g, g}); a failing ioctl is only logged.  Get* answer the stored value.
 * - AI/AO SetPubAttr only store; IMP_AI_Enable / IMP_AO_Enable send the
 *   parameters (SET_PARAM) and enable the stream.  The AI record thread runs
 *   from IMP_AI_EnableChn (usrFrmDepth deep, 2..frmNum, else -1).
 * - AO: SendFrame collects frames into blocks of numPerFrm samples (raised to
 *   40 ms when numPerFrm is shorter and a 10 ms multiple, as the vendor), runs
 *   the AO effects and the volume on each full block and queues it (frmNum
 *   blocks; BLOCK waits for a free one, NOBLOCK returns -1).  A play thread
 *   writes the queue to the driver.  With the cache on (vendor default) it
 *   starts after an underrun only when busy >= free (half the queue).
 *   QueryChnStat reports {frmNum, free, busy}.  FlushChnBuf pads the open
 *   block with silence, queues it, waits until the queue is empty (5 s) and
 *   syncs the driver; ClearChnBuf drops the queue.  Pause stops the thread
 *   after the block in flight.
 * - Record chain (IMP_AI_SetAlgoOrder, default NS, AGC, HPF), then DRC/EQ,
 *   LPF, howling suppression; AO chain (IMP_AO_SetAlgoOrder, default HPF,
 *   AGC, NS), then DRC/EQ.  Both only with 16-bit samples and blocks of a
 *   10 ms multiple.  The algorithms are libaudioProcess entry points.
 *
 * Differences (OpenIMP better or simpler): the caller's IMPAudioIOAttr is
 * not rewritten (the vendor writes the 40 ms block size back); after Flush
 * and Clear the open block starts empty (the vendor keeps its fill pointer
 * and replays the padded tail); LPF/DRC/EQ apply to the one OpenIMP AI device
 * (the vendor flags only work while AI device 1 is enabled); OPENIMP_AO_CACHE=0
 * starts with the AO cache off.
 */

#include <math.h>
#include <sys/time.h>

/* ---- shared ------------------------------------------------------------ */

static double t32_vol_factor(int *volume)
{
    if (*volume < -30) {
        *volume = -30;
        return 0.0;
    }
    if (*volume > 120) {
        *volume = 120;
        return pow(10.0, 1.5);
    }
    if (*volume == -30)
        return 0.0;
    return pow(10.0, ((double)*volume * 0.5 - 30.0) / 20.0);
}

/* vendor _audio_set_volume for 16-bit samples */
static void t32_apply_volume(const int16_t *in, int16_t *out, size_t count,
                             double factor)
{
    size_t i;

    for (i = 0; i < count; i++) {
        int value = (int)((double)in[i] * factor);

        if (value >= 0x8000)
            value = 0x7fff;
        else if (value < -0x7fff)
            value = -0x7fff;
        out[i] = (int16_t)value;
    }
}

static int t32_ten_ms(int samples, int rate)
{
    return rate > 0 && ((unsigned)samples * 1000U / (unsigned)rate) % 10U == 0;
}

static void *t32_effect_symbol(const char *name)
{
    if (p3_effects_load() != 0)
        return NULL;
    return dlsym(p3_audio.effects_library, name);
}

static int t32_deadline(struct timespec *deadline, int ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += ms / 1000;
    deadline->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

/* libaudioProcess NS on frames of 80 (8 kHz) / 160 samples, the float result
 * truncated to 16 bits like the vendor */
static void t32_ns_run(void *handle, int16_t *samples, int count, int frame)
{
    float input[160];
    float output[160];
    const float *inputs[1] = { input };
    float *outputs[1] = { output };
    int offset, i;

    if (!handle || frame <= 0 || frame > 160)
        return;
    for (offset = 0; offset + frame <= count; offset += frame) {
        for (i = 0; i < frame; i++)
            input[i] = (float)samples[offset + i];
        memset(output, 0, sizeof(output));
        p3_audio.ns_process(handle, inputs, 1, outputs);
        for (i = 0; i < frame; i++)
            samples[offset + i] = (int16_t)(int)output[i];
    }
}

static void t32_agc_run(void *handle, int16_t *samples, int count, int frame)
{
    int offset;

    if (!handle || frame <= 0)
        return;
    for (offset = 0; offset + frame <= count; offset += frame) {
        const int16_t *inputs[1] = { samples + offset };
        int16_t *outputs[1] = { samples + offset };
        int32_t level = 0;
        uint8_t saturated = 0;

        (void)p3_audio.agc_process(handle, inputs, 1, (size_t)frame, outputs,
                                   127, &level, 0, &saturated);
    }
}

static int t32_algo_frame(int rate)
{
    return rate == 8000 ? 80 : 160;
}

/* DRC/EQ (libaudioProcess audio_process_drc_eq_*), 160-sample frames */
typedef void *(*T32DrcEqCreate)(int);
typedef int (*T32DrcEqSetConfig)(void *, int, int, int, int, int, int, int,
                                 int);
typedef int (*T32DrcEqEnable)(void *, int, int);
typedef int (*T32DrcEqProcess)(void *, float **, float **);
typedef int (*T32DrcEqFree)(void *);

typedef struct {
    void *handle;
    float *in;
    float *out;
    int frame;
    int enabled;
    T32DrcEqProcess process;
    T32DrcEqFree release;
} T32DrcEq;

static void t32_drc_eq_free(T32DrcEq *drc)
{
    if (drc->handle && drc->release)
        drc->release(drc->handle);
    free(drc->in);
    free(drc->out);
    memset(drc, 0, sizeof(*drc));
}

/* vendor IMP_A[IO]_EnableDrcAndEq: 16 bit, 10 ms blocks; 8/16 kHz only (else
 * 0 without enabling); the config words go to set_config as the vendor
 * passes them (threshold_low, threshold_high, ratio, eq_type, freq_low,
 * freq_high, EqTable, size) */
static int t32_drc_eq_enable(T32DrcEq *drc, const IMPAudioIOAttr *attr,
                             const IMPAudioDrcEqConfig *config, int drc_on,
                             int eq_on, int device_on, const char *tag)
{
    T32DrcEqCreate create;
    T32DrcEqSetConfig set_config;
    T32DrcEqEnable enable;
    int32_t words[8];

    if (!attr || !config || attr->bitwidth != AUDIO_BIT_WIDTH_16 ||
        !t32_ten_ms(attr->numPerFrm, attr->samplerate))
        return -1;
    if (drc->handle)
        t32_drc_eq_free(drc);
    create = (T32DrcEqCreate)t32_effect_symbol("audio_process_drc_eq_create");
    set_config = (T32DrcEqSetConfig)t32_effect_symbol(
        "audio_process_drc_eq_set_config");
    enable = (T32DrcEqEnable)t32_effect_symbol("audio_process_drc_eq_enable");
    drc->process = (T32DrcEqProcess)t32_effect_symbol(
        "audio_process_drc_eq_process");
    drc->release = (T32DrcEqFree)t32_effect_symbol("audio_process_drc_eq_free");
    if (!create || !set_config || !enable || !drc->process || !drc->release)
        return -1;
    drc->handle = create(attr->samplerate);
    if (!drc->handle)
        return -1;
    if (attr->samplerate != 8000 && attr->samplerate != 16000) {
        IMP_LOG_LIMITED(LOG_ERR, tag, "DRC/EQ: samplerate %d not supported",
                        attr->samplerate);
        return 0;
    }
    drc->frame = 160;
    memcpy(words, config, sizeof(words) < sizeof(*config) ? sizeof(words)
                                                          : sizeof(*config));
    if (set_config(drc->handle, words[0], words[1], words[2], words[3],
                   words[4], words[5], words[7], words[6]) != 0)
        return -1;
    drc->in = calloc((size_t)drc->frame, sizeof(float));
    drc->out = calloc((size_t)drc->frame, sizeof(float));
    if (!drc->in || !drc->out || enable(drc->handle, drc_on, eq_on) != 0) {
        t32_drc_eq_free(drc);
        return -1;
    }
    if (device_on)
        drc->enabled = 1;
    else
        IMP_LOG_LIMITED(LOG_ERR, tag, "DRC/EQ: device is not enabled");
    return 0;
}

static void t32_drc_eq_run(T32DrcEq *drc, int16_t *samples, int count)
{
    int offset, i;

    if (!drc->enabled || !drc->handle || drc->frame <= 0)
        return;
    for (offset = 0; offset + drc->frame <= count; offset += drc->frame) {
        for (i = 0; i < drc->frame; i++)
            drc->in[i] = (float)samples[offset + i];
        memset(drc->out, 0, (size_t)drc->frame * sizeof(float));
        drc->process(drc->handle, &drc->in, &drc->out);
        for (i = 0; i < drc->frame; i++)
            samples[offset + i] = (int16_t)(int)drc->out[i];
    }
}

/* unused on T32 (its record chain is t32_ai_effects) */
static void (*const t32_p3_effects_unused)(int16_t *, int)
    __attribute__((unused)) = p3_process_effects;

/* ---- AI ---------------------------------------------------------------- */

typedef void (*T32LpfCreate)(void *, int, int);
typedef void (*T32LpfProcess)(void *, int16_t *, int);

static struct {
    double vol_live;            /* applied (0 while muted) */
    double vol_saved;           /* from SetVol */
    int vol;
    int gain;
    int mute;
    int order[3];
    int order_set;
    int lpf_enabled;
    int lpf_cutoff;
    int16_t lpf_state[8];
    T32LpfProcess lpf_process;
    T32DrcEq drc;
} t32_ai;

static int t32_ai_args(int device, int channel)
{
    return device >= 0 && device < 2 && channel == 0;
}

int IMP_AI_SetPubAttr(int device, IMPAudioIOAttr *attribute)
{
    if (device < 0 || device >= 2 || !attribute || attribute->chnCnt >= 3)
        return -1;
    if (attribute->soundmode == AUDIO_SOUND_MODE_STEREO ? attribute->chnCnt != 2
                                                        : attribute->chnCnt != 1)
        return -1;
    if (attribute->samplerate > 0 &&
        !t32_ten_ms(attribute->numPerFrm, attribute->samplerate))
        IMP_LOG_LIMITED(LOG_WARNING, "AI",
                        "numPerFrm should be a multiple of 10 ms");
    p3_audio.ai_attr = *attribute;
    return 0;
}

int IMP_AI_GetPubAttr(int device, IMPAudioIOAttr *attribute)
{
    (void)device;
    if (!attribute)
        return -1;
    *attribute = p3_audio.ai_attr;
    return 0;
}

int IMP_AI_Enable(int device)
{
    if (device < 0 || device >= 2)
        return -1;
    if (p3_audio.ai_enabled)
        return 0;
    if (p3_audio.ai_attr.bitwidth != AUDIO_BIT_WIDTH_16) {
        IMP_LOG_LIMITED(LOG_ERR, "AI", "only 16-bit samples are supported");
        return -1;
    }
    if (p3_audio_parameter(AMIC_AI_SET_PARAM, &p3_audio.ai_attr) != 0 ||
        ioctl(p3_audio.fd, AMIC_AI_ENABLE_STREAM, 1) != 0) {
        p3_audio_maybe_close();
        return -1;
    }
    p3_audio.ai_enabled = 1;
    return 0;
}

int IMP_AI_EnableChn(int device, int channel)
{
    int depth = p3_audio.ai_channel.usrFrmDepth;
    size_t bytes;

    if (!t32_ai_args(device, channel))
        return -1;
    if (!p3_audio.ai_enabled || p3_audio.ai_channel_enabled)
        return 0;                       /* vendor: logged, 0 */
    if (depth < 2 || depth > p3_audio.ai_attr.frmNum) {
        IMP_LOG_LIMITED(LOG_ERR, "AI", "The channel parameters are not set");
        return -1;
    }
    bytes = (size_t)p3_audio.ai_attr.numPerFrm *
            (p3_audio.ai_attr.soundmode == AUDIO_SOUND_MODE_STEREO ? 2U : 1U) *
            sizeof(int16_t);
    if (!bytes)
        return -1;
    p3_audio.cap_depth = (unsigned)(depth < P3_CAPTURE_SLOTS ? depth
                                                             : P3_CAPTURE_SLOTS);
    t32_ai.vol = 60;
    t32_ai.vol_live = t32_ai.vol_saved = 1.0;
    if (p3_capture_start(bytes) != 0)
        return -1;
    p3_audio.ai_channel_enabled = 1;
    return 0;
}

int IMP_AI_PollingFrame(int device, int channel, unsigned int timeout_ms)
{
    struct timespec deadline;
    int ready;

    if (!t32_ai_args(device, channel) || !p3_audio.ai_channel_enabled ||
        p3_audio.frame_outstanding)
        return -1;
    t32_deadline(&deadline, (int)(timeout_ms > 60000U ? 60000U : timeout_ms));
    pthread_mutex_lock(&p3_cap_lock);
    while (p3_audio.cap_running && !p3_audio.cap_count && !p3_audio.cap_exited &&
           pthread_cond_timedwait(&p3_cap_cond, &p3_cap_lock, &deadline) == 0)
        ;
    ready = p3_audio.cap_count > 0;
    pthread_mutex_unlock(&p3_cap_lock);
    return ready ? 0 : -1;
}

int IMP_AI_SetVol(int device, int channel, int value)
{
    double factor;

    if (!t32_ai_args(device, channel))
        return -1;
    factor = t32_vol_factor(&value);
    t32_ai.mute = 0;
    p3_audio.ai_mute = 0;
    t32_ai.vol_live = t32_ai.vol_saved = factor;
    t32_ai.vol = value;
    return 0;
}

int IMP_AI_GetVol(int device, int channel, int *value)
{
    if (!t32_ai_args(device, channel) || !value)
        return -1;
    *value = t32_ai.vol;
    return 0;
}

int IMP_AI_SetGain(int device, int channel, int value)
{
    P3AudioVolume volume;

    if (!t32_ai_args(device, channel))
        return -1;
    if (value < 0)
        value = 0;
    else if (value > 31)
        value = 31;
    memset(&volume, 0, sizeof(volume));
    volume.channel = (uint32_t)p3_audio.ai_attr.chnCnt;
    volume.gain[0] = volume.gain[1] = (uint32_t)value;
    if (p3_audio.fd < 0 || ioctl(p3_audio.fd, AMIC_AI_SET_GAIN, &volume) != 0)
        IMP_LOG_LIMITED(LOG_ERR, "AI", "IMP_AI_SetGain: set gain %d failed", value);
    t32_ai.gain = value;
    return 0;
}

int IMP_AI_GetGain(int device, int channel, int *value)
{
    if (!t32_ai_args(device, channel) || !value)
        return -1;
    *value = t32_ai.gain;
    return 0;
}

int IMP_AI_SetVolMute(int device, int channel, int mute)
{
    if (!t32_ai_args(device, channel) || (mute != 0 && mute != 1))
        return -1;
    t32_ai.mute = mute;
    t32_ai.vol_live = mute ? 0.0 : t32_ai.vol_saved;
    p3_audio.ai_mute = mute;
    return 0;
}

int IMP_AI_SetAlgoOrder(int device, int channel, AudioAI_Type order[])
{
    if (!t32_ai_args(device, channel) || !order)
        return -1;
    t32_ai.order[0] = (int)order[0];
    t32_ai.order[1] = (int)order[1];
    t32_ai.order[2] = (int)order[2];
    t32_ai.order_set = 1;
    return 0;
}

int IMP_AI_GetAlgoOrder(int device, int channel, AudioAI_Type order[])
{
    int i;

    if (!t32_ai_args(device, channel) || !order)
        return -1;
    for (i = 0; i < 3; i++)
        order[i] = (AudioAI_Type)(t32_ai.order_set ? t32_ai.order[i] : i);
    return 0;
}

int IMP_AI_SetLpfCoFrequency(int frequency)
{
    if (!p3_audio.ai_enabled) {
        IMP_LOG_LIMITED(LOG_ERR, "AI", "LPF: ai device is not enabled");
        return 0;
    }
    t32_ai.lpf_cutoff = frequency;
    return 0;
}

int IMP_AI_EnableLpf(IMPAudioIOAttr *attribute)
{
    T32LpfCreate create;

    if (!attribute || attribute->bitwidth != AUDIO_BIT_WIDTH_16 ||
        !t32_ten_ms(attribute->numPerFrm, attribute->samplerate))
        return -1;
    if (t32_ai.lpf_enabled)
        return 0;
    if (t32_ai.lpf_cutoff < 0 || t32_ai.lpf_cutoff > (int)attribute->samplerate) {
        IMP_LOG_LIMITED(LOG_ERR, "AI", "LPF cut-off frequency is illegal");
        return -1;
    }
    create = (T32LpfCreate)t32_effect_symbol("audio_process_lpf_create");
    t32_ai.lpf_process =
        (T32LpfProcess)t32_effect_symbol("audio_process_lpf_process");
    if (!create || !t32_ai.lpf_process || !t32_effect_symbol("audio_process_lpf_free"))
        return -1;
    memset(t32_ai.lpf_state, 0, sizeof(t32_ai.lpf_state));
    create(t32_ai.lpf_state, attribute->samplerate, t32_ai.lpf_cutoff);
    if (p3_audio.ai_enabled)
        t32_ai.lpf_enabled = 1;
    else
        IMP_LOG_LIMITED(LOG_ERR, "AI", "LPF: ai device is not enabled");
    return 0;
}

int IMP_AI_DisableLpf(void)
{
    t32_ai.lpf_enabled = 0;
    return 0;
}

int IMP_AI_EnableDrcAndEq(IMPAudioIOAttr *attribute, IMPAudioDrcEqConfig *config,
                          int drc_enable, int eq_enable)
{
    return t32_drc_eq_enable(&t32_ai.drc, attribute, config, drc_enable,
                             eq_enable, p3_audio.ai_enabled, "AI");
}

int IMP_AI_DisableDrcAndEq(void)
{
    t32_drc_eq_free(&t32_ai.drc);
    return 0;
}

static void t32_ai_effects(int16_t *samples, int count)
{
    const IMPAudioIOAttr *attr = &p3_audio.ai_attr;
    int i;

    if (attr->bitwidth == AUDIO_BIT_WIDTH_16 &&
        t32_ten_ms(attr->numPerFrm, attr->samplerate)) {
        int frame = t32_algo_frame(attr->samplerate);

        for (i = 0; i < 3; i++) {
            switch (t32_ai.order_set ? t32_ai.order[i] : i) {
            case AI_MODULE_NS:
                if (p3_audio.ns_enabled)
                    t32_ns_run(p3_audio.ns, samples, count, frame);
                break;
            case AI_MODULE_AGC:
                if (p3_audio.agc_enabled)
                    t32_agc_run(p3_audio.agc, samples, count, frame);
                break;
            case AI_MODULE_HPF:
                if (p3_audio.hpf_enabled)
                    p3_audio.hpf_process(p3_audio.hpf_state, samples, count);
                break;
            default:
                break;
            }
        }
        t32_drc_eq_run(&t32_ai.drc, samples, count);
        if (t32_ai.lpf_enabled && t32_ai.lpf_process)
            t32_ai.lpf_process(t32_ai.lpf_state, samples, count);
        if (openimp_t23_ai_pre_effects)
            openimp_t23_ai_pre_effects(samples, count, attr->samplerate);
    }
    t32_apply_volume(samples, samples, (size_t)count, t32_ai.vol_live);
}

/* ---- AO ---------------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;       /* queue + thread state */
    pthread_mutex_t send_lock;  /* the open block (one producer at a time) */
    pthread_cond_t cond;        /* to the play thread */
    pthread_cond_t done;        /* from the play thread */
    pthread_t thread;
    int thread_run;
    int thread_started;
    int chn_enabled;
    int chn_idle;               /* play thread saw the channel disabled */
    int paused;
    int pause_done;
    int cache;
    int filling;
    int node_samples;           /* numPerFrm of a queued block */
    size_t node_bytes;
    unsigned char *nodes;
    unsigned char *open_block;
    size_t fill;
    int total;
    int free_n;
    int busy;
    int head;
    unsigned generation;        /* ClearChnBuf */
    double vol_live;
    double vol_saved;
    int vol;
    int gain;
    int mute;
    int order[3];
    int order_set;
    int hpf_enabled;
    int hpf_cutoff;
    int16_t hpf_state[16];
    void *agc;
    int agc_enabled;
    void *ns;
    int ns_frame;
    int ns_enabled;
    T32DrcEq drc;
} t32_ao = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .send_lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .done = PTHREAD_COND_INITIALIZER,
};

static int t32_ao_cache_default(void)
{
    const char *value = getenv("OPENIMP_AO_CACHE");

    return !(value && value[0] == '0');
}

static int t32_ao_ready(int device, int channel)
{
    return device == 0 && p3_audio.ao_enabled && channel == 0;
}

/* lock held */
static void t32_ao_wait(pthread_cond_t *cond, int ms)
{
    struct timespec deadline;

    t32_deadline(&deadline, ms);
    pthread_cond_timedwait(cond, &t32_ao.lock, &deadline);
}

static void t32_ao_write(const unsigned char *data, size_t len)
{
    while (len > 3) {
        P3AudioOutputStream stream;

        memset(&stream, 0, sizeof(stream));
        stream.data = (void *)(uintptr_t)data;
        stream.size = (uint32_t)len;
        if (ioctl(p3_audio.fd, AMIC_AO_SET_STREAM, &stream) != 0)
            IMP_LOG_LIMITED(LOG_ERR, "AO", "write to /dev/dsp failed");
        /* the driver answers the bytes it took in size */
        if ((int32_t)stream.size <= 0 || stream.size > len)
            break;
        len -= stream.size;
        data += stream.size;
    }
}

static void *t32_ao_play_main(void *argument)
{
    (void)argument;
    openimp_thread_setname("ao-_ao_play_thread");
    pthread_mutex_lock(&t32_ao.lock);
    while (t32_ao.thread_run) {
        int take = 0;

        if (!t32_ao.chn_enabled) {
            t32_ao.chn_idle = 1;
            pthread_cond_broadcast(&t32_ao.done);
        } else if (t32_ao.paused) {
            t32_ao.pause_done = 1;
            pthread_cond_broadcast(&t32_ao.done);
        } else if (!t32_ao.cache) {
            t32_ao.filling = 0;
            take = t32_ao.busy > 0;
            if (!take)
                t32_ao.filling = 1;
        } else if (!t32_ao.filling) {
            take = t32_ao.busy > 0;
            if (!take)
                t32_ao.filling = 1;
        } else if (t32_ao.free_n <= t32_ao.busy && t32_ao.busy > 0) {
            t32_ao.filling = 0;
            take = 1;
        }
        if (!take) {
            t32_ao_wait(&t32_ao.cond, 5000);
            continue;
        }
        {
            unsigned generation = t32_ao.generation;
            const unsigned char *block =
                t32_ao.nodes + (size_t)t32_ao.head * t32_ao.node_bytes;

            pthread_mutex_unlock(&t32_ao.lock);
            t32_ao_write(block, t32_ao.node_bytes);
            pthread_mutex_lock(&t32_ao.lock);
            if (generation == t32_ao.generation && t32_ao.busy > 0) {
                t32_ao.head = (t32_ao.head + 1) % t32_ao.total;
                t32_ao.busy--;
                t32_ao.free_n++;
            }
            pthread_cond_broadcast(&t32_ao.done);
        }
    }
    pthread_mutex_unlock(&t32_ao.lock);
    return NULL;
}

int IMP_AO_SetPubAttr(int device, IMPAudioIOAttr *attribute)
{
    int samples;

    if (device != 0 || !attribute ||
        attribute->soundmode != AUDIO_SOUND_MODE_MONO)
        return -1;
    samples = attribute->numPerFrm;
    if (attribute->samplerate > 0) {
        int block = attribute->samplerate / 100 * 4;

        if (!t32_ten_ms(samples, attribute->samplerate))
            IMP_LOG_LIMITED(LOG_WARNING, "AO",
                            "numPerFrm should be a multiple of 10 ms");
        else if (samples < block)
            samples = block;    /* vendor: 40 ms blocks */
    }
    p3_audio.ao_attr = *attribute;
    t32_ao.node_samples = samples;
    return 0;
}

int IMP_AO_Enable(int device)
{
    if (device != 0)
        return -1;
    if (p3_audio.ao_enabled)
        return 0;
    if (p3_audio_parameter(AMIC_AO_SET_PARAM, &p3_audio.ao_attr) != 0 ||
        ioctl(p3_audio.fd, AMIC_AO_ENABLE_STREAM, 1) != 0) {
        p3_audio_maybe_close();
        return -1;
    }
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.thread_run = 1;
    t32_ao.chn_enabled = 0;
    pthread_mutex_unlock(&t32_ao.lock);
    if (pthread_create(&t32_ao.thread, NULL, t32_ao_play_main, NULL) != 0) {
        t32_ao.thread_run = 0;
        (void)ioctl(p3_audio.fd, AMIC_AO_DISABLE_STREAM, 1);
        p3_audio_maybe_close();
        return -1;
    }
    t32_ao.thread_started = 1;
    p3_audio.ao_enabled = 1;
    return 0;
}

static void t32_ao_free_queue(void)
{
    free(t32_ao.nodes);
    free(t32_ao.open_block);
    t32_ao.nodes = NULL;
    t32_ao.open_block = NULL;
    t32_ao.fill = 0;
    t32_ao.total = t32_ao.free_n = t32_ao.busy = t32_ao.head = 0;
}

int IMP_AO_Disable(int device)
{
    int result = 0;

    if (device != 0)
        return -1;
    if (!p3_audio.ao_enabled)
        return 0;
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.thread_run = 0;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_mutex_unlock(&t32_ao.lock);
    if (t32_ao.thread_started)
        pthread_join(t32_ao.thread, NULL);
    t32_ao.thread_started = 0;
    if (ioctl(p3_audio.fd, AMIC_AO_DISABLE_STREAM, 1) != 0) {
        IMP_LOG_LIMITED(LOG_ERR, "AO", "AMIC_AO_DISABLE_STREAM failed");
        result = 0;     /* vendor: logged only */
    }
    if (t32_ao.chn_enabled) {
        t32_ao.chn_enabled = 0;
        t32_ao_free_queue();
    }
    p3_audio.ao_enabled = 0;
    p3_audio.ao_channel_enabled = 0;
    p3_audio_maybe_close();
    return result;
}

int IMP_AO_EnableChn(int device, int channel)
{
    size_t bytes;
    int total = p3_audio.ao_attr.frmNum;

    if (device != 0)
        return -1;
    if (!p3_audio.ao_enabled)
        return 0;                       /* vendor: logged, 0 */
    if (channel != 0)
        return -1;
    if (t32_ao.chn_enabled)
        return 0;
    bytes = (size_t)t32_ao.node_samples * sizeof(int16_t);
    if (!bytes || total < 1)
        return -1;
    pthread_mutex_lock(&t32_ao.send_lock);
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao_free_queue();
    t32_ao.nodes = calloc((size_t)total, bytes);
    t32_ao.open_block = calloc(1, bytes);
    if (!t32_ao.nodes || !t32_ao.open_block) {
        t32_ao_free_queue();
        pthread_mutex_unlock(&t32_ao.lock);
        pthread_mutex_unlock(&t32_ao.send_lock);
        return -1;
    }
    t32_ao.node_bytes = bytes;
    t32_ao.total = t32_ao.free_n = total;
    t32_ao.busy = t32_ao.head = 0;
    t32_ao.fill = 0;
    t32_ao.vol = 60;
    t32_ao.vol_live = t32_ao.vol_saved = 1.0;
    t32_ao.cache = t32_ao_cache_default();
    t32_ao.filling = 1;
    t32_ao.paused = 0;
    t32_ao.pause_done = 0;
    t32_ao.chn_idle = 0;
    t32_ao.chn_enabled = 1;
    p3_audio.ao_channel_enabled = 1;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_mutex_unlock(&t32_ao.lock);
    pthread_mutex_unlock(&t32_ao.send_lock);
    return 0;
}

int IMP_AO_DisableChn(int device, int channel)
{
    struct timespec deadline;

    if (device != 0 || !p3_audio.ao_enabled || channel != 0)
        return -1;
    if (!t32_ao.chn_enabled)
        return 0;
    pthread_mutex_lock(&t32_ao.send_lock);
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.chn_enabled = 0;
    t32_ao.chn_idle = 0;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_cond_broadcast(&t32_ao.done);
    t32_deadline(&deadline, 2000);
    while (!t32_ao.chn_idle &&
           pthread_cond_timedwait(&t32_ao.done, &t32_ao.lock, &deadline) == 0)
        ;
    if (t32_ao.chn_idle)
        t32_ao_free_queue();            /* else the thread still writes it */
    p3_audio.ao_channel_enabled = 0;
    pthread_mutex_unlock(&t32_ao.lock);
    pthread_mutex_unlock(&t32_ao.send_lock);
    return 0;
}

static void t32_ao_effects(int16_t *samples, int count)
{
    const IMPAudioIOAttr *attr = &p3_audio.ao_attr;
    int i;

    if (attr->bitwidth != AUDIO_BIT_WIDTH_16 ||
        !t32_ten_ms(t32_ao.node_samples, attr->samplerate))
        return;
    for (i = 0; i < 3; i++) {
        switch (t32_ao.order_set ? t32_ao.order[i] : i) {
        case AO_MODULE_HPF:
            if (t32_ao.hpf_enabled)
                p3_audio.hpf_process(t32_ao.hpf_state, samples, count);
            break;
        case AO_MODULE_AGC:
            if (t32_ao.agc_enabled)
                t32_agc_run(t32_ao.agc, samples, count,
                            t32_algo_frame(attr->samplerate));
            break;
        case AO_MODULE_NS:
            if (t32_ao.ns_enabled)
                t32_ns_run(t32_ao.ns, samples, count, t32_ao.ns_frame);
            break;
        default:
            break;
        }
    }
    t32_drc_eq_run(&t32_ao.drc, samples, count);
}

/* lock held; a free block index, or -1 (NOBLOCK, or the channel went away) */
static int t32_ao_get_free(IMPBlock block)
{
    while (t32_ao.chn_enabled && t32_ao.busy >= t32_ao.total) {
        if (block != BLOCK)
            return -1;
        t32_ao_wait(&t32_ao.done, 5000);
    }
    if (!t32_ao.chn_enabled)
        return -1;
    return (t32_ao.head + t32_ao.busy) % t32_ao.total;
}

/* send_lock held: effects + volume over the open block into a free slot */
static int t32_ao_queue_block(IMPBlock block, int effects)
{
    int slot;
    unsigned char *node;

    pthread_mutex_lock(&t32_ao.lock);
    slot = t32_ao_get_free(block);
    pthread_mutex_unlock(&t32_ao.lock);
    if (slot < 0)
        return -1;
    node = t32_ao.nodes + (size_t)slot * t32_ao.node_bytes;
    if (effects)
        t32_ao_effects((int16_t *)(void *)t32_ao.open_block,
                       (int)(t32_ao.node_bytes / sizeof(int16_t)));
    t32_apply_volume((const int16_t *)(const void *)t32_ao.open_block,
                     (int16_t *)(void *)node,
                     t32_ao.node_bytes / sizeof(int16_t), t32_ao.vol_live);
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.busy++;
    t32_ao.free_n--;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_SendFrame(int device, int channel, IMPAudioFrame *frame,
                     IMPBlock block)
{
    const unsigned char *data;
    size_t len, room;

    if (!frame || device != 0 || channel != 0 || !p3_audio.ao_enabled ||
        !t32_ao.chn_enabled)
        return -1;
    if (frame->len < 1)
        return 0;
    len = (size_t)frame->len;
    data = (const unsigned char *)(const void *)frame->virAddr;
    if (!data || p3_audio.ao_attr.bitwidth != AUDIO_BIT_WIDTH_16 ||
        len > (size_t)p3_audio.ao_attr.numPerFrm * sizeof(int16_t)) {
        IMP_LOG_LIMITED(LOG_ERR, "AO", "ao frame size is too big");
        return -1;
    }
    pthread_mutex_lock(&t32_ao.send_lock);
    if (!t32_ao.open_block) {
        pthread_mutex_unlock(&t32_ao.send_lock);
        return -1;
    }
    room = t32_ao.node_bytes - t32_ao.fill;
    if (len < room) {
        memcpy(t32_ao.open_block + t32_ao.fill, data, len);
        t32_ao.fill += len;
        pthread_mutex_unlock(&t32_ao.send_lock);
        return 0;
    }
    {
        /* the block is complete: queue it and keep the rest */
        unsigned char *rest_data;

        memcpy(t32_ao.open_block + t32_ao.fill, data, room);
        if (t32_ao_queue_block(block, 1) != 0) {
            pthread_mutex_unlock(&t32_ao.send_lock);
            return -1;
        }
        rest_data = (unsigned char *)(uintptr_t)(data + room);
        memcpy(t32_ao.open_block, rest_data, len - room);
        t32_ao.fill = len - room;
    }
    pthread_mutex_unlock(&t32_ao.send_lock);
    return 0;
}

int IMP_AO_FlushChnBuf(int device, int channel)
{
    struct timespec deadline;

    if (!t32_ao_ready(device, channel) || !t32_ao.chn_enabled)
        return -1;
    pthread_mutex_lock(&t32_ao.send_lock);
    if (t32_ao.open_block) {
        memset(t32_ao.open_block + t32_ao.fill, 0,
               t32_ao.node_bytes - t32_ao.fill);
        (void)t32_ao_queue_block(BLOCK, 0);
        t32_ao.fill = 0;
    }
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.filling = 0;
    pthread_cond_broadcast(&t32_ao.cond);
    t32_deadline(&deadline, 5000);
    while (t32_ao.busy && t32_ao.chn_enabled &&
           pthread_cond_timedwait(&t32_ao.done, &t32_ao.lock, &deadline) == 0)
        ;
    pthread_mutex_unlock(&t32_ao.lock);
    pthread_mutex_unlock(&t32_ao.send_lock);
    if (ioctl(p3_audio.fd, AMIC_AO_SYNC_STREAM, 0) != 0) {
        IMP_LOG_LIMITED(LOG_ERR, "AO", "ao dev sync chn buf error");
        return -1;
    }
    return 0;
}

int IMP_AO_ClearChnBuf(int device, int channel)
{
    if (!t32_ao_ready(device, channel))
        return -1;
    pthread_mutex_lock(&t32_ao.send_lock);
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.generation++;
    t32_ao.busy = 0;
    t32_ao.free_n = t32_ao.total;
    t32_ao.head = 0;
    if (ioctl(p3_audio.fd, AMIC_AO_CLEAR_STREAM, 0) != 0)
        IMP_LOG_LIMITED(LOG_ERR, "AO", "ao dev clear buf error");
    if (t32_ao.open_block)
        memset(t32_ao.open_block, 0, t32_ao.node_bytes);
    t32_ao.fill = 0;
    pthread_cond_broadcast(&t32_ao.done);
    pthread_mutex_unlock(&t32_ao.lock);
    pthread_mutex_unlock(&t32_ao.send_lock);
    return 0;
}

int IMP_AO_PauseChn(int device, int channel)
{
    struct timespec deadline;

    if (!t32_ao_ready(device, channel))
        return -1;
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.paused = 1;
    t32_ao.pause_done = 0;
    pthread_cond_broadcast(&t32_ao.cond);
    t32_deadline(&deadline, 3000);
    while (!t32_ao.pause_done && t32_ao.chn_enabled &&
           pthread_cond_timedwait(&t32_ao.done, &t32_ao.lock, &deadline) == 0)
        ;
    t32_ao.pause_done = 0;
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_ResumeChn(int device, int channel)
{
    if (!t32_ao_ready(device, channel))
        return -1;
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.paused = 0;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_QueryChnStat(int device, int channel, IMPAudioOChnState *status)
{
    if (!t32_ao_ready(device, channel) || !status)
        return -1;
    pthread_mutex_lock(&t32_ao.lock);
    status->chnTotalNum = t32_ao.total;
    status->chnFreeNum = t32_ao.free_n;
    status->chnBusyNum = t32_ao.busy;
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_CacheSwitch(int device, int channel, int enable)
{
    if (device != 0)
        return -1;
    if (!p3_audio.ao_enabled)
        return 0;                       /* vendor: logged, 0 */
    if (channel != 0)
        return -1;
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.cache = enable;
    pthread_cond_broadcast(&t32_ao.cond);
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_SetVol(int device, int channel, int value)
{
    double factor;

    if (!t32_ao_ready(device, channel))
        return -1;
    factor = t32_vol_factor(&value);
    pthread_mutex_lock(&t32_ao.lock);
    t32_ao.vol_live = t32_ao.vol_saved = factor;
    t32_ao.vol = value;
    pthread_mutex_unlock(&t32_ao.lock);
    return 0;
}

int IMP_AO_GetVol(int device, int channel, int *value)
{
    if (!t32_ao_ready(device, channel) || !value)
        return -1;
    *value = t32_ao.vol;
    return 0;
}

int IMP_AO_SetGain(int device, int channel, int value)
{
    P3AudioVolume volume;

    if (!t32_ao_ready(device, channel))
        return -1;
    if (value < 0)
        value = 0;
    else if (value > 31)
        value = 31;
    memset(&volume, 0, sizeof(volume));
    volume.channel = 1;
    volume.gain[0] = volume.gain[1] = (uint32_t)value;
    if (ioctl(p3_audio.fd, AMIC_SPK_SET_GAIN, &volume) != 0)
        IMP_LOG_LIMITED(LOG_ERR, "AO", "Set ao Play Gain Error");
    t32_ao.gain = value;
    return 0;
}

int IMP_AO_GetGain(int device, int channel, int *value)
{
    if (!t32_ao_ready(device, channel) || !value)
        return -1;
    *value = t32_ao.gain;
    return 0;
}

int IMP_AO_SetVolMute(int device, int channel, int mute)
{
    if (!t32_ao_ready(device, channel) || (mute != 0 && mute != 1))
        return -1;
    t32_ao.vol_live = mute ? 0.0 : t32_ao.vol_saved;
    t32_ao.mute = mute;
    p3_audio.ao_mute = mute;
    return 0;
}

/* vendor _ao_play_mute_thread / _ao_play_unmute_thread: the volume steps by
 * 3 (1.5 dB) every 150 ms down to -30 / up to the set volume */
static void *t32_ao_ramp_main(void *argument)
{
    int down = argument != NULL;
    int step;

    openimp_thread_setname(down ? "ao-_ao_play_mute_thread"
                                : "ao-_ao_play_unmute_thread");
    if (down) {
        for (step = t32_ao.vol; step > -30; step -= 3) {
            int level = step;

            t32_ao.vol_live = t32_vol_factor(&level);
            usleep(150000);
        }
        t32_ao.vol_live = 0.0;
    } else {
        for (step = -30; step < t32_ao.vol; step += 3) {
            int level = step;

            t32_ao.vol_live = t32_vol_factor(&level);
            usleep(150000);
        }
        step = t32_ao.vol;
        t32_ao.vol_live = t32_vol_factor(&step);
    }
    return NULL;
}

static int t32_ao_ramp(int device, int channel, int down)
{
    pthread_t thread;

    if (!t32_ao_ready(device, channel))
        return -1;
    if (pthread_create(&thread, NULL, t32_ao_ramp_main,
                       down ? (void *)&t32_ao : NULL) != 0)
        return -1;
    pthread_detach(thread);
    return 0;
}

int IMP_AO_Soft_Mute(int device, int channel)
{
    return t32_ao_ramp(device, channel, 1);
}

int IMP_AO_Soft_UNMute(int device, int channel)
{
    return t32_ao_ramp(device, channel, 0);
}

int IMP_AO_SetAlgoOrder(int device, int channel, AudioAO_Type order[])
{
    if (device < 0 || device >= 1 || channel != 0 || !order)
        return -1;
    t32_ao.order[0] = (int)order[0];
    t32_ao.order[1] = (int)order[1];
    t32_ao.order[2] = (int)order[2];
    t32_ao.order_set = 1;
    return 0;
}

int IMP_AO_GetAlgoOrder(int device, int channel, AudioAO_Type order[])
{
    int i;

    if (device < 0 || device >= 1 || channel != 0 || !order)
        return -1;
    for (i = 0; i < 3; i++)
        order[i] = (AudioAO_Type)(t32_ao.order_set ? t32_ao.order[i] : i);
    return 0;
}

static int t32_ao_attr_ok(const IMPAudioIOAttr *attribute)
{
    return attribute && attribute->bitwidth == AUDIO_BIT_WIDTH_16 &&
           t32_ten_ms(attribute->numPerFrm, attribute->samplerate);
}

int IMP_AO_SetHpfCoFrequency(int frequency)
{
    if (!p3_audio.ao_enabled)
        return 0;                       /* vendor: logged, 0 */
    t32_ao.hpf_cutoff = frequency;
    return 0;
}

int IMP_AO_EnableHpf(IMPAudioIOAttr *attribute)
{
    if (!t32_ao_attr_ok(attribute) || t32_ao.hpf_cutoff < 0 ||
        t32_ao.hpf_cutoff > (int)attribute->samplerate || p3_effects_load() != 0)
        return -1;
    memset(t32_ao.hpf_state, 0, sizeof(t32_ao.hpf_state));
    p3_audio.hpf_create(t32_ao.hpf_state, t32_ao.hpf_state + 8, 0, 0, 8, 8);
    if (p3_audio.effects_neo && t32_ao.hpf_cutoff > 0 &&
        (int64_t)t32_ao.hpf_cutoff * 2 < (int64_t)attribute->samplerate)
        p3_hpf_neo_overlay(t32_ao.hpf_state, attribute->samplerate,
                           t32_ao.hpf_cutoff);
    if (p3_audio.ao_enabled)
        t32_ao.hpf_enabled = 1;
    return 0;
}

int IMP_AO_DisableHpf(void)
{
    t32_ao.hpf_enabled = 0;
    return 0;
}

int IMP_AO_EnableAgc(IMPAudioIOAttr *attribute, IMPAudioAgcConfig configuration)
{
    P3WebRtcAgcConfig config;

    if (!t32_ao_attr_ok(attribute) || p3_effects_load() != 0)
        return -1;
    if (attribute->samplerate != 8000 && attribute->samplerate != 16000 &&
        attribute->samplerate != 32000 && attribute->samplerate != 48000)
        return -1;
    if (!t32_ao.agc)
        t32_ao.agc = p3_audio.agc_create();
    if (!t32_ao.agc)
        return -1;
    config.target_level_dbfs = (int16_t)configuration.TargetLevelDbfs;
    config.compression_gain_db = (int16_t)configuration.CompressionGaindB;
    config.limiter_enable = 1;
    if (p3_audio.agc_set_config(t32_ao.agc, 0, 255,
                                p3_audio.agc_mode ? p3_audio.agc_mode : 2,
                                attribute->samplerate, config) != 0)
        return -1;
    if (p3_audio.ao_enabled)
        t32_ao.agc_enabled = 1;
    return 0;
}

int IMP_AO_DisableAgc(void)
{
    if (!t32_ao.agc)
        return 0;
    t32_ao.agc_enabled = 0;
    p3_audio.agc_free(t32_ao.agc);
    t32_ao.agc = NULL;
    return 0;
}

int IMP_AO_EnableNs(IMPAudioIOAttr *attribute, int mode)
{
    int rate;

    if (!t32_ao_attr_ok(attribute))
        return -1;
    if ((unsigned)mode > 3U)
        mode = 3;                       /* vendor: unsigned compare */
    if (t32_ao.ns_enabled)
        return 0;
    rate = attribute->samplerate;
    if (p3_effects_load() != 0)
        return -1;
    if (rate != 8000 && rate != 16000 && rate != 32000 && rate != 48000)
        return -1;
    if (!t32_ao.ns)
        t32_ao.ns = p3_audio.ns_create();
    if (!t32_ao.ns)
        return -1;
    if (p3_audio.ns_set_config(t32_ao.ns, rate, mode) != 0) {
        p3_audio.ns_free(t32_ao.ns);
        t32_ao.ns = NULL;
        return -1;
    }
    t32_ao.ns_frame = t32_algo_frame(rate);
    if (p3_audio.ao_enabled)
        t32_ao.ns_enabled = 1;
    return 0;
}

int IMP_AO_DisableNs(void)
{
    if (!t32_ao.ns)
        return 0;
    t32_ao.ns_enabled = 0;
    p3_audio.ns_free(t32_ao.ns);
    t32_ao.ns = NULL;
    return 0;
}

int IMP_AO_EnableDrcAndEq(IMPAudioIOAttr *attribute, IMPAudioDrcEqConfig *config,
                          int drc_enable, int eq_enable)
{
    return t32_drc_eq_enable(&t32_ao.drc, attribute, config, drc_enable,
                             eq_enable, p3_audio.ao_enabled, "AO");
}

int IMP_AO_DisableDrcAndEq(void)
{
    t32_drc_eq_free(&t32_ao.drc);
    return 0;
}
