/* The two persistence calls EncoderInit() reaches through its startup trace;
 * defined without prototypes (separate translation unit).  Everything else
 * the encoder file references stays unresolved (see the Makefile). */
int openimp_t23_persist_enabled() { return 0; }
int openimp_t23_persist_write() { return 0; }
/* GetGOPSize on T30 asks the codec; the stub has none (fallback maxGop) */
int AL_Codec_Encode_GetGopLength() { return -1; }
