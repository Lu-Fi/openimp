#ifndef OPENIMP_T31_VENDOR_HWRC_H
#define OPENIMP_T31_VENDOR_HWRC_H

/*
 * T31 vendor libimp 1.1.6 GOP validation and hardware-RC picture targets
 * (#604).  Pure integer helpers, host-tested in tests/t31/hwrc_target_test.c
 * against command words captured at the AVPU push of the vendor stack
 * (Cinnado T31L, sc2336, 1080p + 360p, CBR, 15 fps).
 *
 * Addresses are the vendor-libimp-t31-116.so symbol values (nm).
 */
#include <stdint.h>

/* Channel rate as the vendor builds the AL frame rate (0x7098c,
 * c_reduce_fraction): uFrameRate = reduced numerator (u16), uClkRatio =
 * reduced denominator * 1000 (u16). */
static inline void t31_vendor_al_rate(uint32_t num, uint32_t den,
                                      uint32_t *frame_rate,
                                      uint32_t *clk_ratio)
{
    uint32_t a = num, b = den, t;

    if (!a || !b) {
        *frame_rate = 0u;
        *clk_ratio = 0u;
        return;
    }
    while (b) {
        t = a % b;
        a = b;
        b = t;
    }
    *frame_rate = (num / a) & 0xffffu;
    *clk_ratio = ((den / a) * 1000u) & 0xffffu;
}

/*
 * IDR distance of a new channel.  The CreateChn glue (0x7098c) passes
 * uGopLength and freqIDR = uGopLength * uMaxSameSenceCnt (0 -> 1) as they
 * are (no whole-second step as T40 #455); AL_Codec_Encode_Create then runs
 * the GOP check at 0x77e30 (ValidateGopParam): rate = uFrameRate * 1000 /
 * uClkRatio, freqIDR = min(freqIDR, 1000 - rate) rounded UP to a multiple
 * of the rate, gopLength = min(gopLength, freqIDR) rounded up the same way.
 * Vendor capture: GOP 25 at 15 fps -> IDR every 30, GOP 50 -> 60.
 * Returns freqIDR (the IDR distance); 0 rate keeps the GOP.
 */
static inline uint32_t t31_vendor_idr_distance(uint32_t gop,
                                               uint32_t same_scene,
                                               uint32_t num, uint32_t den)
{
    uint32_t fr, clk, rate, freq;

    t31_vendor_al_rate(num, den, &fr, &clk);
    if (!clk)
        return gop;
    rate = fr * 1000u / clk;
    if (!rate || rate >= 1000u || !gop)
        return gop;
    freq = (same_scene ? same_scene : 1u) * gop;
    if (freq > 1000u - rate)
        freq = 1000u - rate;
    freq = (freq + rate - 1u) / rate * rate;
    return freq > 0xffffu ? 0xffffu : freq;
}

/*
 * GOP length a run-time setter leaves in effect (#804).  IMP_Encoder_
 * SetChnGopLength (0x88810) and SetChnGopAttr (0x87070) only store the
 * pending GOP attribute (channel +316, length at +320); the encoder thread
 * applies it before the next picture (glue 0x7efc8..0x7f0ec), with the
 * channel output rate num/den as stored (not reduced) and in float:
 *   new length:  s = len * den / num, plus 1.0 (0xde464) unless whole
 *                seconds, g = trunc(s * num / den) & 0xffff
 *   unchanged length (only the same-scene count changed):
 *                g = trunc((len * den / num) * num / den) & 0xffff
 * then AL_Codec_Encode_SetGopParam (0x7ad0c) with uGopLength = g and
 * freqIDR = g * uMaxSameSenceCnt (0 -> 1), which runs ValidateGopParam
 * (0x77e30): the IDR distance is t31_vendor_idr_distance(g, ...).
 * GOP 25 at 15 fps -> g 39 (float 39.999996) -> IDR every 45.
 */
static inline uint32_t t31_vendor_set_gop(uint32_t len, int changed,
                                          uint32_t num, uint32_t den)
{
    volatile float s, t;
    uint32_t frames;

    if (!num || !den)
        return len;
    frames = len * den;
    s = (float)frames;
    s = s / (float)num;
    if (changed && frames % num)
        s = s + 1.0f;
    t = s * (float)num;
    t = t / (float)den;
    return (uint32_t)t & 0xffffu;
}

/* AL_TRCParam.uCPBSize of an IMP channel: 270000 ticks of 90 kHz
 * (AL_Codec_Encode_SetDefaultParam 0x790b8, see avpu_t31_al_param). */
#define T31_VENDOR_CPB_TICKS 270000u

/*
 * GetTargetSize (0x59afc): bits of one picture for the hardware RC.
 *   B = uClkRatio * uTargetBitRate / (uFrameRate * 1000)
 *   GOP length < 2 (or low-delay mode 8, not used by IMP): B * 95 / 100
 *   P (type 1):  B * 85 / 100
 *   I (type 2):  min(B + (gop - 1) * 15 * B / 100,
 *                    uTargetBitRate * uCPBSize / 90000) * 95 / 100
 * gop is the validated GOP length (t31_vendor_idr_distance).
 */
static inline uint32_t t31_vendor_target_size(uint32_t bitrate,
                                              uint32_t num, uint32_t den,
                                              uint32_t gop, int is_idr)
{
    uint32_t fr, clk, base, grow, cap, v;

    t31_vendor_al_rate(num, den, &fr, &clk);
    if (!fr)
        return 0u;
    base = (uint32_t)((uint64_t)clk * bitrate / ((uint64_t)fr * 1000u));
    if ((gop & 0xffffu) < 2u)
        return (uint32_t)((uint64_t)base * 95u / 100u);
    if (!is_idr)
        return (uint32_t)((uint64_t)base * 85u / 100u);
    grow = (uint32_t)((uint64_t)(((gop & 0xffffu) - 1u) * 15u) * base / 100u);
    v = grow + base;
    cap = (uint32_t)((uint64_t)bitrate * T31_VENDOR_CPB_TICKS / 90000u);
    if (cap < v)
        v = cap;
    return (uint32_t)((uint64_t)v * 95u / 100u);
}

/*
 * cmd[0x15] (UpdateHwRateCtrlParam 0x5a740): the picture target spread over
 * the HWRC groups, groups * target / LCUs in 32-bit unsigned arithmetic as
 * the vendor (the column factor of the vendor's two-step divide cancels:
 * the group grid always divides the LCU columns).
 */
static inline uint32_t t31_vendor_hwrc_word15(uint32_t target,
                                              uint32_t groups,
                                              uint32_t lcus)
{
    if (!lcus)
        return 0u;
    return (groups * target) / lcus;
}

#endif
