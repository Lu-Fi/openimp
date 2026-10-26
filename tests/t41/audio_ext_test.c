/* T41 1.2.6 audio extras on the P3 audio path (driver ioctls faked):
 * IMP_AI_EnableGetRaw / DisableGetRaw / GetFrameAndRaw, IMP_Audio_Select_Codec,
 * IMP_AI/AO_Set/GetDigitalGain, IMP_A[IO]_EnableAlgo / DisableAlgo with a
 * webrtc_profile.ini, IMP_AI_EnableHs without libaudioProcess.  Argument
 * checks and return codes are the vendor's (ai.c/ao.c of the T41 1.2.5
 * libimp.a, 1.2.6 libimp.so for GetRaw and Select_Codec). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int64_t IMP_System_GetTimeStamp(void) { return 1234; }
static unsigned long fake_req;
static int fake_calls, fake_fail, fake_codec = -1;

static int fake_ioctl(int fd, unsigned long request, long arg);
#define ioctl(fd, req, arg) fake_ioctl((fd), (req), (long)(arg))
#define _Static_assert(...) extern int audio_ext_test_unused
#include "../../src/t40/openimp_p3_audio.c"
#undef _Static_assert
#undef ioctl

int IMP_AI_EnableAlgo(int device, int channel);
int IMP_AI_DisableAlgo(int device, int channel);
int IMP_AO_EnableAlgo(int device, int channel);
int IMP_AO_DisableAlgo(int device, int channel);
int IMP_AI_EnableHs(void);
int IMP_AI_DisableHs(void);

static int fake_ioctl(int fd, unsigned long request, long arg)
{
    (void)fd;
    fake_req = request;
    fake_calls++;
    if (fake_fail)
        return -1;
    if (request == AMIC_AI_GET_STREAM) {
        P3AudioInputStream *s = (P3AudioInputStream *)arg;
        int16_t *d = s->data;
        uint32_t i;

        for (i = 0; i < s->size / 2u; i++)
            d[i] = (int16_t)(100 + i);
    } else if (request == AMIC_SELECT_CODEC) {
        fake_codec = *(int *)arg;
    }
    return 0;
}

int main(void)
{
    IMPAudioFrame f, r;
    char dir[] = "/tmp/audio_ext_testXXXXXX";
    char path[96];
    FILE *ini;
    int g;

    p3_audio.fd = 3;
    /* GetRaw flags: device < 2, channel <= 0 */
    assert(IMP_AI_EnableGetRaw(2, 0) == -1 && IMP_AI_EnableGetRaw(0, 1) == -1);
    assert(IMP_AI_DisableGetRaw(2, 0) == -1);
    /* GetFrameAndRaw: device/channel enabled, raw enabled */
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK) == -1);
    p3_audio.ai_enabled = 1;
    p3_audio.ai_channel_enabled = 1;
    p3_audio.ai_attr.samplerate = 16000;
    p3_audio.ai_attr.bitwidth = AUDIO_BIT_WIDTH_16;
    p3_audio.ai_attr.soundmode = AUDIO_SOUND_MODE_MONO;
    p3_audio.ai_attr.numPerFrm = 160;
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK) == -1);   /* no raw */
    assert(IMP_AI_EnableGetRaw(0, 0) == 0);
    assert(IMP_AI_GetFrameAndRaw(2, 0, &f, &r, BLOCK) == -1);
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, NULL, BLOCK) == -1);
    /* the processed frame is muted, the raw one keeps the capture */
    p3_audio.ai_mute = 1;
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK) == 0);
    assert(f.len == 320 && r.len == 320 && f.virAddr != r.virAddr);
    assert(((int16_t *)r.virAddr)[0] == 100 && ((int16_t *)r.virAddr)[159] == 259);
    assert(((int16_t *)f.virAddr)[0] == 0 && ((int16_t *)f.virAddr)[159] == 0);
    assert(r.timeStamp == f.timeStamp && r.seq == f.seq);
    /* one frame outstanding until ReleaseFrame */
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK) == -1);
    assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
    p3_audio.ai_mute = 0;
    assert(IMP_AI_GetFrame(0, 0, &f, BLOCK) == 0 &&
           ((int16_t *)f.virAddr)[1] == 101);
    assert(IMP_AI_ReleaseFrame(0, 0, &f) == 0);
    assert(IMP_AI_DisableGetRaw(0, 0) == 0);
    assert(IMP_AI_GetFrameAndRaw(0, 0, &f, &r, BLOCK) == -1);

    /* Select_Codec: ioctl 0x4004504a with the codec, failure is -1 */
    assert(IMP_Audio_Select_Codec(2, 0, 0) == -1);
    assert(IMP_Audio_Select_Codec(0, 1, 0) == -1);
    assert(IMP_Audio_Select_Codec(0, 0, 1) == 0 && fake_codec == 1 &&
           fake_req == AMIC_SELECT_CODEC);
    fake_fail = 1;
    assert(IMP_Audio_Select_Codec(0, 0, 0) == -1);
    fake_fail = 0;

    /* digital gain on T41: same ioctls and checks as T40 */
    assert(IMP_AI_SetDigitalGain(0, 0, 300) == 0 &&
           (fake_req & 0xffffu) == 0x505bu);
    assert(IMP_AI_GetDigitalGain(0, 0, &g) == 0 && g == 255);
    assert(IMP_AI_SetDigitalGain(2, 0, 1) == -1 &&
           IMP_AI_SetDigitalGain(0, 1, 1) == -1);
    assert(IMP_AO_SetDigitalGain(0, 0, 10) == -1);      /* AO not enabled */
    p3_audio.ao_enabled = 1;
    assert(IMP_AO_SetDigitalGain(0, 0, -3) == 0 &&
           (fake_req & 0xffffu) == 0x5059u);
    assert(IMP_AO_GetDigitalGain(0, 0, &g) == 0 && g == 0);

    /* EnableAlgo reads webrtc_profile.ini from the profile directory */
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/webrtc_profile.ini", dir);
    ini = fopen(path, "w");
    assert(ini);
    fputs("[AGC_AI]\nAGC_enable = true\nset_mode = kFixedDigital\n"
          "[NS_AI]\nNS_enable = false\n[HP_AI]\nHP_enable = false\n", ini);
    fclose(ini);
    assert(IMP_AI_Set_WebrtcProfileIni_Path(dir) == 0);
    assert(IMP_AI_EnableAlgo(2, 0) == -1 && IMP_AI_EnableAlgo(0, 1) == -1);
    /* no libaudioProcess on the host: the AGC fails (logged, as the
     * vendor), the mode from the profile is taken, the call is 0 */
    assert(IMP_AI_EnableAlgo(0, 0) == 0);
    assert(p3_audio.agc_mode == 3);
    ini = fopen(path, "w");
    fputs("[HP_AI]\nHP_enable = true\ncofrequency = 300\n", ini);
    fclose(ini);
    /* a failing HPF fails the call, as the vendor */
    assert(IMP_AI_EnableAlgo(0, 0) == -1 && p3_audio.hpf_cutoff == 300);
    assert(IMP_AI_DisableAlgo(0, 0) == 0 && IMP_AI_DisableAlgo(2, 0) == -1);
    assert(IMP_AO_EnableAlgo(1, 0) == -1 && IMP_AO_DisableAlgo(0, 1) == -1);
    assert(IMP_AO_DisableAlgo(0, 0) == 0);
    unlink(path);
    rmdir(dir);

    /* howling suppression needs libaudioProcess.so */
    assert(IMP_AI_EnableHs() == -1);
    assert(IMP_AI_DisableHs() == 0);
    puts("T41 audio extras: all checks passed");
    return 0;
}
