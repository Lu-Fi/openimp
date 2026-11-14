/*
 * OpenIMP T32 H.264 syntax around the HERA slice data (#293, planned).
 *
 * HERA writes a slice's CABAC data as an RBSP (from the first macroblock to
 * the stop bit, no emulation prevention).  libimp writes everything else:
 * SPS, PPS, the slice header up to its cabac_alignment_one_bits, the NAL
 * headers and start codes, and escapes header and data together.  The
 * parameter sets are vendor libimp 2.2.0's (High profile, CABAC, one
 * reference, pic_order_cnt_type 2, 8x8 transform with flat 4x4 and default
 * 8x8 scaling lists, VUI timing), checked byte for byte against vendor
 * streams by tests/t32/hera_list_test.c.
 */
#ifndef OPENIMP_T32_H264_H
#define OPENIMP_T32_H264_H

#include <stddef.h>
#include <stdint.h>

/* VUI fields an application can set (IMP_Encoder_SetH264Vui / SetH265Vui, #480): what vendor libimp 2.0.2.1
 * writes of them (QEMU -M ingenic-t33 streams): the video signal type (format, full range; no colour
 * description), the aspect ratio (H.264: from the SAR, H.265: as given) and the bitstream restriction;
 * all zero = the default parameter sets */
typedef struct {
    uint8_t signal, format, full;       /* video_signal_type_present, video_format, video_full_range */
    uint8_t restriction;                /* bitstream_restriction_flag */
    uint8_t aspect, aspect_idc;         /* H.265: aspect_ratio_info_present, aspect_ratio_idc */
    uint16_t sar_w, sar_h;              /* H.264: written when both are set; H.265: with idc 255 */
} T32Vui;

typedef struct {
    uint32_t width, height;             /* picture (cropped) size */
    uint32_t fps_num, fps_den;
    uint32_t bitrate;                   /* bits/s for the level, 0 = none */
    int init_qp;                        /* pic_init_qp */
    T32Vui vui;
} T32H264Seq;

typedef struct {
    int idr;                            /* IDR slice (I), else P */
    uint32_t frame_num;                 /* 0 on IDR */
    uint32_t idr_pic_id;
    int qp;                             /* slice QP */
} T32H264Slice;

#define T32_H264_LOG2_MAX_FRAME_NUM 10u

/* SPS / PPS NAL with a 4-byte start code into @out (>= 64 bytes): length */
size_t t32_h264_sps(const T32H264Seq *s, uint8_t *out);
size_t t32_h264_pps(const T32H264Seq *s, uint8_t *out);

/* start code, NAL header and the slice header up to its CABAC alignment,
 * escaped, into @out (>= 32 bytes): length.  @esc carries the escaper's
 * zero count on to the slice data. */
size_t t32_h264_slice_head(const T32H264Seq *s, const T32H264Slice *sl, uint8_t *out, int *esc);

/* copy RBSP bytes with emulation prevention: bytes written (<= 3/2 len + 1) */
size_t t32_h264_escape(uint8_t *dst, const uint8_t *src, size_t len, int *esc);

#endif
