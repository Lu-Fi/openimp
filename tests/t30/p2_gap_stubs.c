/* The two persistence calls EncoderInit() reaches through its startup trace;
 * defined without prototypes (separate translation unit).  Everything else
 * the encoder file references stays unresolved (see the Makefile). */
int openimp_t23_persist_enabled() { return 0; }
int openimp_t23_persist_write() { return 0; }
