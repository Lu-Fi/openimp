#ifndef OPENIMP_T40_VENDOR_CMD_H
#define OPENIMP_T40_VENDOR_CMD_H

/*
 * T40 command-list values recovered from the vendor libimp 1.3.1 (Eufy
 * T40XP, SC830AI, H.264 High; command lists captured at the AVPU push at
 * 3840x2160, 1920x1080, 1280x720 and 640x360, CBR/VBR/FixQP, 15/20 fps,
 * GOP 20..80).  Pure functions so tests/t40/vendor_cmd_test.c can check
 * them against the captured words.
 */
#include <stdint.h>

/* cmd[0x12] bits 27:24: vendor UpdateCommand stores SliceParam+0xae =
 * 512 / (64-pixel columns), at most 128; SliceParamToCmdRegsEnc1 packs
 * (value - 1) into four bits (3840 -> 7, 1920 -> 0, 1280 -> 8, 640 -> 2). */
static inline uint32_t t40_vendor_cmd12_hi(uint32_t width)
{
    uint32_t columns = (width + 63u) >> 6;
    uint32_t value = columns ? 512u / columns : 128u;

    if (value > 128u)
        value = 128u;
    return (value - 1u) & 0xfu;
}

/* Hardware RC targets in bits per HWRC group:
 *   cmd[0x15] = target bit/s * 1.9 * groups / LCUs for IDR, 5/7 of that
 *               for P (independent of frame rate and GOP);
 *   cmd[0x16] = maximum bit/s per frame * groups / LCUs (CBR: the target,
 *               VBR: uMaxBitRate). */
static inline void t40_vendor_hwrc_targets(uint64_t bitrate,
                                           uint64_t max_bitrate,
                                           uint32_t fps_num, uint32_t fps_den,
                                           uint32_t lcu_count, uint32_t groups,
                                           int is_idr, uint32_t *word15,
                                           uint32_t *word16)
{
    uint64_t lcus = lcu_count ? lcu_count : 1u;
    uint64_t num = fps_num ? fps_num : 25u;
    uint64_t den = fps_den ? fps_den : 1u;

    if (max_bitrate < bitrate)
        max_bitrate = bitrate;
    *word16 = (uint32_t)(max_bitrate * den * groups / (lcus * num));
    if (is_idr)
        *word15 = (uint32_t)(bitrate * groups * 19u / (lcus * 10u));
    else
        *word15 = (uint32_t)(bitrate * groups * 95u / (lcus * 70u));
}

/* iInitialQP -1: vendor AL_Common_Encoder_ComputeRCParam (0x302c0) takes
 * the first row of its table (0xd5bf4: AVC, +0x120: HEVC) whose threshold
 * reaches bit rate / frame rate * 1000 / pixels; QP = 40 - row.  Past the
 * last row the QP is the configured minimum. */
static inline int t40_vendor_initial_qp(uint32_t bitrate, uint32_t fps,
                                        uint64_t pixels, int hevc,
                                        int min_qp, int max_qp)
{
    static const uint16_t thresholds[2][29] = {
        { 33, 37, 41, 45, 51, 56, 62, 70, 76, 83, 94, 104, 114, 129, 140,
          156, 176, 194, 215, 241, 262, 289, 321, 349, 382, 423, 458, 504,
          551 },
        { 3, 8, 11, 14, 18, 23, 28, 32, 40, 48, 56, 64, 104, 114, 129, 140,
          156, 176, 194, 215, 241, 262, 289, 321, 349, 382, 423, 458, 504 },
    };
    uint64_t per_kpixel;
    int qp = min_qp;
    unsigned int i;

    if (!pixels || !fps)
        return 26;
    per_kpixel = (uint64_t)(bitrate / fps) * 1000u / pixels;
    for (i = 0; i < 29u; ++i) {
        if (thresholds[hevc ? 1 : 0][i] >= per_kpixel) {
            qp = 40 - (int)i;
            break;
        }
    }
    if (qp < min_qp)
        qp = min_qp;
    if (qp > max_qp)
        qp = max_qp;
    return qp;
}

/* cmd[0x17] bits 31:24, the hardware RC's lowest QP: the configured
 * minimum, but at most 32 below the maximum (vendor: 15..48 -> 16,
 * 20..44 -> 20). */
static inline uint32_t t40_vendor_hwrc_min_qp(uint32_t min_qp, uint32_t max_qp)
{
    if (max_qp > 32u && min_qp < max_qp - 32u)
        return max_qp - 32u;
    return min_qp;
}

/* imp_encoder.h: "GOPLength = uGopLength * uMaxSameSenceCnt"; the vendor
 * codes an IDR every uGopLength * uMaxSameSenceCnt pictures. */
static inline uint32_t t40_vendor_gop_length(uint32_t gop, uint32_t same_scene)
{
    if (same_scene > 1u)
        gop *= same_scene;
    return gop > 0xffffu ? 0xffffu : gop;
}

/* The channel rate as the vendor glue keeps it: reduced (c_reduce_fraction);
 * 0 parts give 0/0, which the GOP helpers below treat as "keep the GOP". */
static inline void t40_vendor_rate_reduce(uint32_t *num, uint32_t *den)
{
    uint32_t a = *num, b = *den, t;

    if (!a || !b) {
        *num = *den = 0u;
        return;
    }
    while (b) {
        t = a % b;
        a = b;
        b = t;
    }
    *num /= a;
    *den /= a;
}

/* T40 1.3.1 IMP glue (on_encoder_group_data_update, encoder_get_fps_mask,
 * first picture of a channel, #455): the GOP of CreateChn is kept in whole
 * seconds of the channel rate, rounded DOWN, and sent to the encoder with
 * the rate (GOP 25 at 20 fps -> 20, 50 -> 40; Eufy light-box pass
 * 2026-11-12: vendor IDR every 20 / 40 pictures).  GetChnGopAttr reads the
 * new length from then on.  Under one second the vendor computes GOP 0;
 * OpenIMP keeps the GOP then (as on T30, docs/BEYOND_VENDOR.md). */
static inline uint32_t t40_vendor_start_gop(uint32_t gop, uint32_t num,
                                            uint32_t den)
{
    uint32_t g;

    t40_vendor_rate_reduce(&num, &den);
    if (!num || !den)
        return gop;
    g = (uint32_t)((uint64_t)gop * den / num) * num / den;
    return g ? g : gop;
}

/* The same glue for a GOP set at run time (SetChnGopLength, or a new
 * length through SetChnGopAttr): whole seconds rounded UP. */
static inline uint32_t t40_vendor_set_gop(uint32_t gop, uint32_t num,
                                          uint32_t den)
{
    uint64_t frames;
    uint32_t secs, g;

    t40_vendor_rate_reduce(&num, &den);
    if (!num || !den)
        return gop;
    frames = (uint64_t)gop * den;
    secs = (uint32_t)(frames / num + (frames % num != 0u));
    g = (uint32_t)((uint64_t)secs * num / den);
    return g ? (g > 0xffffu ? 0xffffu : g) : gop;
}

/* T41 1.2.5 / 1.2.6 IMP glue (encoder_get_fps_mask, GOP part 0x1d5c-0x1fec,
 * #475): single-precision float, operation order as the vendor (div, mul,
 * div; volatile keeps the compiler from fusing or widening).  Vendor run on
 * the Vanhua T55A 2026-11-13 (audit/issue475): GOP 25 at 20 fps -> IDR every
 * 40, read-back 25, then 40 after the first picture; SetChnGopLength(25)
 * at run time -> 60 (read-back 60 after the next picture); 30 at 25 -> 50.
 *
 * GOP of the first picture (and of a rate change, old rate = given rate):
 * seconds are NOT floored, g = trunc(gop * old_den / old_num * num / den). */
static inline uint32_t t41_vendor_start_gop(uint32_t gop, uint32_t old_num,
                                            uint32_t old_den, uint32_t num,
                                            uint32_t den)
{
    volatile float a, b, c;
    uint32_t g;

    t40_vendor_rate_reduce(&old_num, &old_den);
    t40_vendor_rate_reduce(&num, &den);
    if (!old_num || !old_den || !num || !den)
        return gop;
    a = (float)(gop * old_den);
    b = a / (float)old_num;
    c = b * (float)num;
    c = c / (float)den;
    g = (uint32_t)c;
    return g ? (g > 0xffffu ? 0xffffu : g) : gop;
}

/* GOP set at run time (SetChnGopLength, a new length through SetChnGopAttr):
 * s = p * den / num, one full second added unless it is whole seconds,
 * g = trunc(s * num / den): 25 at 20 fps -> 45, 30 -> 50, 40 -> 40. */
static inline uint32_t t41_vendor_set_gop(uint32_t p, uint32_t num,
                                          uint32_t den)
{
    volatile float s, t;
    uint32_t frames, g;

    t40_vendor_rate_reduce(&num, &den);
    if (!num || !den)
        return p;
    frames = p * den;
    s = (float)frames;
    s = s / (float)num;
    if (frames % num)
        s = s + 1.0f;
    t = s * (float)num;
    t = t / (float)den;
    g = (uint32_t)t;
    return g ? (g > 0xffffu ? 0xffffu : g) : p;
}

/* AL_Codec_Encode_ValidateGopParam (lib_codec.c, every SetGopParam): the
 * IDR distance length * same-scene, at most 1000 - rate, rounded up to a
 * multiple of the reduced rate numerator; the GOP length becomes that
 * distance (I pictures only at the IDR). */
static inline uint32_t t40_vendor_idr_distance(uint32_t gop,
                                               uint32_t same_scene,
                                               uint32_t num, uint32_t den)
{
    uint32_t idr = t40_vendor_gop_length(gop, same_scene);

    t40_vendor_rate_reduce(&num, &den);
    if (!num || num >= 1000u || !idr)
        return idr;
    if (idr > 1000u - num)
        idr = 1000u - num;
    idr = (idr + num - 1u) / num * num;
    return idr > 0xffffu ? 0xffffu : idr;
}

#endif
