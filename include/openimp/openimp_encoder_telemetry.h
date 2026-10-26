/* OpenIMP extension (not in any vendor SDK): cumulative per-channel encoder
 * telemetry.  Counted when GetStream hands a stream out; reset by
 * IMP_Encoder_CreateChn.  Link it weakly if the streamer must also run on the
 * vendor libimp.
 *
 * ABI: set t->size = sizeof(*t) before the call.  The library fills at most
 * that many bytes, writes back how many it filled and sets t->version; new
 * fields are only ever appended, so an older caller keeps working. */
#ifndef OPENIMP_ENCODER_TELEMETRY_H
#define OPENIMP_ENCODER_TELEMETRY_H

#include <stdint.h>
#include <imp/imp_encoder.h>

#define OPENIMP_ENCODER_TELEMETRY_VERSION 1

typedef struct {
    uint32_t size;              /* in: sizeof(*t); out: bytes filled */
    uint32_t version;           /* out: OPENIMP_ENCODER_TELEMETRY_VERSION */
    uint64_t frames;            /* streams handed out by GetStream */
    uint64_t bytes;             /* their total length */
    uint64_t idr_frames;
    uint32_t last_frame_bytes;
    uint32_t encode_us_last;    /* source frame -> codec -> stream back */
    uint32_t encode_us_avg;
    uint32_t encode_us_max;
    uint32_t fps_x100;          /* delivered frame rate x100 (first to last stream) */
    uint32_t avg_kbps;          /* bit rate over the same window */
    uint32_t encoding;          /* 1 while a picture is in the codec */
} OpenIMPEncoderTelemetry;

int OpenIMP_Encoder_GetTelemetry(int encChn, OpenIMPEncoderTelemetry *t);

/* Beyond vendor: tells whether a JPEG stream is a stand-in.  When the JPEG
 * encoder is busy (or rmem is short) OpenIMP delivers the channel's last
 * picture (at most 5 s old) again, stamped with the CURRENT frame time.
 * Between IMP_Encoder_GetStream() and IMP_Encoder_ReleaseStream() call this
 * with the channel and the stream just got: 1 = reused old picture, 0 = a
 * freshly encoded one, -1 = bad channel / not the stream currently held.
 * The IMPEncoderStream layout is untouched (vendor ABI).  Weak-symbol
 * detectable (a vendor libimp does not export it). */
int OpenIMP_Encoder_StreamIsReused(int encChn, const IMPEncoderStream *stream);

#endif
