/*
 * T33 RC parameters (#784 group 2) vs vendor libimp 2.0.2.1: SetChnRcTrigLevel
 * and SetSuperFrameCfg restart the H.264 controller at the next IDR
 * (i264e_idr_reconfig 0x81b1c calls i264e_ratecontrol_init for flags 0x10 /
 * 0x2000); QpgMode (P+0x194) is read by that init.  Vendor evidence: QEMU
 * runs vtr / vsf (CBR) and vvt (VBR) restart the QP sequence at the IDR of
 * picture 4 (45 42 39 36 45 ...), vb4 (RequestIDR only) does not.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_rc33.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* 10 pictures, IDR at 0 and 4; @flags at picture 4 as the encoder glue passes them */
static void run(unsigned flags, int *qps, T32Rc *rc)
{
    IMPEncoderAttrRcMode m;
    uint32_t st[64] = { 0 }, trig[2] = { 2, 1 }, sf[6] = { 2, 20000, 10000, 10000, 1, 2 };

    memset(&m, 0, sizeof(m));
    memset(rc, 0, sizeof(*rc));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, PT_H264, 15, 1000);
    CHECK(t32_rc_init(rc, &m, 1920, 1088, 0, 30, 15, 1, 0) == 0 && rc->on, "rc init");
    for (int i = 0, pos = 0; i < 10 && rc->on; i++, pos++) {
        int idr = i == 0 || i == 4;

        if (i == 4)
            pos = 0;
        if (t32_rc_idr_params(rc, i == 4 ? flags : 0u, trig, sf, i >= 2 ? 3u : 0u) && idr)
            CHECK(t32_rc_reconfig(rc, &m, 1920, 1088, 0, 30, 15, 1, 0) == 0, "reconfig");
        qps[i] = t32_rc_start(rc, idr, (uint32_t)pos);
        t32_rc_end(rc, idr ? 400000u : 150000u, st, 48);
    }
}

int main(void)
{
    int q0[10], q1[10], q2[10];
    T32Rc rc;

    run(0, q0, &rc);
    CHECK(t32_rc_get_param(&rc, 0x194) == 3u, "QpgMode word P+0x194 %u", t32_rc_get_param(&rc, 0x194));
    t32_rc_free(&rc);
    run(1, q1, &rc);
    /* level 2 as a raw word: the float clamp makes 2.0f of it; mode 1 -> at least max QP */
    CHECK(t32_rc_get_param(&rc, 0x140) == 0x40000000u, "trig level %#x", t32_rc_get_param(&rc, 0x140));
    CHECK(t32_rc_get_param(&rc, 0x144) == t32_rc_get_param(&rc, T33P_MAX_QP), "trig QP %u",
          t32_rc_get_param(&rc, 0x144));
    CHECK(t32_rc_get_param(&rc, 0x194) == 3u, "QpgMode after the restart");
    t32_rc_free(&rc);
    run(2, q2, &rc);
    CHECK(t32_rc_get_param(&rc, 0x19c) == 2u && t32_rc_get_param(&rc, 0x1b0) == 2u, "super frame mode %u / %u",
          t32_rc_get_param(&rc, 0x19c), t32_rc_get_param(&rc, 0x1b0));
    CHECK(t32_rc_get_param(&rc, 0x1a0) == 1920u * 1088u * 3u / 50u, "super I threshold clamp %u",
          t32_rc_get_param(&rc, 0x1a0));
    t32_rc_free(&rc);
    /* batch 3: Color2Grey / HSkip (T32_E1RC_RESTART = 4) restart without parameter words */
    int q4[10];
    run(4, q4, &rc);
    t32_rc_free(&rc);
    CHECK(q4[4] == q0[0], "Color2Grey/HSkip restart QP %d (first %d)", q4[4], q0[0]);
    for (int i = 0; i < 4; i++)
        CHECK(q4[i] == q0[i], "restart flag: picture %d before the change", i);
    if (getenv("RC784_DUMP"))
        for (int i = 0; i < 10; i++)
            printf("pic %d qp %d %d %d\n", i, q0[i], q1[i], q2[i]);
    CHECK(q1[4] == q0[0] && q2[4] == q0[0], "restart QP %d %d (first %d)", q1[4], q2[4], q0[0]);
    CHECK(q0[4] != q0[0], "no restart without a change: %d", q0[4]);
    for (int i = 0; i < 4; i++)
        CHECK(q1[i] == q0[i] && q2[i] == q0[i], "picture %d before the change", i);
    if (fails)
        return 1;
    printf("t33 rc784: ok\n");
    return 0;
}
