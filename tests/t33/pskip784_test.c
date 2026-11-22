/*
 * T33 P-skip (#784) vs vendor libimp 2.0.2.1: the encode loop's threshold
 * (0xde45c..0xde4ac) and what a request does to the H.264 controller's slice
 * block.  Vendor evidence: QEMU runs vps2 (vendor) / ops2 (OpenIMP), CBR
 * 1000 kbps, 1920x1088, GOP 15, SetPskipCfg {1, 2, 120} at picture 4,
 * RequestPskip at picture 8: only picture 8 differs from a plain P picture
 * (MCE 0x30048 not written, VMAU 0x40438 bit 0 clear).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_pskip.h"
#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_rc33.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void thr(void)
{
    CHECK(t32_pskip_thr(120, 1, 1000) == 2200, "120 %%, 1000: %u", t32_pskip_thr(120, 1, 1000));
    CHECK(t32_pskip_thr(0, 2, 4096) == 4096, "0 %%: %u", t32_pskip_thr(0, 2, 4096));
    CHECK(t32_pskip_thr(33, 5, 1000) == 1330, "33 %%: %u", t32_pskip_thr(33, 5, 1000));
    CHECK(t32_pskip_thr(120, 0, 1000) == 0, "FIXQP: %u", t32_pskip_thr(120, 0, 1000));
    CHECK(t32_pskip_thr(120, 6, 1000) == 0, "QVBR: %u", t32_pskip_thr(120, 6, 1000));
    /* at and beyond 2^31: the unsigned conversion (0xdecb4) */
    CHECK(t32_pskip_thr(100, 1, 0x40000000u) == 0x80000000u, "2^31: %#x",
          t32_pskip_thr(100, 1, 0x40000000u));
}

static void slices(void)
{
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t pos = 0, st[64] = { 0 };
    int i;

    memset(&m, 0, sizeof(m));
    memset(&rc, 0, sizeof(rc));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, PT_H264, 15, 1000);
    CHECK(t32_rc_init(&rc, &m, 1920, 1088, 0, 15, 15, 1, 0) == 0 && rc.on, "rc init");
    if (!rc.on)
        return;
    for (i = 0; i < 12; i++, pos++) {
        int idr = i == 0, en = i >= 4, req = i == 8;
        const uint8_t *sl;

        t32_rc_pskip(&rc, en ? t32_pskip_thr(120, 1, 1000) : 0, (uint8_t)en, (uint8_t)req, en ? 2 : 0);
        (void)t32_rc_start(&rc, idr, pos);
        sl = t32_rc_slice(&rc);
        if (getenv("PSKIP_DUMP"))
            printf("pic %d 3ec %u 42c %u 2c0 %u 2c4 %u\n", i, sl[0x3ec], sl[0x42c], sl[0x2c0], sl[0x2c4]);
        if (!idr)
            CHECK(sl[0x42c] == (req ? 0 : 1), "picture %d: slice +0x42c %u", i, sl[0x42c]);
        t32_rc_end(&rc, idr ? 60000u : 8000u, st, 48);
    }
    t32_rc_free(&rc);
}

int main(void)
{
    thr();
    slices();
    if (fails)
        return 1;
    printf("t33 pskip784: ok\n");
    return 0;
}
