#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t40/t40_ep1.h"

/* The layout test exercises only the lambda prefix.  Initialization's
 * scaling-list calls are linked here as harmless host stubs. */
int32_t AL_AVC_GenerateHwScalingList(void *scaling, int32_t scratch)
{
    (void)scaling;
    (void)scratch;
    return 0;
}

int32_t AL_AVC_WriteEncHwScalingList(void *scaling, void *scratch,
                                     uint32_t *output)
{
    (void)scaling;
    (void)scratch;
    (void)output;
    return 0;
}

int32_t AL_AVC_WriteEncHwScalingListT41(void *scaling, void *scratch,
                                        uint32_t *output)
{
    (void)scaling;
    (void)scratch;
    (void)output;
    return 0;
}

/* Vendor libimp 1.2.6 EP1 under FixQP (capture .34 sc5336, 1280x720
 * FixQP 28, uGopLength 30, uMaxSameSenceCnt 2): the IDR, a P picture and
 * the P picture 30 after the IDR; little-endian lanes A/B for QP 0..51. */
static const uint16_t fixqp_idr[52][2] = {
    {28, 37}, {31, 42}, {35, 47}, {40, 53},
    {45, 60}, {51, 67}, {57, 75}, {64, 85},
    {72, 95}, {80, 107}, {90, 120}, {101, 135},
    {114, 151}, {128, 170}, {143, 191}, {161, 214},
    {181, 240}, {203, 270}, {228, 303}, {256, 340},
    {287, 382}, {323, 429}, {362, 481}, {407, 540},
    {457, 607}, {513, 681}, {575, 764}, {646, 858},
    {725, 963}, {814, 1081}, {914, 1214}, {1026, 1363},
    {1151, 1529}, {1292, 1717}, {1451, 1927}, {1628, 2163},
    {1828, 2428}, {2052, 2726}, {2303, 3059}, {2585, 3434},
    {2902, 3855}, {3257, 4327}, {3656, 4857}, {4104, 5452},
    {4606, 6119}, {5170, 6869}, {5804, 7710}, {6514, 8654},
    {7312, 9714}, {8208, 10904}, {9213, 12239}, {10341, 13738},
};
static const uint16_t fixqp_p[52][2] = {
    {53, 53}, {60, 60}, {67, 67}, {75, 75},
    {84, 84}, {95, 95}, {107, 107}, {120, 120},
    {135, 135}, {151, 151}, {170, 170}, {191, 191},
    {214, 214}, {241, 241}, {270, 270}, {303, 303},
    {340, 340}, {382, 382}, {429, 429}, {481, 481},
    {540, 540}, {607, 607}, {681, 681}, {764, 764},
    {858, 858}, {1000, 1000}, {1165, 1165}, {1357, 1357},
    {1570, 1570}, {1817, 1817}, {2103, 2103}, {2421, 2421},
    {2788, 2788}, {3212, 3212}, {3685, 3685}, {4230, 4230},
    {4857, 4857}, {5452, 5452}, {6119, 6119}, {6869, 6869},
    {7710, 7710}, {8654, 8654}, {9714, 9714}, {10904, 10904},
    {12239, 12239}, {13738, 13738}, {15420, 15420}, {17309, 17309},
    {19429, 19429}, {21808, 21808}, {24479, 24479}, {27476, 27476},
};
static const uint16_t fixqp_p_full[52][2] = {
    {37, 37}, {42, 42}, {47, 47}, {53, 53},
    {60, 60}, {67, 67}, {75, 75}, {85, 85},
    {95, 95}, {107, 107}, {120, 120}, {135, 135},
    {151, 151}, {170, 170}, {191, 191}, {214, 214},
    {240, 240}, {270, 270}, {303, 303}, {340, 340},
    {382, 382}, {429, 429}, {481, 481}, {540, 540},
    {607, 607}, {681, 681}, {764, 764}, {858, 858},
    {963, 963}, {1081, 1081}, {1214, 1214}, {1363, 1363},
    {1529, 1529}, {1717, 1717}, {1927, 1927}, {2163, 2163},
    {2428, 2428}, {2726, 2726}, {3059, 3059}, {3434, 3434},
    {3855, 3855}, {4327, 4327}, {4857, 4857}, {5452, 5452},
    {6119, 6119}, {6869, 6869}, {7710, 7710}, {8654, 8654},
    {9714, 9714}, {10904, 10904}, {12239, 12239}, {13738, 13738},
};

/* The same vendor run as H.265 (FixQP 28, AL_GetLambda wrapped via
 * LD_PRELOAD, 2026-10-25): HEVC uses the identical dynamic lambda. */
static const uint16_t hevc_fixqp_idr[52][2] = {
    {28, 37}, {31, 42}, {35, 47}, {40, 53},
    {45, 60}, {51, 67}, {57, 75}, {64, 85},
    {72, 95}, {80, 107}, {90, 120}, {101, 135},
    {114, 151}, {128, 170}, {143, 191}, {161, 214},
    {181, 240}, {203, 270}, {228, 303}, {256, 340},
    {287, 382}, {323, 429}, {362, 481}, {407, 540},
    {457, 607}, {513, 681}, {575, 764}, {646, 858},
    {725, 963}, {814, 1081}, {914, 1214}, {1026, 1363},
    {1151, 1529}, {1292, 1717}, {1451, 1927}, {1628, 2163},
    {1828, 2428}, {2052, 2726}, {2303, 3059}, {2585, 3434},
    {2902, 3855}, {3257, 4327}, {3656, 4857}, {4104, 5452},
    {4606, 6119}, {5170, 6869}, {5804, 7710}, {6514, 8654},
    {7312, 9714}, {8208, 10904}, {9213, 12239}, {10341, 13738},
};
static const uint16_t hevc_fixqp_p[52][2] = {
    {53, 53}, {60, 60}, {67, 67}, {75, 75},
    {84, 84}, {95, 95}, {107, 107}, {120, 120},
    {135, 135}, {151, 151}, {170, 170}, {191, 191},
    {214, 214}, {241, 241}, {270, 270}, {303, 303},
    {340, 340}, {382, 382}, {429, 429}, {481, 481},
    {540, 540}, {607, 607}, {681, 681}, {764, 764},
    {858, 858}, {1000, 1000}, {1165, 1165}, {1357, 1357},
    {1570, 1570}, {1817, 1817}, {2103, 2103}, {2421, 2421},
    {2788, 2788}, {3212, 3212}, {3685, 3685}, {4230, 4230},
    {4857, 4857}, {5452, 5452}, {6119, 6119}, {6869, 6869},
    {7710, 7710}, {8654, 8654}, {9714, 9714}, {10904, 10904},
    {12239, 12239}, {13738, 13738}, {15420, 15420}, {17309, 17309},
    {19429, 19429}, {21808, 21808}, {24479, 24479}, {27476, 27476},
};
static const uint16_t hevc_fixqp_p_full[52][2] = {
    {37, 37}, {42, 42}, {47, 47}, {53, 53},
    {60, 60}, {67, 67}, {75, 75}, {85, 85},
    {95, 95}, {107, 107}, {120, 120}, {135, 135},
    {151, 151}, {170, 170}, {191, 191}, {214, 214},
    {240, 240}, {270, 270}, {303, 303}, {340, 340},
    {382, 382}, {429, 429}, {481, 481}, {540, 540},
    {607, 607}, {681, 681}, {764, 764}, {858, 858},
    {963, 963}, {1081, 1081}, {1214, 1214}, {1363, 1363},
    {1529, 1529}, {1717, 1717}, {1927, 1927}, {2163, 2163},
    {2428, 2428}, {2726, 2726}, {3059, 3059}, {3434, 3434},
    {3855, 3855}, {4327, 4327}, {4857, 4857}, {5452, 5452},
    {6119, 6119}, {6869, 6869}, {7710, 7710}, {8654, 8654},
    {9714, 9714}, {10904, 10904}, {12239, 12239}, {13738, 13738},
};

static void check_fixqp(const uint16_t expect[52][2], int intra, int full)
{
    uint8_t ep1[52u * 4u];
    unsigned int qp;

    memset(ep1, 0xa5, sizeof(ep1));
    assert(openimp_t41_update_ep1_lambda_fixqp(ep1, sizeof(ep1), intra,
                                               full) == 0);
    for (qp = 0u; qp < 52u; ++qp) {
        uint16_t a = (uint16_t)(ep1[qp * 4u] | (ep1[qp * 4u + 1u] << 8));
        uint16_t b = (uint16_t)(ep1[qp * 4u + 2u] | (ep1[qp * 4u + 3u] << 8));

        if (a != expect[qp][0] || b != expect[qp][1]) {
            fprintf(stderr, "fixqp lambda qp %u: %u/%u expected %u/%u\n",
                    qp, a, b, expect[qp][0], expect[qp][1]);
            assert(0);
        }
    }
}

int main(void)
{
    uint8_t ep1[0x6400u];
    uint8_t intra[52u * 4u];
    unsigned int changed = 0u;
    unsigned int qp;

    assert(openimp_t41_init_ep1(ep1, sizeof(ep1)) == 0);
    memcpy(intra, ep1, sizeof(intra));
    for (qp = 0u; qp < 52u; ++qp) {
        assert(ep1[qp * 4u] == 0u);
        assert(ep1[qp * 4u + 2u] == 0u);
    }

    assert(openimp_t41_update_ep1_lambda(ep1, sizeof(ep1), 1u) == 0);
    for (qp = 0u; qp < 52u; ++qp) {
        assert(ep1[qp * 4u] == 0u);
        assert(ep1[qp * 4u + 2u] == 0u);
        assert(ep1[qp * 4u + 3u] == intra[qp * 4u + 3u]);
        changed += memcmp(ep1 + qp * 4u, intra + qp * 4u, 4u) != 0;
    }
    /* This is the exact first-P delta observed in the OEM EP1 trace. */
    assert(changed == 26u);

    assert(openimp_t41_update_ep1_lambda(ep1, sizeof(ep1), 2u) == 0);
    assert(memcmp(ep1, intra, sizeof(intra)) == 0);
    check_fixqp(fixqp_idr, 1, 1);
    check_fixqp(fixqp_p, 0, 0);
    check_fixqp(fixqp_p_full, 0, 1);
    check_fixqp(hevc_fixqp_idr, 1, 1);
    check_fixqp(hevc_fixqp_p, 0, 0);
    check_fixqp(hevc_fixqp_p_full, 0, 1);
    assert(openimp_t41_update_ep1_lambda_fixqp(NULL, 208u, 0, 0) == -1);
    assert(openimp_t41_update_ep1_lambda_fixqp(ep1, 207u, 0, 0) == -1);
    assert(openimp_t41_update_ep1_lambda(NULL, sizeof(ep1), 1u) == -1);
    assert(openimp_t41_update_ep1_lambda(ep1, 0xcfu, 1u) == -1);
    assert(openimp_t41_update_ep1_lambda(ep1, sizeof(ep1), 3u) == -1);

    /* HEVC (T41 AL_GetLambda, HEVC_NEW_DEFAULT_LDA_TABLE): the I lane
     * equals AVC's, P differs only at QP 37 (12 vs 13), B in many places;
     * no scaling list. */
    {
        static uint8_t hevc[0x6400u];
        unsigned int i;

        memset(hevc, 0xa5, sizeof(hevc));
        assert(openimp_t41_init_hevc_ep1(hevc, sizeof(hevc)) == 0);
        assert(hevc[51u * 4u + 1u] == 67u && hevc[51u * 4u + 3u] == 59u);
        assert(hevc[37u * 4u + 3u] == 12u && intra[37u * 4u + 3u] == 13u);
        hevc[37u * 4u + 3u] = 13u;
        assert(memcmp(hevc, intra, sizeof(intra)) == 0);
        for (i = 0x100u; i < sizeof(hevc); ++i)
            assert(hevc[i] == 0u);
        assert(openimp_t41_update_ep1_lambda_codec(hevc, sizeof(hevc),
                                                   0u, 1u) == 0);
        assert(hevc[51u * 4u + 1u] == 104u);
        assert(openimp_t41_update_ep1_lambda_codec(ep1, sizeof(ep1),
                                                   0u, 0u) == 0);
        assert(ep1[51u * 4u + 1u] == 91u);
        assert(openimp_t41_init_hevc_ep1(hevc, 0x63ffu) == -1);
    }

    puts("T41 EP1 lambda layout: OK");
    return 0;
}
