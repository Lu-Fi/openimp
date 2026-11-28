/*
 * T33 SetChnMapRoi (#989, #990, #99) vs vendor libimp 2.0.2.1: the map into the
 * controller (i264e_reconfig flag 4 after rcStart 0x818e4, H.265 ReconfigPFrame
 * 0x983ec before it) and the enables the HERA lists carry (H264E_T32V_SliceInit
 * 0x4b6c8 revision 7: s+0x159 -> 0x20038 bit 1, 0x90000 bit 6; H265E_T32V_SliceInit
 * 0x59298: s+0x1c8 / s+0x2ae).  Vendor evidence: QEMU T33 640x368, maproi at
 * picture 4 (audit issue99-gaps1128 gm264f / gm264c / gm265f2 / gm265c):
 * H.264 0x20038 0x73a1 -> 0x73a3, 0x90000 0x507 -> 0x547 from picture 5; H.265
 * 0x90000 0x37 -> 0x77 from the IDR at picture 4.  H.265 FIXQP keeps the
 * controller (#990).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_rc33.h"
#include "t32/openimp_t33_hera.h"
#include "t32/openimp_t33_hera_h265.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int find(const uint32_t *w, size_t pairs, uint32_t reg, uint32_t *v)
{
    for (size_t i = 0; i < pairs; i++)
        if ((w[2 * i + 1] & 0xffffcu) == (reg & 0xffffcu)) {
            *v = w[2 * i];
            return 1;
        }
    return 0;
}

static uint8_t map[40 * 23];

static void h264(void)
{
    static uint32_t out[T32_HERA_LIST_MAX_PAIRS * 2];
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t st[64] = { 0 };

    memset(&m, 0, sizeof(m));
    memset(&rc, 0, sizeof(rc));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, PT_H264, 15, 1000);
    CHECK(t32_rc_init(&rc, &m, 640, 368, 0, 15, 15, 1, 0) == 0 && rc.on, "H.264 rc init");
    if (!rc.on)
        return;
    for (int i = 0; i < 8; i++) {
        T33HeraH264Pic t;
        uint32_t a = 0, b = 0;
        size_t n;
        int qp = t32_rc_start(&rc, i == 0, (uint32_t)i), on = i >= 5;

        if (i == 4)                     /* the encoder: flag 4 after the start, for the next picture */
            t32_rc_map_roi(&rc, map, sizeof(map));
        memset(&t, 0, sizeof(t));
        t.pic.width = 640;
        t.pic.height = 368;
        t.pic.p_frame = i != 0;
        t.pic.qp = qp < 0 ? 30 : qp;
        t.pic.rc_slice = t32_rc_slice(&rc);
        n = t33_hera_h264_list(&t, out);
        CHECK(n && find(out, n, 0x20038u, &a) && find(out, n, 0x90000u, &b), "H.264 picture %d: no list", i);
        CHECK(((a >> 1) & 1u) == (uint32_t)on && ((b >> 6) & 1u) == (uint32_t)on,
              "H.264 picture %d: 0x20038 %#x 0x90000 %#x (map %s)", i, a, b, on ? "on" : "off");
        t32_rc_end(&rc, i == 0 ? 20000u : 3000u, st, 48);
    }
    t32_rc_free(&rc);
}

static void h265_fixqp(void)
{
    static uint32_t out[T32_HERA5_LIST_MAX_PAIRS * 2];
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t st[64] = { 0 };

    memset(&m, 0, sizeof(m));
    memset(&rc, 0, sizeof(rc));
    m.rcMode = IMP_ENC_RC_MODE_FIXQP;
    t32_rc_default_attr(&m, PT_H265, 15, 1000);
    CHECK(t32_rc_init(&rc, &m, 640, 368, 0, 15, 15, 1, 1) == 0 && rc.on, "H.265 FIXQP: no controller (#990)");
    if (!rc.on)
        return;
    for (int i = 0; i < 8; i++) {
        T33HeraH265Pic t;
        uint32_t a = 0, b = 0;
        size_t n;
        int qp, on = i >= 4;

        if (i == 4)                     /* I265e: ReconfigPFrame before rcStart */
            t32_rc_map_roi(&rc, map, sizeof(map));
        qp = t32_rc_start(&rc, i == 0 || i == 4, (uint32_t)(i % 4));
        memset(&t, 0, sizeof(t));
        t.pic.width = 640;
        t.pic.height = 368;
        t.pic.p_frame = i != 0 && i != 4;
        t.pic.after_p = i % 4 > 1;
        t.pic.qp = qp < 0 ? 30 : qp;
        t.rc_slice = t32_rc_slice(&rc);
        n = t33_hera_h265_list(&t, out);
        CHECK(n && find(out, n, 0x20038u, &a) && find(out, n, 0x90000u, &b), "H.265 picture %d: no list", i);
        CHECK(((a >> 1) & 1u) == (uint32_t)on && ((b >> 6) & 1u) == (uint32_t)on,
              "H.265 picture %d: 0x20038 %#x 0x90000 %#x (map %s)", i, a, b, on ? "on" : "off");
        t32_rc_end(&rc, i % 4 == 0 ? 20000u : 3000u, st, 48);
    }
    t32_rc_free(&rc);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(map); i++)
        map[i] = (i % 40) < 20 ? 0x2a : 0;
    h264();
    h265_fixqp();
    if (fails)
        return 1;
    printf("t33 maproi99: ok\n");
    return 0;
}
