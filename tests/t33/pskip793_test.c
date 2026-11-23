/*
 * T33 H.265 P-skip (#793) vs vendor libimp 2.0.2.1: the H.265 controller's
 * skipped picture and the register bits H265E_T32V_SliceInit (0x59298,
 * revision 7) packs from its slice block.  Vendor evidence: QEMU v5pss,
 * CBR 1000 kbps, 640x368, GOP 15, SetPskipCfg {1, 2, 120} at picture 4,
 * RequestPskip at picture 8: picture 8 differs from a plain P picture in
 * eight registers only (bits below), the QPs stay those of the plain run.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_pskip.h"
#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_rc33.h"
#include "t32/openimp_t33_hera_h265.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* register, the bits the skipped picture moves, vendor value (masked) on a plain P / the skipped picture */
static const struct { uint32_t reg, mask, plain, skip; } bits[] = {
    { 0x20020u, 0xffffff00u, 0x00100c00u, 0x07ff1000u },
    { 0x20304u, 0x000000f0u, 0x00000050u, 0x000000f0u },
    { 0x30000u, 0x00010000u, 0x00000000u, 0x00010000u },
    { 0x40020u, 0x00000003u, 0x00000000u, 0x00000003u },
    { 0x40164u, 0x00080000u, 0x00080000u, 0x00000000u },
    { 0x40234u, 0xe0000000u, 0xe0000000u, 0x00000000u },
    { 0x40240u, 0x80000000u, 0x80000000u, 0x00000000u },
    { 0x40284u, 0x00040001u, 0x00040001u, 0x00000000u },
};

static int find(const uint32_t *w, size_t pairs, uint32_t reg, uint32_t *v)
{
    for (size_t i = 0; i < pairs; i++)
        if ((w[2 * i + 1] & 0xffffcu) == (reg & 0xffffcu)) {
            *v = w[2 * i];
            return 1;
        }
    return 0;
}

static void list_bits(int pic, const uint8_t *sl, int qp, int skip)
{
    static uint32_t out[T32_HERA5_LIST_MAX_PAIRS * 2];
    T33HeraH265Pic t;
    size_t n;

    memset(&t, 0, sizeof(t));
    t.pic.width = 640;
    t.pic.height = 368;
    t.pic.p_frame = 1;
    t.pic.after_p = pic > 1;
    t.pic.qp = qp;
    t.rc_slice = sl;
    n = t33_hera_h265_list(&t, out);
    CHECK(n > 0, "picture %d: no list", pic);
    for (size_t i = 0; n && i < sizeof(bits) / sizeof(bits[0]); i++) {
        uint32_t v = 0, want = skip ? bits[i].skip : bits[i].plain;

        CHECK(find(out, n, bits[i].reg, &v), "picture %d: no %#x", pic, bits[i].reg);
        CHECK((v & bits[i].mask) == want, "picture %d: %#x = %#x, vendor %#x (mask %#x)", pic, bits[i].reg,
              v & bits[i].mask, want, bits[i].mask);
    }
}

int main(void)
{
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t st[64] = { 0 };
    int i;

    memset(&m, 0, sizeof(m));
    memset(&rc, 0, sizeof(rc));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, PT_H265, 15, 1000);
    CHECK(t32_rc_init(&rc, &m, 640, 368, 0, 15, 15, 1, 1) == 0 && rc.on, "rc init");
    if (!rc.on)
        return 1;
    for (i = 0; i < 12; i++) {
        int idr = i == 0, en = i >= 4, req = i == 8, qp;
        const uint8_t *sl;

        t32_rc_pskip(&rc, en ? t32_pskip_thr(120, 1, 1000) : 0, (uint8_t)en, (uint8_t)req, en ? 2 : 0);
        qp = t32_rc_start(&rc, idr, (uint32_t)i);
        sl = t32_rc_slice(&rc);
        CHECK(sl && sl[0xdbc] == 7u, "picture %d: no revision 7 slice block", i);
        if (!sl)
            break;
        if (getenv("PSKIP_DUMP"))
            printf("pic %d qp %d 4b0 %u 4b1 %u 4b3 %u 4b5 %u 513 %u e0 %u\n", i, qp, sl[0x4b0], sl[0x4b1],
                   sl[0x4b3], sl[0x4b5], sl[0x513], sl[0xe0] | sl[0xe1] << 8);
        if (!idr)
            list_bits(i, sl, qp, req);
        t32_rc_end(&rc, idr ? 20000u : 3000u, st, 48);
    }
    t32_rc_free(&rc);
    if (fails)
        return 1;
    printf("t33 pskip793: ok\n");
    return 0;
}
