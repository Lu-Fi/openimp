/*
 * OpenIMP T32 H.264 parameter sets and slice headers (#293, planned).
 * See openimp_t32_h264.h.
 */
#include <string.h>

#include "t32/openimp_t32_h264.h"
#include "t30/t30_h264_level.h"

typedef struct {
    uint8_t buf[64];
    unsigned bits;
} T32Bits;

static void put(T32Bits *b, unsigned n, uint32_t v)
{
    while (n--) {
        if (b->bits / 8u >= sizeof(b->buf))
            return;
        if (v >> n & 1u)
            b->buf[b->bits / 8u] |= (uint8_t)(0x80u >> (b->bits % 8u));
        b->bits++;
    }
}

static void put_ue(T32Bits *b, uint32_t v)
{
    unsigned n = 0;
    uint32_t x = v + 1u;

    while (x >> (n + 1u))
        n++;
    put(b, n, 0);
    put(b, n + 1u, x);
}

static void put_se(T32Bits *b, int v)
{
    put_ue(b, v <= 0 ? (uint32_t)(-2 * v) : (uint32_t)(2 * v - 1));
}

static void trailing(T32Bits *b)
{
    put(b, 1, 1);
    while (b->bits % 8u)
        put(b, 1, 0);
}

size_t t32_h264_escape(uint8_t *dst, const uint8_t *src, size_t len, int *esc)
{
    size_t i, n = 0;
    int zeros = *esc;

    for (i = 0; i < len; i++) {
        if (zeros >= 2 && src[i] <= 3) {
            dst[n++] = 3;
            zeros = 0;
        }
        dst[n++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    *esc = zeros;
    return n;
}

static size_t nal(uint8_t *out, unsigned ref_idc, unsigned type, const T32Bits *b, int *esc)
{
    int zeros = 0;

    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    out[4] = (uint8_t)(ref_idc << 5 | type);
    return 5u + t32_h264_escape(out + 5, b->buf, b->bits / 8u, esc ? esc : &zeros);
}

/* aspect_ratio_info: none without a SAR; the table index of the reduced SAR (H.264 Table E-1), else 255
 * (Extended_SAR) and the SAR itself */
static void put_aspect(T32Bits *b, uint32_t w, uint32_t h)
{
    static const uint8_t tab[16][2] = { { 1, 1 }, { 12, 11 }, { 10, 11 }, { 16, 11 }, { 40, 33 }, { 24, 11 }, { 20, 11 },
                                        { 32, 11 }, { 80, 33 }, { 18, 11 }, { 15, 11 }, { 64, 33 }, { 160, 99 }, { 4, 3 },
                                        { 3, 2 }, { 2, 1 } };
    uint32_t x = w, y = h, t, i;

    if (!w || !h) {
        put(b, 1, 0);
        return;
    }
    while (y) {
        t = x % y;
        x = y;
        y = t;
    }
    w /= x;
    h /= x;
    put(b, 1, 1);
    for (i = 0; i < 16; i++)
        if (tab[i][0] == w && tab[i][1] == h) {
            put(b, 8, i + 1u);
            return;
        }
    put(b, 8, 255);
    put(b, 16, w);
    put(b, 16, h);
}

size_t t32_h264_sps(const T32H264Seq *s, uint8_t *out)
{
    T32Bits b;
    uint32_t w16 = (s->width + 15u) & ~15u, h16 = (s->height + 15u) & ~15u;
    int crop = w16 != s->width || h16 != s->height;

    memset(&b, 0, sizeof(b));
    put(&b, 8, 100);                    /* High */
    put(&b, 8, 0);                      /* constraint flags, reserved */
    put(&b, 8, t30_h264_level(s->width, s->height, s->fps_num, s->fps_den, s->bitrate, 1));
    put_ue(&b, 0);                      /* sps id */
    put_ue(&b, 1);                      /* 4:2:0 */
    put_ue(&b, 0);
    put_ue(&b, 0);                      /* 8 bit */
    put(&b, 1, 0);                      /* no transform bypass */
    put(&b, 1, 0);                      /* no SPS scaling matrix */
    put_ue(&b, T32_H264_LOG2_MAX_FRAME_NUM - 4u);
    put_ue(&b, 2);                      /* pic_order_cnt_type 2 */
    put_ue(&b, 1);                      /* one reference */
    put(&b, 1, 1);                      /* gaps in frame_num allowed */
    put_ue(&b, w16 / 16u - 1u);
    put_ue(&b, h16 / 16u - 1u);
    put(&b, 1, 1);                      /* frame_mbs_only */
    put(&b, 1, 1);                      /* direct_8x8_inference */
    put(&b, 1, (uint32_t)crop);
    if (crop) {
        put_ue(&b, 0);
        put_ue(&b, (w16 - s->width) / 2u);
        put_ue(&b, 0);
        put_ue(&b, (h16 - s->height) / 2u);
    }
    put(&b, 1, 1);                      /* VUI */
    put_aspect(&b, s->vui.sar_w, s->vui.sar_h);
    put(&b, 1, 0);                      /* overscan */
    put(&b, 1, s->vui.signal);
    if (s->vui.signal) {
        put(&b, 3, s->vui.format);
        put(&b, 1, s->vui.full ? 1u : 0u);
        put(&b, 1, 0);                  /* no colour description (the vendor does not pass it on) */
    }
    put(&b, 1, 0);                      /* chroma loc */
    put(&b, 1, 1);                      /* timing */
    put(&b, 32, s->fps_den ? s->fps_den : 1u);
    put(&b, 32, 2u * (s->fps_num ? s->fps_num : 25u));
    put(&b, 1, 1);                      /* fixed frame rate */
    put(&b, 3, 0);                      /* no HRD, no pic_struct */
    put(&b, 1, s->vui.restriction ? 1u : 0u);
    if (s->vui.restriction) {           /* vendor 2.0.2.1 values */
        put(&b, 1, 1);                  /* motion_vectors_over_pic_boundaries */
        put_ue(&b, 0);                  /* max_bytes_per_pic_denom */
        put_ue(&b, 0);                  /* max_bits_per_mb_denom */
        put_ue(&b, 11);                 /* log2_max_mv_length_horizontal */
        put_ue(&b, 11);                 /* log2_max_mv_length_vertical */
        put_ue(&b, 0);                  /* max_num_reorder_frames */
        put_ue(&b, 1);                  /* max_dec_frame_buffering */
    }
    trailing(&b);
    return nal(out, 3, 7, &b, NULL);
}

size_t t32_h264_pps(const T32H264Seq *s, uint8_t *out)
{
    T32Bits b;
    int i;

    memset(&b, 0, sizeof(b));
    put_ue(&b, 0);                      /* pps id */
    put_ue(&b, 0);                      /* sps id */
    put(&b, 1, 1);                      /* CABAC */
    put(&b, 1, 0);
    put_ue(&b, 0);                      /* one slice group */
    put_ue(&b, 0);
    put_ue(&b, 0);                      /* one reference each list */
    put(&b, 3, 0);                      /* no weighted prediction */
    put_se(&b, s->init_qp - 26);
    put_se(&b, 0);
    put_se(&b, 0);                      /* chroma QP offset */
    put(&b, 1, 1);                      /* deblocking control present */
    put(&b, 2, 0);
    put(&b, 1, 1);                      /* transform_8x8_mode */
    put(&b, 1, 1);                      /* pic scaling matrix */
    for (i = 0; i < 8; i++) {           /* 4x4 Y intra and inter: flat 16; 8x8: default */
        int flat = i == 0 || i == 3;
        put(&b, 1, (uint32_t)flat);
        if (flat) {
            put_se(&b, 8);              /* 8 + 8 = 16 */
            put_se(&b, -16);            /* next scale 0: the rest repeats 16 */
        }
    }
    put_se(&b, 0);                      /* second chroma QP offset */
    trailing(&b);
    return nal(out, 3, 8, &b, NULL);
}

size_t t32_h264_slice_head(const T32H264Seq *s, const T32H264Slice *sl, uint8_t *out, int *esc)
{
    T32Bits b;

    memset(&b, 0, sizeof(b));
    put_ue(&b, 0);                      /* first_mb_in_slice */
    put_ue(&b, sl->idr ? 2u : 0u);      /* I, P */
    put_ue(&b, 0);                      /* pps id */
    put(&b, T32_H264_LOG2_MAX_FRAME_NUM, sl->frame_num);
    if (sl->idr) {
        put_ue(&b, sl->idr_pic_id);
        put(&b, 2, 0);                  /* no_output_of_prior_pics, long_term_reference */
    } else {
        put(&b, 1, 0);                  /* num_ref_idx_active_override */
        put(&b, 1, 0);                  /* ref_pic_list_modification */
        put(&b, 1, 0);                  /* adaptive_ref_pic_marking */
        put_ue(&b, 0);                  /* cabac_init_idc */
    }
    put_se(&b, sl->qp - s->init_qp);
    put_ue(&b, 0);                      /* deblocking on */
    put_se(&b, 0);
    put_se(&b, 0);
    while (b.bits % 8u)                 /* cabac_alignment_one_bit */
        put(&b, 1, 1);
    *esc = 0;
    return nal(out, sl->idr ? 3u : 2u, sl->idr ? 5u : 1u, &b, esc);
}
