/*
 * OpenIMP T32 H.265 syntax around the HERA slice data (#294, planned).
 *
 * HERA writes a slice's CABAC data as an RBSP (from the first CTB to the
 * stop bit, no emulation prevention).  libimp writes everything else:
 * VPS, SPS, PPS, the slice segment header up to its byte_alignment(), the
 * NAL headers and 4-byte start codes, and escapes header and data together.
 * The bit writers are the T30 Radix ones (src/t30/radix/t30_radix_hevc.c);
 * the values are vendor libimp 2.2.0's on T32: Main, CTB 64 / CB 8, TB
 * 4..32 with no transform tree split, no SAO / AMP / TMVP / strong intra
 * smoothing, POC LSB 8 bits, DPB 2, no VPS extension, VUI timing
 * 1 / fps, cu_qp_delta depth 2, no chroma QP offsets, one reference (the
 * last picture), two merge candidates.  slice_loop_filter_across_slices
 * follows the vendor's fixed POC pattern (no effect with one slice).
 * Checked byte for byte against vendor streams by tests/t32/hera_list_test.c.
 */
#ifndef OPENIMP_T32_H265_H
#define OPENIMP_T32_H265_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width, height;             /* picture size (multiples of 8) */
    uint32_t fps_num, fps_den;
    uint32_t rc_mode;                   /* 0 FIXQP: no bit rate in the level */
    uint32_t bitrate;                   /* kbit/s */
} T32H265Seq;

typedef struct {
    int idr;                            /* IDR_W_RADL (I), else TRAIL_R (P) */
    uint32_t poc;                       /* pictures since the IDR */
    int qp;                             /* slice QP */
} T32H265Slice;

#define T32_H265_LOG2_MAX_POC_LSB 8u
#define T32_H265_PARAM_MAX        96u   /* bytes a parameter set NAL may take */

/* VPS / SPS / PPS NAL with a 4-byte start code into @out (>=
 * T32_H265_PARAM_MAX bytes): length, 0 on error */
size_t t32_h265_vps(const T32H265Seq *s, uint8_t *out);
size_t t32_h265_sps(const T32H265Seq *s, uint8_t *out);
size_t t32_h265_pps(const T32H265Seq *s, uint8_t *out);

/* start code, NAL header and the slice segment header up to its byte
 * alignment, escaped, into @out (>= 32 bytes): length.  @esc carries the
 * escaper's zero count on to the slice data (t32_h264_escape). */
size_t t32_h265_slice_head(const T32H265Seq *s, const T32H265Slice *sl, uint8_t *out, int *esc);

/* the vendor's slice_loop_filter_across_slices_enabled_flag for a POC */
int t32_h265_lfas(uint32_t poc);

#endif
