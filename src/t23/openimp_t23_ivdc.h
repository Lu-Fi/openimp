/* T23 IVDC direct mode (ISP frame channel 0 -> Helix line by line), as the
 * vendor libimp T23 1.3.0.  openimp #767. */
#ifndef OPENIMP_T23_IVDC_H
#define OPENIMP_T23_IVDC_H

#include <stdint.h>

/* /dev/misc-ivdc ioctls (tx-isp-t23 ivdc_misc_unlocked_ioctl) */
#define T23_IVDC_GET_DIRECT   0xc004543fu
#define T23_IVDC_SET_PADDR    0xc0045441u
#define T23_IVDC_GET_MEM_LINE 0xc0045442u

/* Helix source of an IVDC frame: luma, chroma = luma | 0x1000000 */
#define T23_IVDC_SRC_Y        0x80000000u
#define T23_IVDC_SRC_UV_BIT   0x01000000u
#define T23_IVDC_FRAME_TYPE   3u
#define T23_IVDC_THRESHOLD    300

/* Private words of an IVDC frame record, the vendor frame offsets
 * (get_frame @0x94744: +0x3f0 overflow count, +0x3f4 threshold,
 * +0x3f8 frame type, +0x3f9 sensor). */
#define T23_FRAME_OVERFLOW    0x3f0
#define T23_FRAME_THRESHOLD   0x3f4
#define T23_FRAME_TYPE        0x3f8
#define T23_FRAME_SENSOR      0x3f9

/* FrameSource side */
int  OpenIMP_T23_IvdcCreateChn(int chn);
void OpenIMP_T23_IvdcDestroyChn(int chn);
int  OpenIMP_T23_IvdcEnableChn(int chn, int width, int height);
void OpenIMP_T23_IvdcDisableChn(int chn);
/* 1 when channel chn delivers IVDC frames (no frame memory) */
int  OpenIMP_T23_IvdcChnDirect(int chn);
int  OpenIMP_T23_IvdcThreshold(int chn);
int  OpenIMP_T23_IvdcSetThreshold(int chn, int threshold);
uint32_t OpenIMP_T23_IvdcMemLine(void);
/* direct mode of the last GET_DIRECT (0: normal capture or no IVDC) */
int  OpenIMP_T23_IvdcMode(void);

/* Encoder side: 1 if the frame record is an IVDC frame */
int  OpenIMP_T23_IvdcFrame(const void *frame);
/* 0 = encode, -1 = drop (ring overflow or the ISP too far ahead); the
 * channel node values the kernel checks are returned on success. */
int  OpenIMP_T23_IvdcCheck(const void *frame, uint32_t *overflow_cnt,
                           uint32_t *mem_line, uint32_t *threshold,
                           uint64_t *time_ms);

/* host test hook: register read override (NULL = /dev/mem mapping) */
void OpenIMP_T23_IvdcSetRegReader(int (*fn)(uint32_t addr, uint32_t *val));
/* lead of the ISP write line over the VPU read line, wrap corrected */
uint32_t OpenIMP_T23_IvdcLead(uint32_t isp_line_reg, uint32_t vpu_line_reg,
                              uint32_t mem_line);

#endif
