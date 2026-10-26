/*
 * audio_devtest - device proof of the T41 audio-input extras (microphone
 * only, no audio output is opened):
 *   IMP_AI_Set/GetDigitalGain   read-back and capture level per gain
 *   IMP_AI_EnableGetRaw + GetFrameAndRaw   raw vs processed (HPF, mute)
 *   IMP_AI_EnableAlgo/DisableAlgo  with a webrtc_profile.ini in argv[1]
 *   IMP_AI_EnableHs/DisableHs, IMP_Audio_Select_Codec(SELECT_INTERNAL)
 *   IMP_Log_Get/Set_Option read-back
 * Output: one line per step on stdout.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_audio.h>
#include <imp/imp_log.h>

int IMP_Log_Get_Option(void);
void IMP_Log_Set_Option(int option);

static double rms(const int16_t *s, int n, double *mean)
{
    double sum = 0, sq = 0;
    int i;

    for (i = 0; i < n; i++) {
        sum += s[i];
        sq += (double)s[i] * s[i];
    }
    if (mean)
        *mean = sum / n;
    return sqrt(sq / n);
}

/* average level over k frames, discarding the first d */
static double level(int k, int d, double *mean_out)
{
    IMPAudioFrame f;
    double acc = 0, macc = 0, m;
    int i, got = 0;

    for (i = 0; i < k + d; i++) {
        if (IMP_AI_PollingFrame(0, 0, 1000) != 0 ||
            IMP_AI_GetFrame(0, 0, &f, BLOCK) != 0)
            continue;
        if (i >= d) {
            acc += rms((int16_t *)f.virAddr, f.len / 2, &m);
            macc += m;
            got++;
        }
        IMP_AI_ReleaseFrame(0, 0, &f);
    }
    if (mean_out)
        *mean_out = got ? macc / got : 0;
    return got ? acc / got : -1;
}

static void raw_pair(const char *name, int k)
{
    IMPAudioFrame f, r;
    double pr = 0, rr = 0, pm = 0, rm = 0, m;
    int i, got = 0, ret = 0, diff = 0;

    for (i = 0; i < k; i++) {
        if (IMP_AI_PollingFrame(0, 0, 1000) != 0)
            continue;
        ret = IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK);
        if (ret != 0)
            break;
        pr += rms((int16_t *)f.virAddr, f.len / 2, &m);
        pm += m;
        rr += rms((int16_t *)r.virAddr, r.len / 2, &m);
        rm += m;
        diff += memcmp(f.virAddr, r.virAddr, (size_t)f.len) != 0;
        got++;
        IMP_AI_ReleaseFrame(0, 0, &f);
    }
    printf("%s: ret %d frames %d differ %d processed rms %.1f mean %.1f | raw rms %.1f mean %.1f\n",
           name, ret, got, diff, got ? pr / got : 0, got ? pm / got : 0,
           got ? rr / got : 0, got ? rm / got : 0);
}

int main(int argc, char **argv)
{
    IMPAudioIOAttr attr;
    IMPAudioIChnParam chn;
    IMPAudioFrame f, r;
    double m;
    int g = -1, opt, i;

    opt = IMP_Log_Get_Option();
    IMP_Log_Set_Option(0x12);
    printf("log option: initial 0x%x, after Set(0x12) 0x%x\n", opt,
           IMP_Log_Get_Option());
    IMP_Log_Set_Option(opt);

    memset(&attr, 0, sizeof(attr));
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 640;
    attr.chnCnt = 1;
    chn.usrFrmDepth = 20;
    if (IMP_AI_SetPubAttr(0, &attr) || IMP_AI_Enable(0) ||
        IMP_AI_SetChnParam(0, 0, &chn) || IMP_AI_EnableChn(0, 0)) {
        printf("AI setup failed\n");
        return 1;
    }
    IMP_AI_SetVol(0, 0, 60);
    IMP_AI_SetGain(0, 0, 20);
    printf("GetFrameAndRaw before EnableGetRaw: %d\n",
           IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK));
    printf("Select_Codec(INTERNAL): %d, (2,0): %d\n",
           IMP_Audio_Select_Codec(0, 0, SELECT_INTERNAL),
           IMP_Audio_Select_Codec(2, 0, SELECT_INTERNAL));
    /* digital gain: read-back and level */
    for (i = 0; i < 3; i++) {
        static const int gains[3] = { 150, 200, 250 };

        printf("SetDigitalGain(%d): %d", gains[i],
               IMP_AI_SetDigitalGain(0, 0, gains[i]));
        printf(", Get: %d -> %d", IMP_AI_GetDigitalGain(0, 0, &g), g);
        printf(", level rms %.1f\n", level(25, 10, NULL));
    }
    printf("SetDigitalGain(300) clamps: %d, Get %d -> ",
           IMP_AI_SetDigitalGain(0, 0, 300), IMP_AI_GetDigitalGain(0, 0, &g));
    printf("%d; (2,0) %d (0,1) %d\n", g, IMP_AI_SetDigitalGain(2, 0, 1),
           IMP_AI_SetDigitalGain(0, 1, 1));
    IMP_AI_SetDigitalGain(0, 0, 200);
    /* raw vs processed */
    printf("EnableGetRaw: %d\n", IMP_AI_EnableGetRaw(0, 0));
    raw_pair("no effects", 20);
    IMP_AI_SetVolMute(0, 0, 1);
    raw_pair("software mute", 10);
    IMP_AI_SetVolMute(0, 0, 0);
    /* EnableAlgo with the profile (HPF on): DC offset removed */
    if (argc > 1)
        printf("Set_WebrtcProfileIni_Path(%s): %d\n", argv[1],
               IMP_AI_Set_WebrtcProfileIni_Path(argv[1]));
    printf("EnableAlgo: %d (2,0): %d\n", IMP_AI_EnableAlgo(0, 0),
           IMP_AI_EnableAlgo(2, 0));
    level(10, 0, NULL);
    raw_pair("after EnableAlgo (HPF+NS)", 25);
    printf("DisableAlgo: %d\n", IMP_AI_DisableAlgo(0, 0));
    raw_pair("after DisableAlgo", 10);
    printf("EnableHs: %d\n", IMP_AI_EnableHs());
    raw_pair("Hs on", 25);
    printf("DisableHs: %d\n", IMP_AI_DisableHs());
    printf("DisableGetRaw: %d, GetFrameAndRaw after: %d\n",
           IMP_AI_DisableGetRaw(0, 0),
           IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK));
    printf("level mean check: rms %.1f", level(10, 0, &m));
    printf(" mean %.1f\n", m);
    IMP_AI_DisableChn(0, 0);
    IMP_AI_Disable(0);
    return 0;
}
