/*
 * T32 AENC/ADEC host test (#298): the built-in G.711A/G.711U/G.726/ADPCM
 * encoders and decoders on a fixed synthetic PCM give the same bytes as the
 * vendor T32 libimp 2.2.0 (hashes recorded in QEMU with
 * tools/t32/t32_misc_sample.c "acodec 6", same PCM and hash), and the AENC
 * stream carries the frame's time stamp like the vendor (no wall clock).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <imp/imp_audio.h>

#define SAMPLES 320
#define FRAMES 6

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 16;   /* as in tools/t32/t32_misc_sample.c */
}

static uint32_t fnv(const void *p, int len)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; b && i < len; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* identical to acodec_pcm() in tools/t32/t32_misc_sample.c */
static void acodec_pcm(int16_t *pcm, int samples, int frame)
{
    static uint32_t seed = 12345;
    static int phase;
    int k;

    if (frame == 0) {
        seed = 12345;
        phase = 0;
    }
    for (k = 0; k < samples; k++) {
        int t = phase++;
        int tri = (t * (3 + frame)) & 0xffff;
        int v = (tri < 0x8000 ? tri : 0xffff - tri) * 2 - 0x8000;
        int amp = 1 + ((t >> 9) & 7);
        v = v * amp / 8 + (int)(lcg(&seed) >> 22) - 512;
        pcm[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* no AI/AO device here (the QEMU run did not set them up either) */
int IMP_AI_GetPubAttr(int dev, IMPAudioIOAttr *attr) { (void)dev; (void)attr; return -1; }
int IMP_AO_GetPubAttr(int dev, IMPAudioIOAttr *attr) { (void)dev; (void)attr; return -1; }

int main(void)
{
    /* vendor libimp 2.2.0 (QEMU ingenic-t32, audit/issue298/vac) */
    static const struct { const char *tag; int type, elen; uint32_t enc, dec; } v[] = {
        { "g711a", PT_G711A, 320, 0x457e47b6, 0x39d6e86a },
        { "g711u", PT_G711U, 320, 0xd33be4ef, 0xf8978aab },
        { "g726",  PT_G726,  160, 0x56be476d, 0x63479d9e },
        { "adpcm", PT_ADPCM, 160, 0x4186e14b, 0x3869bcf1 },
    };
    static uint8_t streams[FRAMES][1024];
    static int slen[FRAMES];
    int16_t pcm[SAMPLES];
    int t, i, fail = 0;

    for (t = 0; t < 4; t++) {
        IMPAudioEncChnAttr ea = { v[t].type, 20, NULL };
        IMPAudioDecChnAttr da = { v[t].type, 20, ADEC_MODE_PACK, NULL };
        uint32_t henc = 2166136261u, hdec = 2166136261u;
        int ts = 0, lens = 0;

        if (IMP_AENC_CreateChn(t, &ea)) {
            printf("FAIL %s create\n", v[t].tag);
            return 1;
        }
        for (i = 0; i < FRAMES; i++) {
            IMPAudioFrame f;
            IMPAudioStream s;

            acodec_pcm(pcm, SAMPLES, i);
            memset(&f, 0, sizeof f);
            f.bitwidth = AUDIO_BIT_WIDTH_16;
            f.soundmode = AUDIO_SOUND_MODE_MONO;
            f.virAddr = (uint32_t *)pcm;
            f.len = sizeof pcm;
            f.seq = i;
            f.timeStamp = 1000 + 20000LL * i;
            if (IMP_AENC_SendFrame(t, &f) || IMP_AENC_GetStream(t, &s, BLOCK)) {
                printf("FAIL %s enc %d\n", v[t].tag, i);
                return 1;
            }
            ts += s.timeStamp == f.timeStamp;
            lens += s.len == v[t].elen;
            slen[i] = s.len;
            memcpy(streams[i], s.stream, s.len);
            henc = fnv(s.stream, s.len) ^ (henc * 31u);
            IMP_AENC_ReleaseStream(t, &s);
        }
        IMP_AENC_DestroyChn(t);
        if (IMP_ADEC_CreateChn(t, &da)) {
            printf("FAIL %s dec create\n", v[t].tag);
            return 1;
        }
        for (i = 0; i < FRAMES; i++) {
            IMPAudioStream in, out;

            memset(&in, 0, sizeof in);
            in.stream = streams[i];
            in.len = slen[i];
            if (IMP_ADEC_SendStream(t, &in, BLOCK) || IMP_ADEC_GetStream(t, &out, BLOCK)) {
                printf("FAIL %s dec %d\n", v[t].tag, i);
                return 1;
            }
            hdec = fnv(out.stream, out.len) ^ (hdec * 31u);
            IMP_ADEC_ReleaseStream(t, &out);
        }
        IMP_ADEC_DestroyChn(t);
        printf("%s %s: enc %08x (vendor %08x) dec %08x (vendor %08x) len %d/%d ts=frame %d/%d\n",
               henc == v[t].enc && hdec == v[t].dec && ts == FRAMES && lens == FRAMES ? "ok  " : "FAIL",
               v[t].tag, henc, v[t].enc, hdec, v[t].dec, lens, FRAMES, ts, FRAMES);
        fail |= henc != v[t].enc || hdec != v[t].dec || ts != FRAMES || lens != FRAMES;
    }
    printf("t32 acodec: %s\n", fail ? "FAIL" : "PASS (4 codecs bit-exact with vendor 2.2.0, frame time stamps)");
    return fail;
}
