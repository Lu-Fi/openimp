/* The two persistence calls EncoderInit() reaches through its startup trace;
 * defined without prototypes (separate translation unit).  Everything else
 * the encoder file references stays unresolved (see the Makefile). */
int openimp_t23_persist_enabled() { return 0; }
int openimp_t23_persist_write() { return 0; }
/* GetGOPSize on T30 asks the codec; the stub has none (fallback maxGop) */
int AL_Codec_Encode_GetGopLength() { return -1; }
int AL_Codec_Encode_GetRunningRate() { return -1; }

#if defined(PLATFORM_T30) && !defined(PLATFORM_T21)
#include <stdint.h>
/* T30: IMP_Encoder_SetPoolSize sizes the shared Helix bitstream buffer
 * (src/t30/helix_bitstream.c, #697) */
static uint32_t stub_helix_pool;
void OpenIMP_HelixBitstream_SetPoolSize(uint32_t size)
{
    stub_helix_pool = size;
}

uint32_t OpenIMP_HelixBitstream_PoolSize(void)
{
    return stub_helix_pool;
}
#endif
