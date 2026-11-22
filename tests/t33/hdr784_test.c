/*
 * T33 #784 host test: SetChnCrop and SetH264TransCfg in the headers and the HERA list, against vendor
 * libimp 2.0.2.1 (QEMU -M ingenic-t33: apitest t33e1 streams, audit runs t33-cache vfx / v265).
 *
 *   - H.264 SPS of a 640x360 channel without and with crop 16,8 320x180: vendor frame cropping
 *     8, 152, 4, 90 (chroma units of the 640x368 coded picture)
 *   - H.264 PPS without and with chroma_qp_index_offset 3
 *   - H.265 SPS conformance window of the same crop: 16, 304, 8, 172 luma samples (8, 152, 4, 86)
 *   - HERA H.264 list: EFE_FRM_QP bits 4:0 and VMAU 0x40424 bits 31:24 carry the offset (vendor
 *     0x33011e03 / 0x03000000 for 3 at QP 30)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_h264.h"
#include "t32/openimp_t32_h265.h"
#include "t32/openimp_t32_hera.h"
#include "t32/openimp_t33_hera.h"

static int fails;

static void hex(const uint8_t *b, size_t n, char *out)
{
    size_t i;

    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

/* compare a NAL (after its 4-byte start code) with the vendor's hex; a want ending in '*' is a prefix
 * (the H.264 SPS: up to the cropping fields, the vendor stream's VUI timing came from another rate) */
static void nal_eq(const char *what, const uint8_t *nal, size_t len, const char *want)
{
    size_t wl = strlen(want);
    int prefix = wl && want[wl - 1] == '*';

    char got[512];

    if (len < 4 || len - 4 > 200) {
        printf("FAIL %s: length %zu\n", what, len);
        fails++;
        return;
    }
    hex(nal + 4, len - 4, got);
    if (prefix ? strncmp(got, want, wl - 1) : strcmp(got, want)) {
        printf("FAIL %s:\n  got  %s\n  want %s\n", what, got, want);
        fails++;
    } else
        printf("ok   %s\n", what);
}

static uint32_t list_reg(const uint32_t *l, size_t n, uint32_t reg)
{
    size_t i;

    for (i = 0; i < n; i++)
        if ((l[2 * i + 1] & 0xffffcu) == reg)
            return l[2 * i];
    return 0xdeadbeefu;
}

int main(void)
{
    static uint32_t list[2 * T32_HERA_LIST_MAX_PAIRS];
    T32H264Seq s;
    T32H265Seq s5;
    uint8_t buf[256];
    size_t n;
    T33HeraH264Pic t;
    uint32_t v;

    /* the apitest t33e1 channel: 640x360 H.264 CBR, 15 fps */
    memset(&s, 0, sizeof(s));
    s.width = 640;
    s.height = 360;
    s.fps_num = 15;
    s.fps_den = 1;
    s.init_qp = 26;
    n = t32_h264_sps(&s, buf);
    nal_eq("H.264 SPS 640x360", buf, n, "67640016ac3b505017fcb*");
    s.crop = 1;
    s.crop_x = 16;
    s.crop_y = 8;
    s.crop_w = 320;
    s.crop_h = 180;
    n = t32_h264_sps(&s, buf);
    nal_eq("H.264 SPS crop 16,8 320x180", buf, n, "67640016ac3b505017e2404c940b7*");
    n = t32_h264_pps(&s, buf);
    nal_eq("H.264 PPS", buf, n, "68ee3ce1004242008430");
    s.chroma_qp_offset = 3;
    n = t32_h264_pps(&s, buf);
    nal_eq("H.264 PPS chroma_qp_index_offset 3", buf, n, "68ee334e100424200840d0");

    memset(&s5, 0, sizeof(s5));
    s5.width = 640;
    s5.height = 360;
    s5.fps_num = 15;
    s5.fps_den = 1;
    s5.rc_mode = 1;
    s5.bitrate = 1000;
    n = t32_h265_sps(&s5, buf);
    nal_eq("H.265 SPS 640x360", buf, n,
           "42010101600000030090000003000003003fa00502016965aa924c22010000030001000003000f08");
    s5.crop = 1;
    s5.crop_x = 16;
    s5.crop_y = 8;
    s5.crop_w = 320;
    s5.crop_h = 180;
    n = t32_h265_sps(&s5, buf);
    nal_eq("H.265 SPS crop 16,8 320x180", buf, n,
           "42010101600000030090000003000003003fa005020169890132502be5aa924c22010000030001000003000f08");

    s5.crop = 2;                        /* switched off again: the vendor's zero window */
    n = t32_h265_sps(&s5, buf);
    nal_eq("H.265 SPS after crop off", buf, n,
           "42010101600000030090000003000003003fa005020169fe5aa924c22010000003001000000300f080");

    /* HERA: a 640x368 I picture at QP 30 without and with the offset */
    memset(&t, 0, sizeof(t));
    t.pic.width = 640;
    t.pic.height = 368;
    t.pic.qp = 30;
    t.pic.src_stride = 640;
    n = t33_hera_h264_list(&t, list);
    v = list_reg(list, n, 0x2001cu);
    if (!n || (v & 0x1fu) != 0 || list_reg(list, n, 0x40424u) != 0) {
        printf("FAIL HERA offset 0: EFE_FRM_QP %08x VMAU 40424 %08x\n", v, list_reg(list, n, 0x40424u));
        fails++;
    } else
        printf("ok   HERA offset 0: EFE_FRM_QP %08x\n", v);
    t.pic.chroma_qp_offset = 3;
    n = t33_hera_h264_list(&t, list);
    v = list_reg(list, n, 0x2001cu);
    if (!n || (v & 0x1fu) != 3u || (v >> 8 & 0xffu) != 30u || list_reg(list, n, 0x40424u) != 0x03000000u) {
        printf("FAIL HERA offset 3: EFE_FRM_QP %08x VMAU 40424 %08x\n", v, list_reg(list, n, 0x40424u));
        fails++;
    } else
        printf("ok   HERA offset 3: EFE_FRM_QP %08x VMAU 40424 %08x (vendor x3 / 03000000)\n", v,
               list_reg(list, n, 0x40424u));
    t.pic.chroma_qp_offset = -2;
    n = t33_hera_h264_list(&t, list);
    v = list_reg(list, n, 0x2001cu);
    if (!n || (v & 0x1fu) != 0x1eu) {
        printf("FAIL HERA offset -2: EFE_FRM_QP %08x\n", v);
        fails++;
    } else
        printf("ok   HERA offset -2: EFE_FRM_QP %08x (5-bit two's complement)\n", v);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
