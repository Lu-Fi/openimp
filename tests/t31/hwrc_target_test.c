/*
 * T31 hardware-RC picture targets and IDR distance versus the vendor libimp
 * 1.1.6 (#604, host test, no camera).
 *
 * Every expected value is a word of a vendor command list captured at the
 * AVPU push (cmdcap31.so) on a Cinnado T31L (sc2336), CBR, encoder 15 fps,
 * min/max QP 20/45, 2026-11-18/20 (audit/issue604/t31/vendor_cbr1000_*.bin):
 *   ch0 1920x1080 1000 kbit GOP 25, ch1 640x360 384 kbit GOP 50;
 * plus the older 640x360 25 fps 1 Mbit GOP 50 and 1080p 25 fps 3 Mbit
 * oracles kept in codec-t40.c.  The fixed IDR factor the open stack used
 * before (7.93 budgets = GOP 50) gave 0x205 / 0x50f / 0x44f for the first
 * three IDR words.
 */
#include <stdint.h>
#include <stdio.h>

#include "t40/t31_vendor_hwrc.h"

static int failures;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got 0x%x want 0x%x\n", what, got, want);
        ++failures;
    }
}

/* cmd[0x15] of one channel, as codec-t40.c builds it. */
static uint32_t word15(uint32_t kbit, uint32_t fps, uint32_t gop,
                       uint32_t groups, uint32_t lcus, int is_idr)
{
    uint32_t g = t31_vendor_idr_distance(gop, 1u, fps, 1u);

    return t31_vendor_hwrc_word15(
        t31_vendor_target_size(kbit * 1000u, fps, 1u, g, is_idr),
        groups, lcus);
}

int main(void)
{
    /* ValidateGopParam: whole seconds of the rate, rounded up */
    expect("gop 25 @15", t31_vendor_idr_distance(25u, 1u, 15u, 1u), 30u);
    expect("gop 50 @15", t31_vendor_idr_distance(50u, 1u, 15u, 1u), 60u);
    expect("gop 50 @25", t31_vendor_idr_distance(50u, 1u, 25u, 1u), 50u);
    expect("gop 25 @30", t31_vendor_idr_distance(25u, 1u, 30u, 1u), 30u);
    expect("gop 25 same 2 @25",
           t31_vendor_idr_distance(25u, 2u, 25u, 1u), 50u);
    expect("gop 999 @15 (1000 - rate)",
           t31_vendor_idr_distance(999u, 1u, 15u, 1u), 990u);
    expect("gop 7 @0 kept", t31_vendor_idr_distance(7u, 1u, 0u, 1u), 7u);

    /* #804 run-time GOP (encoder-thread glue 0x7efc8, float, one second
     * added unless whole seconds), then ValidateGopParam */
    expect("set 25 @15 (float: 39.999996)",
           t31_vendor_set_gop(25u, 1, 15u, 1u), 39u);
    expect("set 25 @15 idr", t31_vendor_idr_distance(
        t31_vendor_set_gop(25u, 1, 15u, 1u), 1u, 15u, 1u), 45u);
    expect("set 30 @15 (whole)", t31_vendor_set_gop(30u, 1, 15u, 1u), 30u);
    expect("set 50 @15 (float: 64.99999)",
           t31_vendor_set_gop(50u, 1, 15u, 1u), 64u);
    expect("set 50 @15 idr", t31_vendor_idr_distance(
        t31_vendor_set_gop(50u, 1, 15u, 1u), 1u, 15u, 1u), 75u);
    expect("set 15 @25", t31_vendor_set_gop(15u, 1, 25u, 1u), 40u);
    expect("set 15 @25 idr", t31_vendor_idr_distance(
        t31_vendor_set_gop(15u, 1, 25u, 1u), 1u, 25u, 1u), 50u);
    expect("set 50 @25 (whole)", t31_vendor_set_gop(50u, 1, 25u, 1u), 50u);
    expect("set 25 @30000/1001", t31_vendor_set_gop(25u, 1, 30000u, 1001u),
           54u);
    expect("unchanged 40 @15", t31_vendor_set_gop(40u, 0, 15u, 1u), 40u);
    expect("unchanged 40 @15 same 2 idr", t31_vendor_idr_distance(
        t31_vendor_set_gop(40u, 0, 15u, 1u), 2u, 15u, 1u), 90u);
    expect("set 7 @0 kept", t31_vendor_set_gop(7u, 1, 0u, 1u), 7u);

    /* H.264 1080p (8 groups, 120x68 MB) 1000 kbit GOP 25 @ 15 */
    expect("avc 1080p IDR", word15(1000u, 15u, 25u, 8u, 8160u, 1), 0x14cu);
    expect("avc 1080p P", word15(1000u, 15u, 25u, 8u, 8160u, 0), 0x37u);
    expect("avc 1080p cmd18 low",
           word15(1000u, 15u, 25u, 8u, 8160u, 1) >> 7, 2u);
    /* H.265 1080p (5 groups, 60x34 32x32 blocks) */
    expect("hevc 1080p IDR", word15(1000u, 15u, 25u, 5u, 2040u, 1), 0x33eu);
    expect("hevc 1080p P", word15(1000u, 15u, 25u, 5u, 2040u, 0), 0x8au);
    expect("hevc 1080p cmd18 low",
           word15(1000u, 15u, 25u, 5u, 2040u, 1) >> 7, 6u);
    /* 360p sub stream (5 groups, 40x23 MB) 384 kbit GOP 50 @ 15 */
    expect("avc 360p IDR", word15(384u, 15u, 50u, 5u, 920u, 1), 0x515u);
    expect("avc 360p P", word15(384u, 15u, 50u, 5u, 920u, 0), 0x76u);
    expect("avc 360p cmd18 low",
           word15(384u, 15u, 50u, 5u, 920u, 1) >> 7, 0xau);
    /* older oracles: 360p 25 fps 1 Mbit GOP 50, 1080p 25 fps 3 Mbit P */
    expect("avc 360p 25fps IDR", word15(1000u, 25u, 50u, 5u, 920u, 1), 0x6bcu);
    expect("avc 360p 25fps P", word15(1000u, 25u, 50u, 5u, 920u, 0), 0xb8u);
    expect("avc 1080p 3M P", word15(3000u, 25u, 50u, 8u, 8160u, 0), 0x64u);

    /* GetTargetSize corner cases: GOP 1 (all IDR) and the CPB cap */
    expect("gop 1", t31_vendor_target_size(1000000u, 25u, 1u, 1u, 1), 38000u);
    expect("cpb cap", t31_vendor_target_size(1000000u, 5u, 1u, 995u, 1),
           2850000u);

    if (failures) {
        printf("t31 hwrc target test: %d failure(s)\n", failures);
        return 1;
    }
    printf("t31 hwrc target test: ok\n");
    return 0;
}
