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
void OpenIMP_T32_E1AutoVui(int encChn, int hevc);   /* IMP_Encoder_SetAutoVui, per frame (#930) */
size_t OpenIMP_T32_E1Sei(int encChn, int hevc, uint8_t *out, size_t cap);
int OpenIMP_T32_E1RoiTake(int encChn, uint8_t rec[16][9]);
#ifndef PLATFORM_T33
/* #977: 1 when SetChnMapRoi gave a new H.264 map not yet taken (@take: mark it taken);
 * @map / @size: the channel's map copy (NULL / 0: none) */
int OpenIMP_T32_E1MapTake(int encChn, int take, const uint8_t **map, uint32_t *size);
/* #977: the channel's SetChnQpgAI map (map NULL: none), 1 when not yet taken */
typedef struct {
    const uint8_t *map;
    uint32_t size, width, height;
    uint8_t fore, mark;
} T32E1QpgAI;
int OpenIMP_T32_E1QpgAITake(int encChn, int take, T32E1QpgAI *q);
#endif

/* what the next IDR's headers / pictures carry (#784): the crop window (SPS frame cropping /
 * conformance window) and the H.264 chroma QP offset (PPS, HERA) */
typedef struct {
    int chroma264;
    int chroma265;                      /* H.265: one offset for cb and cr (vendor) */
    uint8_t crop;
    uint32_t crop_x, crop_y, crop_w, crop_h;
    uint8_t motion_quality;             /* #784: H.265 SetMotionQuality enable, from this IDR on */
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

/* The per-picture P-skip fields of the vendor 2.0.2.1 encode loop (H.264
 * 0xde428, H.265 0xdf4cc): with SetPskipCfg on, thr = t32_pskip_thr()
 * (openimp_t32_pskip.h); en = 1, maxFrames; off: all 0.  req = RequestPskip (taken: the
 * channel flag is cleared; independent of en).  i264e_update_fenc puts them
 * at F+0x160 / 0x165 / 0x166 / 0x168, I265eFrame::recvPic at frame +0xf8 /
 * 0xfe / 0xff / 0x100. */
typedef struct {
    uint32_t thr, max_frames;
    uint8_t en, req;
} T32E1Pskip;
void OpenIMP_T32_E1PskipTake(int encChn, int rc_mode, uint32_t bitrate, T32E1Pskip *p);

/* RC parameters the vendor 2.0.2.1 encoder takes at the next IDR (#784 group 2):
 * SetChnRcTrigLevel (H.264, i264e_set_param 4 -> i264e_reconfig_rc_trig_set 0x83d94:
 * level / mode as given into P+0x140 / +0x144, flag 0x10) and SetSuperFrameCfg
 * (i264e / i265e set_param 0xd -> i264e_super_rc_set 0x82f24: mode, I / P / B
 * thresholds, rcPriority, maxReEncodeTimes into P+0x19c..0x1b0, flag 0x2000);
 * i264e_idr_reconfig (0x81b1c) restarts the controller (i264e_ratecontrol_init)
 * for every flag outside 0x1e0000.  qpg = the QpgMode word i264e_reconfig
 * (0x818e4, flag 0x8000) copies to P+0x194 before every picture; only
 * i264e_ratecontrol_init reads it.  Returns T32_E1RC_* flags (taken). */
/* T32_E1RC_RESTART (#784 batch 3): vendor 2.0.2.1 i264e/i265e_set_param of Color2Grey (H.264, H.265)
 * and HSkip (H.264) restarts the rate control at the next IDR; no HERA register changes (QEMU, FIXQP) */
enum { T32_E1RC_TRIG = 1, T32_E1RC_SF = 2, T32_E1RC_RESTART = 4 };
typedef struct {
    uint32_t trig[2];           /* P+0x140 level (raw word), +0x144 mode */
    uint32_t sf[6];             /* P+0x19c..0x1b0 */
    uint32_t qpg;               /* P+0x194 */
} T32E1Rc;
int OpenIMP_T32_E1RcTake(int encChn, int idr, T32E1Rc *r);
/* #788: Color2Grey active for the next picture (H.264 and H.265): 1 = chroma source is a grey plane */
int OpenIMP_T32_E1GreyChroma(int encChn);

#endif
