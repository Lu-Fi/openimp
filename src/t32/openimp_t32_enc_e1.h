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
int OpenIMP_T32_E1RoiTake(int encChn, uint8_t rec[16][9]);

/* what the next IDR's headers / pictures carry (#784): the crop window (SPS frame cropping /
 * conformance window) and the H.264 chroma QP offset (PPS, HERA) */
typedef struct {
    int chroma264;
    int chroma265;                      /* H.265: one offset for cb and cr (vendor) */
    uint8_t crop;
    uint32_t crop_x, crop_y, crop_w, crop_h;
} T32E1Hdr;
void OpenIMP_T32_E1Hdr(int encChn, T32E1Hdr *h);

/* The per-picture intra-refresh (GDR) fields the vendor 2.0.2.1 encode loop
 * (0xde3e8) copies from the channel (SetGDRCfg / RequestGDR) into the picture
 * parameters, which i264e_update_fenc (0x6ff80) puts at F+0x151..0x15c: gate
 * (always 1), request (taken: the channel flag is cleared), enable, request
 * frames (the last request's), frames and cycle (0 with GDR off). */
typedef struct {
    uint8_t gate, req, en;
    uint32_t req_frames, frames, cycle;
} T32E1Gdr;
void OpenIMP_T32_E1GdrTake(int encChn, T32E1Gdr *g);

#endif
