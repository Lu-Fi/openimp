/*
 * T33 P-skip threshold (#784), vendor libimp 2.0.2.1 encode loop 0xde45c..
 * 0xde4ac (H.264; H.265 0xdf510..): thr = (pskipThr / 100.0f + 1.0f) *
 * float(bit rate) in single precision (100.0f / 1.0f / 2^31 from the vendor
 * rodata 0x1cd4f8 / 0x1ad9c4 / 0x1bdbe8), the rate only in rcMode 1..5
 * (else 0), converted to unsigned (0xdecb4).
 */
#ifndef OPENIMP_T32_PSKIP_H
#define OPENIMP_T32_PSKIP_H

#include <stdint.h>

static inline uint32_t t32_pskip_thr(int thr, int rc_mode, uint32_t bitrate)
{
    float r = rc_mode >= 1 && rc_mode <= 5 ? (float)(int32_t)bitrate : 0.0f;
    volatile float f = ((float)thr / 100.0f + 1.0f) * r;

    if (f >= 2147483648.0f)
        return (uint32_t)(int32_t)(f - 2147483648.0f) | 0x80000000u;
    return (uint32_t)(int32_t)f;
}

#endif
