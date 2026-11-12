/*
 * T32 rate control (#295) host test: the attribute mapping of
 * src/t32/openimp_t32_rc.c and the controller in a closed loop on a
 * synthetic picture-size model (no HERA needed).
 *
 *   rc_test                 mapping + closed-loop checks
 *   rc_test trace MODE ...  print the QP / size sequence of one run (the
 *                           input format of tools/eprc_t33_oracle.py
 *                           --compare)
 *   rc_test FILE            also compare with the vendor 2.2.0 sequences of
 *                           FILE (vendor-2.2.0-rc-imp-h264.txt, same model,
 *                           set up through the vendor IMP layer;
 *                           the device motion mode 2 gives the same QPs on
 *                           it as the harness's 0): H.264 runs the T33
 *                           revision through the attribute mapping, so the
 *                           QP of every picture must equal the vendor's
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* synthetic coded size: bits = cplx * MBs * 2^((26 - qp) / 6), I pictures
 * 5x a P picture; @scene scales the complexity (motion) */
static uint32_t model_bytes(int qp, int idr, uint32_t mbs, double cplx)
{
    double bits = cplx * mbs * pow(2.0, (26.0 - qp) / 6.0) * (idr ? 5.0 : 1.0);

    return (uint32_t)(bits / 8.0) + 16u;
}

/* statistics the controller reads, synthetic but in the engine's ranges:
 * QP sums (0x80140..) and an SSE share proportional to the size */
static void model_stats(int qp, uint32_t bytes, uint32_t mbs, uint32_t st[48])
{
    memset(st, 0, 48 * sizeof(st[0]));
    st[8] = (uint32_t)qp * mbs & 0xffffu;
    st[10] = mbs & 0xffffu;
    st[12] = bytes * 3u;
    st[15] = mbs * 4u;
    st[20] = bytes / 2u;
    st[21] = bytes / 3u;
}

struct run {
    uint64_t bits;
    int qmin, qmax, imin, imax;
    uint32_t n;
};

/* the attributes of the vendor reference runs: i264e_param_default values
 * for everything the runs do not set (QP 15..48, initial QP 30) */
/* the channel of the vendor runs (tools/eprc_t33_oracle.py --imp): the
 * IMP_Encoder_SetDefaultParam defaults (initial QP 30) with max QP / max I QP 48 */
static void fill(IMPEncoderAttrRcMode *m, int mode, uint32_t kbps)
{
    memset(m, 0, sizeof(*m));
    m->rcMode = (IMPEncoderRcMode)mode;
    t32_rc_default_attr(m, PT_H264, 30, kbps);
    /* the head is common to CBR/VBR/SMART/CVBR/AVBR */
    m->attrH264Cbr.maxQp = 48;
    m->attrH264Cbr.maxIQp = 48;
}

static int qp_trace[4096];

static int simulate(int mode, uint32_t kbps, int hevc, uint32_t w, uint32_t h, uint32_t pics,
                    double cplx, int trace, struct run *r, const IMPEncoderAttrRcMode *given)
{
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    uint32_t mbs = (w / 16u) * (h / 16u), gop = 50, i, gop_pos = 0;

    if (given)
        m = *given;
    else
        fill(&m, mode, kbps);
    memset(&rc, 0, sizeof(rc));
    memset(r, 0, sizeof(*r));
    r->qmin = r->imin = 99;
    if (t32_rc_init(&rc, &m, w, h, 0, gop, 25, 1, hevc) != 0 || !rc.on)
        return -1;
    for (i = 0; i < pics; i++) {
        int idr = gop_pos == 0, qp = t32_rc_start(&rc, idr, gop_pos);
        /* a scene change at 1/3: twice the complexity */
        double c = cplx * (i >= pics / 3u && i < 2u * pics / 3u ? 2.0 : 1.0);
        uint32_t bytes, st[48];

        if (qp < 0) {
            t32_rc_free(&rc);
            return -1;
        }
        bytes = model_bytes(qp, idr, mbs, c);
        model_stats(qp, bytes, mbs, st);
        t32_rc_end(&rc, bytes, hevc ? NULL : st, hevc ? 0 : 48);
        if (trace)
            printf("F %u %d %d %u\n", i, idr ? 2 : 0, qp, bytes);
        if (i < 4096u)
            qp_trace[i] = qp;
        r->bits += bytes * 8ull;
        if (idr) {
            r->imin = qp < r->imin ? qp : r->imin;
            r->imax = qp > r->imax ? qp : r->imax;
        } else {
            r->qmin = qp < r->qmin ? qp : r->qmin;
            r->qmax = qp > r->qmax ? qp : r->qmax;
        }
        gop_pos = (gop_pos + 1u) % gop;
    }
    r->n = pics;
    t32_rc_free(&rc);
    return 0;
}

static void test_mapping(void)
{
    IMPEncoderAttrRcMode m;
    EprcParams p;

    fill(&m, IMP_ENC_RC_MODE_CBR, 2000);
    CHECK(t32_rc_params(&m, 1920, 1088, 50, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_CBR &&
          p.bitrate == 2000 && p.min_qp == 15 && p.max_qp == 48 && p.gop == 50, "CBR mapping");
    fill(&m, IMP_ENC_RC_MODE_VBR, 64);
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_VBR &&
          p.max_bitrate == 128 && p.change_pos == 80, "VBR mapping (128 kbit/s floor)");
    fill(&m, IMP_ENC_RC_MODE_SMART, 1000);
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_SMART, "SMART mapping");
    fill(&m, IMP_ENC_RC_MODE_CVBR, 1500);
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_VBR &&
          p.max_bitrate == 1500 && p.static_time == 2, "CVBR mapping");
    fill(&m, IMP_ENC_RC_MODE_AVBR, 1500);
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_SMART, "AVBR mapping");
    memset(&m, 0, sizeof(m));
    m.rcMode = IMP_ENC_RC_MODE_QVBR;
    m.attrH264QVbr.maxQp = 45;
    m.attrH264QVbr.minQp = 20;
    m.attrH264QVbr.targetBitrate = 800;
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.rc_mode == EPRC_MODE_VBR &&
          p.max_bitrate == 800 && p.min_qp == 20, "QVBR mapping");
    memset(&m, 0, sizeof(m));
    m.rcMode = IMP_ENC_RC_MODE_FIXQP;
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) != 0, "FIXQP has no controller");
    fill(&m, IMP_ENC_RC_MODE_CBR, 0);
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) != 0, "CBR at 0 kbit/s refused");
    fill(&m, IMP_ENC_RC_MODE_CBR, 1000);
    m.attrH264Cbr.minQp = 60;
    m.attrH264Cbr.maxQp = 70;
    CHECK(t32_rc_params(&m, 640, 368, 25, 25, 1, &p) == 0 && p.max_qp == 51 && p.min_qp == 51,
          "QP range clamped to 51");
}

static void test_loop(void)
{
    static const int modes[] = { IMP_ENC_RC_MODE_CBR, IMP_ENC_RC_MODE_VBR, IMP_ENC_RC_MODE_SMART,
                                 IMP_ENC_RC_MODE_CVBR, IMP_ENC_RC_MODE_AVBR };
    static const char *const names[] = { "CBR", "VBR", "SMART", "CVBR", "AVBR" };
    unsigned i;
    int hevc;

    for (hevc = 0; hevc < 2; hevc++)
        for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            struct run r;
            double kbps;

            CHECK(simulate(modes[i], 2000, hevc, 1920, 1088, 750, 60.0, 0, &r, NULL) == 0,
                  "%s run", names[i]);
            kbps = r.bits / 1000.0 / (r.n / 25.0);
            printf("%s %-5s 1920x1088 2000 kbit/s: %.0f kbit/s, P qp %d..%d, I qp %d..%d\n",
                   hevc ? "H.265" : "H.264", names[i], kbps, r.qmin, r.qmax, r.imin, r.imax);
            CHECK(r.qmin >= 15 && r.qmax <= 48 && r.imin >= 15 && r.imax <= 48, "%s QP window", names[i]);
            if (modes[i] == IMP_ENC_RC_MODE_CBR)
                CHECK(kbps > 1700.0 && kbps < 2300.0, "CBR rate %.0f outside 2000 +-15 %%", kbps);
            else
                CHECK(kbps < 2300.0, "%s rate %.0f above the maximum", names[i], kbps);
        }
    {
        /* maxIQp / minIQp bound the IDR QP */
        IMPEncoderAttrRcMode m;
        struct run r;

        fill(&m, IMP_ENC_RC_MODE_CBR, 300);
        m.attrH264Cbr.maxIQp = 33;
        m.attrH264Cbr.minIQp = 28;
        CHECK(simulate(0, 0, 0, 1920, 1088, 200, 60.0, 0, &r, &m) == 0 && r.imin >= 28 && r.imax <= 33,
              "IDR QP window 28..33: %d..%d", r.imin, r.imax);
    }
}

/* the vendor sequences of @path against OpenIMP on the same model */
static void test_vendor(const char *path)
{
    static const char *const names[] = { "", "CBR", "VBR", "SMART", "CVBR", "AVBR" };
    FILE *f = fopen(path, "r");
    char line[128];
    int mode = 0, kbps = 0, pics = 0, n = 0, blocks = 0;
    uint64_t vbits = 0;
    long dsum = 0;
    int vqp[4096];

    CHECK(f != NULL, "open %s", path);
    if (!f)
        return;
    for (;;) {
        char *l = fgets(line, sizeof(line), f);
        int a, b, c, d;

        if (!l || line[0] == 'S') {
            if (n) {
                struct run r;
                double vk = vbits / 1000.0 / (n / 25.0), ok;
                int k, dmax = 0;

                CHECK(simulate(mode, (uint32_t)kbps, 0, 1920, 1088, (uint32_t)n, 60.0, 0, &r, NULL) == 0,
                      "vendor block %d", blocks);
                ok = r.bits / 1000.0 / (n / 25.0);
                for (dsum = 0, k = 0; k < n; k++) {
                    int dd = abs(vqp[k] - qp_trace[k]);

                    dsum += dd;
                    dmax = dd > dmax ? dd : dmax;
                }
                printf("vs vendor %-5s %4d kbit/s: vendor %5.0f, OpenIMP %5.0f kbit/s (%+5.1f %%), "
                       "QP |diff| mean %.2f max %d\n", names[mode], kbps, vk, ok, (ok - vk) * 100.0 / vk,
                       (double)dsum / n, dmax);
                /* H.264 runs the T33 revision: the QP of every picture */
                CHECK(dmax == 0, "%s %d: QP differs from the vendor (max %d)", names[mode], kbps, dmax);
                blocks++;
            }
            if (!l)
                break;
            if (sscanf(line, "S %d %d %d", &mode, &kbps, &pics) != 3 || mode < 1 || mode > 5)
                mode = 0;
            n = 0;
            vbits = 0;
        } else if (line[0] == 'F' && mode && sscanf(line, "F %d %d %d %d", &a, &b, &c, &d) == 4 && n < 4096) {
            vqp[n++] = c;
            vbits += (uint64_t)d * 8u;
        }
    }
    fclose(f);
    CHECK(blocks > 0, "no vendor blocks in %s", path);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "trace")) {
        struct run r;
        int mode = atoi(argv[2]);
        uint32_t kbps = argc > 3 ? (uint32_t)atoi(argv[3]) : 2000u;
        uint32_t pics = argc > 4 ? (uint32_t)atoi(argv[4]) : 250u;

        return simulate(mode, kbps, 0, 1920, 1088, pics, 60.0, 1, &r, NULL) == 0 ? 0 : 1;
    }
    test_mapping();
    test_loop();
    if (argc > 1)
        test_vendor(argv[1]);
    if (fails) {
        printf("rate control: %d failure(s)\n", fails);
        return 1;
    }
    printf("T32 rate control (#295): mapping and closed loop ok\n");
    return 0;
}
