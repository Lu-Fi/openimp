/*
 * T33 intra refresh (GDR, #784) vs vendor libimp 2.0.2.1: the picture types of
 * i264e_decide_slice_type_and_rd (0x7062c) and the per-MB flag map the H.264
 * controller writes (EMC 0x90030).  Vendor evidence: QEMU runs vgc / v5gc
 * (audit t33-cache, HERA_CAP_MBMAP capture), CBR, 1920x1088, GOP 15,
 * SetGDRCfg {1, cycle 30, frames 5} at picture 2, RequestGDR(5) at picture 26:
 * no IDR after picture 0; pictures 26..30 carry 0x9000 in the MB rows 0..12,
 * 13..25, 26..39, 40..53, 54..67; every other picture has an empty map.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_gdr.h"
#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_rc33.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the slice-type rule alone */
static void types(void)
{
    uint32_t pos = 0;
    int32_t d = -1;
    int i, idr, n = 0;

    for (i = 1; i <= 45; i++)            /* no GDR: IDR every 15 */
        n += t32_gdr_slice_type(&pos, &d, 15, 0, 0) ? (pos = 0, d = -1, 1) : 0;
    CHECK(n == 3, "GOP 15, 45 pictures: %d IDRs", n);
    pos = 0, d = -1, n = 0;
    for (i = 1; i <= 100; i++)           /* GDR on: none */
        n += t32_gdr_slice_type(&pos, &d, 15, 0, 1);
    CHECK(n == 0 && pos == 100, "GDR on: %d IDRs, pos %u", n, pos);
    /* a request at picture 5 of a GOP: the next IDR 15 pictures after it */
    pos = 0, d = -1;
    for (i = 1; i < 5; i++)
        (void)t32_gdr_slice_type(&pos, &d, 15, 0, 0);
    CHECK(t32_gdr_slice_type(&pos, &d, 15, 1, 0) == 0 && d == 0, "request: P, d %d", d);
    for (i = 6, idr = 0; i <= 40 && !idr; i++)
        idr = t32_gdr_slice_type(&pos, &d, 15, 0, 0);
    CHECK(idr && i - 1 == 21, "IDR after the request at picture %d", i - 1);
    /* the request overrides a GOP boundary */
    pos = 13, d = -1;
    CHECK(t32_gdr_slice_type(&pos, &d, 15, 1, 0) == 0 && pos == 14, "request at the boundary");
}

static void maps(int hevc)
{
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t pos = 0, len, st[64] = { 0 };
    int32_t d = -1;
    int i, idr;
    static const int band[5][2] = { { 0, 12 }, { 13, 25 }, { 26, 39 }, { 40, 53 }, { 54, 67 } };

    memset(&m, 0, sizeof(m));
    memset(&rc, 0, sizeof(rc));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, hevc ? PT_H265 : PT_H264, 30, 2000);
    CHECK(t32_rc_init(&rc, &m, 1920, 1088, 0, 15, 15, 1, hevc) == 0 && rc.on, "rc init %d", hevc);
    if (!rc.on)
        return;
    for (i = 0; i < 50; i++) {
        int en = i >= 2, req = i == 26;

        idr = i == 0 ? 1 : t32_gdr_slice_type(&pos, &d, 15, req, en);
        if (idr)
            pos = 0, d = -1;
        CHECK(idr == (i == 0), "%s picture %d idr %d", hevc ? "H.265" : "H.264", i, idr);
        t32_rc_gdr(&rc, 1, (uint8_t)req, (uint8_t)en, 5, en ? 5 : 0, en ? 30 : 0);
        (void)t32_rc_start(&rc, idr, pos);
        if (!hevc) {
            const uint8_t *mp = t32_rc_map(&rc, T33_MAP_MB, &len);
            int r, c, bad = 0, set = 0;

            for (r = 0; mp && r < 68; r++)
                for (c = 0; c < 120; c++) {
                    uint16_t v = (uint16_t)(mp[2 * (r * 120 + c)] | mp[2 * (r * 120 + c) + 1] << 8);
                    int want = i >= 26 && i <= 30 && r >= band[i - 26][0] && r <= band[i - 26][1];

                    set += v != 0;
                    bad += want ? v != 0x9000 : v != 0;
                }
            CHECK(!bad && (set == 0) == !(i >= 26 && i <= 30), "H.264 picture %d: %d MBs differ from the vendor map (%d set)",
                  i, bad, set);
        }
        t32_rc_end(&rc, idr ? 60000u : 8000u, st, hevc ? 38 : 48);
    }
    t32_rc_free(&rc);
}

int main(void)
{
    types();
    maps(0);
    maps(1);
    if (fails)
        return 1;
    printf("t33 gdr784: ok\n");
    return 0;
}
