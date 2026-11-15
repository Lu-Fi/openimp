#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "t40_ep1.h"
#include "t40_lambda.h"

/* floor(sqrt(x)) in 1..0xfffe, as the vendor's bisection (x = 0 -> 1). */
static uint32_t t41_lda_isqrt(uint32_t x)
{
    uint32_t lo = 1u, hi = 0xffffu;

    while (lo + 1u != hi) {
        uint32_t mid = (lo + hi) >> 1;

        if (x < mid * mid)
            hi = mid;
        else
            lo = mid;
    }
    return lo;
}

/* round(16 * 2^(qp/3)) for qp 0..51, exact in integers:
 * 16 * 2^(qp/3) = 2^(4 + qp/3) * {1, 2^(1/3), 2^(2/3)}. */
static uint32_t t41_lda_qp_scale(unsigned int qp)
{
    /* 2^(r/3) in Q30 for r = 0, 1, 2 */
    static const uint64_t cube_root2_q30[3] = {
        1073741824ull, 1352829926ull, 1704458901ull,
    };
    uint64_t v = cube_root2_q30[qp % 3u] << (4u + qp / 3u);

    return (uint32_t)((v + (1ull << 29)) >> 30);
}

int openimp_t41_update_ep1_lambda_fixqp(void *ep1, size_t size, int intra,
                                        int full_weight)
{
    uint8_t *out = (uint8_t *)ep1;
    uint32_t weight_a = intra ? 51u : 90u;
    const uint32_t weight_b = 90u;
    unsigned int qp;

    if (!out || size < 52u * 4u)
        return -1;
    for (qp = 0u; qp < 52u; ++qp) {
        uint32_t scale = t41_lda_qp_scale(qp);
        uint32_t wa = weight_a;
        uint32_t wb = weight_b;
        uint32_t a, b;

        if (!full_weight) {
            /* clip((qp - 12) / 6, 2, 4) in Q12, C division */
            int32_t f = ((int32_t)qp * 4096 - 49152) / 6;

            if (f > 16394)
                f = 16394;
            if (f < 8192)
                f = 8192;
            wa = ((uint32_t)f * weight_a) >> 12;
            wb = ((uint32_t)f * weight_b) >> 12;
        }
        a = t41_lda_isqrt(scale * wa);
        b = t41_lda_isqrt(scale * wb);
        out[qp * 4u + 0u] = (uint8_t)a;
        out[qp * 4u + 1u] = (uint8_t)(a >> 8);
        out[qp * 4u + 2u] = (uint8_t)b;
        out[qp * 4u + 3u] = (uint8_t)(b >> 8);
    }
    return 0;
}

int32_t AL_AVC_GenerateHwScalingList(void *scaling, int32_t scratch);
int32_t AL_AVC_WriteEncHwScalingList(void *scaling, void *scratch,
                                     uint32_t *output);
int32_t AL_AVC_WriteEncHwScalingListT41(void *scaling, void *scratch,
                                        uint32_t *output);
int32_t AL_HEVC_GenerateHwScalingList(void *scaling, void *scratch);
int32_t AL_HEVC_WriteEncHwScalingList(void *scaling, void *scratch,
                                      uint32_t *output);

/* The stock AVC/HEVC lambda tables (used when no external Lambdas.hex
 * override exists) are generated from the HM/JM lambda formula, see
 * t40_lambda.h and docs/T31_LAMBDA.md. */

/*
 * T31's fixed-QP AVC path uses the non-rate-controlled lambda table emitted
 * by OEM AL_AVC_GenerateLda.  Each four-byte entry covers one QP (0..51).
 * This table is codec/mode state: it is independent of sensor and picture
 * dimensions.  The generated default table remains in use for rate-controlled
 * T31 pictures and every T40 picture.
 */
static const uint8_t avc_fixqp_lda[0xd0] = {
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x02, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02,
    0x00, 0x02, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02,
    0x00, 0x03, 0x01, 0x03, 0x00, 0x03, 0x02, 0x03,
    0x00, 0x04, 0x02, 0x04, 0x00, 0x05, 0x02, 0x05,
    0x00, 0x06, 0x02, 0x06, 0x00, 0x07, 0x03, 0x07,
    0x00, 0x08, 0x03, 0x08, 0x00, 0x09, 0x04, 0x09,
    0x00, 0x0a, 0x04, 0x0a, 0x00, 0x0c, 0x05, 0x0c,
    0x00, 0x0e, 0x05, 0x0e, 0x00, 0x10, 0x06, 0x10,
    0x00, 0x12, 0x07, 0x12, 0x00, 0x15, 0x08, 0x15,
    0x00, 0x17, 0x08, 0x17, 0x00, 0x1a, 0x0a, 0x1a,
    0x00, 0x1e, 0x0b, 0x1e, 0x00, 0x21, 0x0c, 0x21,
    0x00, 0x25, 0x0e, 0x25, 0x00, 0x2a, 0x10, 0x2a,
    0x00, 0x2f, 0x11, 0x2f, 0x00, 0x35, 0x14, 0x35,
    0x00, 0x3c, 0x16, 0x3c, 0x00, 0x43, 0x19, 0x43,
    0x00, 0x4b, 0x1c, 0x4b, 0x00, 0x55, 0x20, 0x55,
    0x00, 0x5f, 0x23, 0x5f, 0x00, 0x6b, 0x28, 0x6b,
};

static const uint8_t avc_default_scaling_8x8[0x80] = {
    0x06, 0x0a, 0x0d, 0x10, 0x12, 0x17, 0x19, 0x1b, 0x0a, 0x0b, 0x10, 0x12,
    0x17, 0x19, 0x1b, 0x1d, 0x0d, 0x10, 0x12, 0x17, 0x19, 0x1b, 0x1d, 0x1f,
    0x10, 0x12, 0x17, 0x19, 0x1b, 0x1d, 0x1f, 0x21, 0x12, 0x17, 0x19, 0x1b,
    0x1d, 0x1f, 0x21, 0x24, 0x17, 0x19, 0x1b, 0x1d, 0x1f, 0x21, 0x24, 0x26,
    0x19, 0x1b, 0x1d, 0x1f, 0x21, 0x24, 0x26, 0x28, 0x1b, 0x1d, 0x1f, 0x21,
    0x24, 0x26, 0x28, 0x2a, 0x09, 0x0d, 0x0f, 0x11, 0x13, 0x15, 0x16, 0x18,
    0x0d, 0x0d, 0x11, 0x13, 0x15, 0x16, 0x18, 0x19, 0x0f, 0x11, 0x13, 0x15,
    0x16, 0x18, 0x19, 0x1b, 0x11, 0x13, 0x15, 0x16, 0x18, 0x19, 0x1b, 0x1c,
    0x13, 0x15, 0x16, 0x18, 0x19, 0x1b, 0x1c, 0x1e, 0x15, 0x16, 0x18, 0x19,
    0x1b, 0x1c, 0x1e, 0x20, 0x16, 0x18, 0x19, 0x1b, 0x1c, 0x1e, 0x20, 0x21,
    0x18, 0x19, 0x1b, 0x1c, 0x1e, 0x20, 0x21, 0x23,
};

static const uint8_t avc_default_scaling_4x4[0x20] = {
    0x06, 0x0d, 0x14, 0x1c, 0x0d, 0x14, 0x1c, 0x20,
    0x14, 0x1c, 0x20, 0x25, 0x1c, 0x20, 0x25, 0x2a,
    0x0a, 0x0e, 0x14, 0x18, 0x0e, 0x14, 0x18, 0x1b,
    0x14, 0x18, 0x1b, 0x1e, 0x18, 0x1b, 0x1e, 0x22,
};

static int openimp_init_ep1(void *ep1, size_t size, int use_fixqp_lda,
                            int use_t41_layout)
{
    uint8_t scaling[0x400];
    uint8_t *scratch;
    unsigned int i;

    if (ep1 == NULL || size < 0x6400u)
        return -1;
    scratch = (uint8_t *)calloc(1, 0x5dd0u);
    if (scratch == NULL)
        return -1;

    memset(ep1, 0, size);
    memset(scaling, 0, sizeof(scaling));
    if (use_t41_layout) {
        if (openimp_t41_update_ep1_lambda(ep1, size, 2u) != 0) {
            free(scratch);
            return -1;
        }
    } else {
        if (use_fixqp_lda)
            memcpy(ep1, avc_fixqp_lda, sizeof(avc_fixqp_lda));
        else
            t40_lambda_default_table((uint8_t *)ep1, 0);
    }
    memcpy(scaling + 0x1bc, avc_default_scaling_8x8, 0x40);
    memcpy(scaling + 0x27c, avc_default_scaling_8x8 + 0x40, 0x40);
    for (i = 0; i < 3; ++i) {
        memcpy(scaling + 0x03c + i * 0x40,
               avc_default_scaling_4x4, 0x10);
        memcpy(scaling + 0x0fc + i * 0x40,
               avc_default_scaling_4x4 + 0x10, 0x10);
    }

    AL_AVC_GenerateHwScalingList(scaling, (int32_t)(intptr_t)scratch);
    if (use_t41_layout)
        AL_AVC_WriteEncHwScalingListT41(
            scaling, scratch, (uint32_t *)((uint8_t *)ep1 + 0x100));
    else
        AL_AVC_WriteEncHwScalingList(
            scaling, scratch, (uint32_t *)((uint8_t *)ep1 + 0x100));
    free(scratch);
    return 0;
}

/*
 * T31 HEVC: AL_GetLambda selects the HEVC default lambda table instead of
 * the AVC one when the channel codec is HEVC (generated, t40_lambda.h).
 * HEVC runs without scaling lists, so nothing is written at EP1+0x100.
 */

#if defined(PLATFORM_T40) && !defined(PLATFORM_T41)
/* H.265 Table 7-6 default 8x8 lists in raster order (intra, inter) and the
 * flat 4x4 lists, as AL_HEVC_DefaultScalingLists8x8/4x4 in the vendor T40
 * libimp 1.3.1 (HEVC_EncUtils.c.o .rodata 0x14 and 0x94). */
static const uint8_t hevc_default_scaling_8x8[0x80] = {
    16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29, 16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47, 18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88, 24, 25, 29, 36, 47, 65, 88, 115,
    16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28, 16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41, 18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71, 24, 25, 28, 33, 41, 54, 71, 91,
};

/*
 * The scaling-list block of the vendor HEVC SPS structure (AL_HEVC_GenerateSPS
 * with eScalingList = AL_SCL_DEFAULT, SPS + 546), the input of
 * AL_HEVC_GenerateHwScalingList:
 *   +0x00..0x2f  prediction flags / matrix id deltas (0)
 *   +0x30..0x35  16x16 DC of matrices 0..5, +0x36 and +0x39 32x32 DC of
 *                matrices 0 and 3 (16; the other DC bytes stay 0)
 *   +0x3c + (size * 6 + matrix) * 0x40  coefficients in raster order:
 *                4x4 flat 16, 8x8/16x16 matrices 0..2 intra and 3..5 inter,
 *                32x32 matrix 0 intra and 3 inter.
 */
#define T40_HEVC_SCL_SIZE 0x600u
#define T40_HEVC_SCL_LIST(size, matrix) \
    (0x3cu + ((size) * 6u + (matrix)) * 0x40u)

static void t40_hevc_default_scaling(uint8_t *scl)
{
    unsigned int size, matrix;

    memset(scl, 0, T40_HEVC_SCL_SIZE);
    for (matrix = 0u; matrix < 6u; ++matrix) {
        const uint8_t *def8 = hevc_default_scaling_8x8 +
                              (matrix < 3u ? 0u : 0x40u);

        memset(scl + T40_HEVC_SCL_LIST(0u, matrix), 16, 16);
        for (size = 1u; size <= 3u; ++size) {
            if (size == 3u && matrix != 0u && matrix != 3u)
                continue;
            memcpy(scl + T40_HEVC_SCL_LIST(size, matrix), def8, 0x40);
        }
        scl[0x30u + matrix] = 16u;
    }
    scl[0x36] = 16u;
    scl[0x39] = 16u;
}

/*
 * T40 HEVC with default scaling lists (vendor libimp 1.3.1
 * HEVC_Encoder.c preprocessEp1 -> AL_HEVC_PreprocessScalingList): the
 * forward/inverse hardware tables generated from the default lists go to
 * EP1+0x100 (0x6200 bytes, up to EP1+0x6300).  The table depends on the
 * lists only, so writing it once per channel is enough; the lambda rewrites
 * only touch EP1+0x00..0xcf.
 */
int openimp_t40_init_hevc_ep1(void *ep1, size_t size, int scaling_list)
{
    uint8_t *scl;
    uint8_t *scratch;

    if (openimp_t31_init_hevc_ep1(ep1, size) != 0)
        return -1;
    if (!scaling_list)
        return 0;
    scl = (uint8_t *)calloc(1, T40_HEVC_SCL_SIZE + 0x5dd0u);
    if (scl == NULL)
        return -1;
    scratch = scl + T40_HEVC_SCL_SIZE;
    t40_hevc_default_scaling(scl);
    AL_HEVC_GenerateHwScalingList(scl, scratch);
    AL_HEVC_WriteEncHwScalingList(scl, scratch,
                                  (uint32_t *)((uint8_t *)ep1 + 0x100));
    free(scl);
    return 0;
}
#endif

int openimp_t31_init_hevc_ep1(void *ep1, size_t size)
{
    if (ep1 == NULL || size < 0x6400u)
        return -1;
    memset(ep1, 0, size);
    t40_lambda_default_table((uint8_t *)ep1, 1);
    return 0;
}

int openimp_t40_init_ep1(void *ep1, size_t size, int use_fixqp_lda)
{
    return openimp_init_ep1(ep1, size, use_fixqp_lda, 0);
}

int openimp_t41_init_ep1(void *ep1, size_t size)
{
    return openimp_init_ep1(ep1, size, 0, 1);
}

int openimp_t41_update_ep1_lambda(void *ep1, size_t size,
                                  unsigned int picture_type)
{
    return openimp_t41_update_ep1_lambda_codec(ep1, size, picture_type, 0u);
}

int openimp_t41_update_ep1_lambda_codec(void *ep1, size_t size,
                                        unsigned int picture_type,
                                        unsigned int hevc)
{
    uint8_t *output = (uint8_t *)ep1;
    uint8_t default_lda[T40_LAMBDA_TABLE_SIZE];
    unsigned int qp;

    if (!output || size < sizeof(default_lda) || picture_type > 2u)
        return -1;
    t40_lambda_default_table(default_lda, hevc ? 1 : 0);

    /* Exact T41 AL_GetLambda layout: each QP is two big-endian 16-bit
     * lanes, so only bytes 1 and 3 carry the selected lambda values.
     * OEM prepares type 2 for IDR/I pictures and rewrites type 1 for P. */
    for (qp = 0u; qp < 52u; ++qp) {
        output[qp * 4u] = 0u;
        output[qp * 4u + 1u] =
            default_lda[qp * 4u + picture_type];
        output[qp * 4u + 2u] = 0u;
        output[qp * 4u + 3u] = default_lda[qp * 4u + 3u];
    }
    return 0;
}

int openimp_t41_init_hevc_ep1(void *ep1, size_t size)
{
    if (ep1 == NULL || size < 0x6400u)
        return -1;
    memset(ep1, 0, size);
    return openimp_t41_update_ep1_lambda_codec(ep1, size, 2u, 1u);
}

/*
 * T40 vendor libimp 1.3.1 AVC FixQP lambda words (#443): EP1 read at every
 * AVPU push on the Eufy T40XP, FixQP 25 at 1920x1080 and 640x360 (same
 * table at both sizes, 2026-11-14).  Bytes 1 and 3 equal the T31 table
 * above; byte 2 (the intra lane) is about twice the T31 value from QP 22.
 */
static const uint8_t t40_avc_fixqp_lda[0xd0] = {
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x02, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02,
    0x00, 0x02, 0x02, 0x02, 0x00, 0x02, 0x02, 0x02,
    0x00, 0x03, 0x02, 0x03, 0x00, 0x03, 0x02, 0x03,
    0x00, 0x04, 0x03, 0x04, 0x00, 0x05, 0x03, 0x05,
    0x00, 0x06, 0x04, 0x06, 0x00, 0x07, 0x05, 0x07,
    0x00, 0x08, 0x06, 0x08, 0x00, 0x09, 0x07, 0x09,
    0x00, 0x0a, 0x08, 0x0a, 0x00, 0x0c, 0x09, 0x0c,
    0x00, 0x0e, 0x0a, 0x0e, 0x00, 0x10, 0x0c, 0x10,
    0x00, 0x12, 0x0e, 0x12, 0x00, 0x15, 0x10, 0x15,
    0x00, 0x17, 0x11, 0x17, 0x00, 0x1a, 0x14, 0x1a,
    0x00, 0x1e, 0x16, 0x1e, 0x00, 0x21, 0x19, 0x21,
    0x00, 0x25, 0x1c, 0x25, 0x00, 0x2a, 0x20, 0x2a,
    0x00, 0x2f, 0x23, 0x2f, 0x00, 0x35, 0x28, 0x35,
    0x00, 0x3c, 0x2d, 0x3c, 0x00, 0x43, 0x32, 0x43,
    0x00, 0x4b, 0x39, 0x4b, 0x00, 0x55, 0x40, 0x55,
    0x00, 0x5f, 0x47, 0x5f, 0x00, 0x6b, 0x50, 0x6b,
};

/*
 * T40 FixQP: P pictures carry the fixed-QP table; the intra-only form keeps
 * only byte 2 (the intra lambda) and clears the inter lanes (the vendor
 * writes it for the first picture of a channel, see codec-t40.c).  AVC uses
 * the vendor 1.3.1 table unless t31_table is set (HEVC, not captured, and
 * OPENIMP_T40_LDA131=0).  The scaling list at EP1+0x100 stays as
 * initialised.
 */
int openimp_t40_update_fixqp_ep1_table(void *ep1, size_t size, int is_idr,
                                       int t31_table)
{
    uint8_t *output = (uint8_t *)ep1;
    unsigned int qp;

    if (!output || size < sizeof(avc_fixqp_lda))
        return -1;
    memcpy(output, t31_table ? avc_fixqp_lda : t40_avc_fixqp_lda,
           sizeof(avc_fixqp_lda));
    if (is_idr) {
        for (qp = 0u; qp < 52u; ++qp) {
            output[qp * 4u] = 0u;
            output[qp * 4u + 1u] = 0u;
            output[qp * 4u + 3u] = 0u;
        }
    }
    return 0;
}

int openimp_t40_update_fixqp_ep1(void *ep1, size_t size, int is_idr)
{
    return openimp_t40_update_fixqp_ep1_table(ep1, size, is_idr, 0);
}
