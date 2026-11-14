/*
 * T32/T33 encoder parameter calls (#480): the encoder side of openimp_t32_enc_e1.c.
 */
#ifndef OPENIMP_T32_ENC_E1_H
#define OPENIMP_T32_ENC_E1_H

#include <stddef.h>
#include <stdint.h>

#include "t32/openimp_t32_h264.h"

typedef struct {
    int codec;                          /* 0 JPEG, 1 H.264, 2 H.265 */
    int recv;                           /* StartRecvPic */
    uint32_t width, height, fps_num, fps_den;
    uint32_t ud_cnt, ud_size;           /* encAttr.userData */
} T32EncChnInfo;

/* openimp_t32_encoder.c: 0 for a created channel 0..15 (fills *ci), -1 else */
int OpenIMP_T32_EncChnQuery(int encChn, T32EncChnInfo *ci);

/* openimp_t32_enc_e1.c */
void OpenIMP_T32_E1Reset(int encChn, const T32EncChnInfo *ci);
int OpenIMP_T32_E1AtIdr(int encChn, T32Vui *vui);
size_t OpenIMP_T32_E1Sei(int encChn, int hevc, uint8_t *out, size_t cap);

#endif
