#ifndef OPENIMP_CODEC_FRAME_TS_H
#define OPENIMP_CODEC_FRAME_TS_H
#include <stdint.h>
#include <string.h>

/* Capture timestamp inside the FrameSource frame record handed to the codec.
 * T31/T30: 0x20.  T41/T23/T40: 0x28 (T40 1.3.1 IMPFrameInfo.timeStamp). */
static inline uint64_t codec_frame_capture_timestamp(const void *frame)
{
    uint64_t ts = 0;
#if defined(PLATFORM_T31) || defined(PLATFORM_T30)
    memcpy(&ts, (const uint8_t *)frame + 0x20, sizeof(ts));
#elif defined(PLATFORM_T41) || defined(PLATFORM_T23) || defined(PLATFORM_T40)
    memcpy(&ts, (const uint8_t *)frame + 0x28, sizeof(ts));
#else
    (void)frame;
#endif
    return ts;
}
#endif
