/*
 * OpenIMP T32 H.265 parameter sets and slice headers (#294, planned).
 * See openimp_t32_h265.h.
 */
#include <string.h>

#include "t32/openimp_t32_h265.h"
#include "t32/openimp_t32_h264.h"
#include "t30/radix/t30_radix_hevc.h"

/* the T30 i265e channel defaults with vendor libimp 2.2.0's T32 values */
static void params(const T32H265Seq *s, T30HevcVps *vps, T30HevcSps *sps, T30HevcPps *pps)
{
    T30HevcChannel c;

    memset(&c, 0, sizeof(c));
    c.width = s->width;
    c.height = s->height;
    c.fps_num = s->fps_num;
    c.fps_den = s->fps_den;
    c.rc_mode = s->rc_mode;
    c.bitrate = s->bitrate;
    t30_hevc_channel_params(&c, vps, sps, pps);
    vps->extension = 0;
    vps->max_dec_pic_buffering = 2;
    sps->max_dec_pic_buffering = 2;
    sps->sao = 0;
    sps->hrd_flag_written = 0;          /* vui_hrd_parameters_present_flag 0 */
    memset(&sps->vui, 0, sizeof(sps->vui));
    sps->vui.num_units_in_tick = s->fps_den ? s->fps_den : 1u;
    sps->vui.time_scale = s->fps_num ? s->fps_num : 25u;
    sps->vui.video_format = 5;          /* written only with the signal type (#480) */
    sps->vui.colour_primaries = sps->vui.transfer_characteristics = sps->vui.matrix_coeffs = 2;
    if (s->vui.signal) {
        sps->vui.video_signal_type_present = 1;
        sps->vui.video_format = s->vui.format;
        sps->vui.video_full_range = s->vui.full ? 1 : 0;
    }
    if (s->vui.aspect) {
        sps->vui.aspect_ratio_info_present = 1;
        sps->vui.aspect_ratio_idc = s->vui.aspect_idc;
        sps->vui.sar_width = s->vui.sar_w;
        sps->vui.sar_height = s->vui.sar_h;
    }
    sps->vui.bitstream_restriction = s->vui.restriction ? 1 : 0;
    pps->cb_qp_offset = 0;
    pps->cr_qp_offset = 0;
}

static size_t nal(uint8_t *out, uint32_t type, T30HevcBits *b)
{
    if (b->overflow || !b->len)
        return 0;
    return t30_hevc_nal(out, T32_H265_PARAM_MAX, type, b->buf, b->len, 1);
}

size_t t32_h265_vps(const T32H265Seq *s, uint8_t *out)
{
    T30HevcVps vps;
    T30HevcSps sps;
    T30HevcPps pps;
    T30HevcBits b;
    uint8_t rbsp[48];

    params(s, &vps, &sps, &pps);
    t30_hevc_bits_init(&b, rbsp, sizeof(rbsp));
    t30_hevc_write_vps(&b, &vps);
    t30_hevc_trailing(&b);
    return nal(out, 32, &b);
}

size_t t32_h265_sps(const T32H265Seq *s, uint8_t *out)
{
    T30HevcVps vps;
    T30HevcSps sps;
    T30HevcPps pps;
    T30HevcBits b;
    uint8_t rbsp[56];

    params(s, &vps, &sps, &pps);
    t30_hevc_bits_init(&b, rbsp, sizeof(rbsp));
    t30_hevc_write_sps(&b, &sps);
    t30_hevc_trailing(&b);
    return nal(out, 33, &b);
}

size_t t32_h265_pps(const T32H265Seq *s, uint8_t *out)
{
    T30HevcVps vps;
    T30HevcSps sps;
    T30HevcPps pps;
    T30HevcBits b;
    uint8_t rbsp[16];

    params(s, &vps, &sps, &pps);
    t30_hevc_bits_init(&b, rbsp, sizeof(rbsp));
    t30_hevc_write_pps(&b, &pps);
    t30_hevc_trailing(&b);
    return nal(out, 34, &b);
}

/* vendor 2.2.0 (one slice): bit (poc + poc / 31) mod 32 of a fixed word */
int t32_h265_lfas(uint32_t poc)
{
    return (int)(0x5f4e4a53u >> ((poc + poc / 31u) & 31u) & 1u);
}

size_t t32_h265_slice_head(const T32H265Seq *s, const T32H265Slice *sl, uint8_t *out, int *esc)
{
    T30HevcVps vps;
    T30HevcSps sps;
    T30HevcPps pps;
    T30HevcSlice h;
    T30HevcBits b;
    uint8_t rbsp[24];
    uint32_t type = sl->idr ? 19u : 1u;

    params(s, &vps, &sps, &pps);
    memset(&h, 0, sizeof(h));
    h.nal_unit_type = type;
    h.slice_type = sl->idr ? 2u : 1u;
    h.poc = (int32_t)sl->poc;
    h.rps_idx = -1;
    if (!sl->idr) {
        h.num_negative = 1;
        h.delta_poc[0] = -1;
        h.used[0] = 1;
        h.num_ref_idx_l0 = 1;
    }
    h.max_num_merge_cand = 2;
    h.slice_qp = sl->qp;
    h.loop_filter_across_slices = (uint8_t)t32_h265_lfas(sl->poc);
    t30_hevc_bits_init(&b, rbsp, sizeof(rbsp));
    if (t30_hevc_write_slice_header(&b, &h, &sps, &pps) != 0)
        return 0;
    t30_hevc_trailing(&b);              /* byte_alignment() */
    if (b.overflow)
        return 0;
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    out[4] = (uint8_t)(type << 1);
    out[5] = 1;
    *esc = 0;
    return 6u + t32_h264_escape(out + 6, rbsp, b.len, esc);
}
