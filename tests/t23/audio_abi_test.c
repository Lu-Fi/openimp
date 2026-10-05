/*
 * audio_abi_test - T23 audio structs in the vendor 1.1.0-1.3.0 layout.
 *
 * IMPAudioIChnParam has the AEC channel select between usrFrmDepth and Rev
 * (12 bytes).  With OpenIMP's old 8-byte struct, IMP_AI_GetChnParam left a
 * caller's Rev unset and IMP_AI_SetChnParam took aecChn for Rev.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <imp/imp_audio.h>

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

int main(void)
{
    IMPAudioIChnParam p;

    CHECK(sizeof(IMPAudioIChnParam) == 12, "size %zu", sizeof(p));
    CHECK(offsetof(IMPAudioIChnParam, usrFrmDepth) == 0, "usrFrmDepth");
    CHECK(offsetof(IMPAudioIChnParam, aecChn) == 4, "aecChn");
    CHECK(offsetof(IMPAudioIChnParam, Rev) == 8, "Rev");
    CHECK(AUDIO_AEC_CHANNEL_FIRST_LEFT == 0 &&
          AUDIO_AEC_CHANNEL_SECOND_RIGHT == 1 &&
          AUDIO_AEC_CHANNEL_THIRD == 2 && AUDIO_AEC_CHANNEL_FOURTH == 3,
          "aec channel values");
    memset(&p, 0, sizeof(p));
    if (failures) {
        fprintf(stderr, "t23 audio abi: %d check(s) failed\n", failures);
        return 1;
    }
    puts("t23 audio abi tests passed");
    return 0;
}
