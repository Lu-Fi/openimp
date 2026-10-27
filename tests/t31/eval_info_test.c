/* eval_info_test - layout of the T31 IMP_Encoder_GetChnEvalInfo record: the
 * IMPEncoderStreamInfo of the vendor header (36 bytes; the vendor's own copy
 * is shifted by 4 bytes, OpenIMP is not). */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <imp/imp_encoder.h>
#include "t31_eval_info.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void)
{
    uint8_t m[T31_SLICE_STATUS_SIZE], out[T31_EVAL_SIZE];
    uint32_t w[6] = { 11, 22, 33, 44, 55, 66 };
    uint16_t mn = 17, mx = 43, q;
    IMPEncoderStreamInfo si;
    int i;

    /* the public struct the record must match */
    CHECK(sizeof(IMPEncoderStreamInfo) == T31_EVAL_SIZE, "sizeof %zu", sizeof(IMPEncoderStreamInfo));
    CHECK(offsetof(IMPEncoderStreamInfo, iNumBytes) == 0 &&
          offsetof(IMPEncoderStreamInfo, uNumIntra) == 4 &&
          offsetof(IMPEncoderStreamInfo, uNumCU64x64) == 24 &&
          offsetof(IMPEncoderStreamInfo, iSliceQP) == 28 &&
          offsetof(IMPEncoderStreamInfo, iMinQP) == 30 &&
          offsetof(IMPEncoderStreamInfo, iMaxQP) == 32, "IMPEncoderStreamInfo offsets");

    t31_eval_status_init(m);
    memcpy(&q, m + 0x3e, 2);
    CHECK(q == 32767, "min QP starts at 32767, not %u", q);
    for (i = 0; i < 0x70; i++)
        CHECK(i == 0x3e || i == 0x3f || m[i] == 0, "byte %#x not zero", i);

    memcpy(m + 0x1c, w, sizeof(w));
    memcpy(m + 0x3e, &mn, 2);
    memcpy(m + 0x40, &mx, 2);
    memset(out, 0xee, sizeof(out));
    t31_eval_pack(m, 12345, 28, out);
    memcpy(&si, out, sizeof(si));
    CHECK(si.iNumBytes == 12345, "iNumBytes %d", si.iNumBytes);
    CHECK(si.uNumIntra == 11 && si.uNumSkip == 22 && si.uNumCU8x8 == 33 &&
          si.uNumCU16x16 == 44 && si.uNumCU32x32 == 55 && si.uNumCU64x64 == 66,
          "counters %u %u %u %u %u %u", si.uNumIntra, si.uNumSkip, si.uNumCU8x8,
          si.uNumCU16x16, si.uNumCU32x32, si.uNumCU64x64);
    CHECK(si.iSliceQP == 28 && si.iMinQP == 17 && si.iMaxQP == 43,
          "QP %d %d %d", si.iSliceQP, si.iMinQP, si.iMaxQP);
    CHECK(out[34] == 0 && out[35] == 0, "pad %02x %02x", out[34], out[35]);

    /* JPEG: iNumBytes, quality factor at +4, rest zero */
    memset(out, 0xee, sizeof(out));
    t31_eval_pack_jpeg(40000, 75, out);
    memcpy(&si, out, 8);
    memcpy(&q, out + 4, 2);
    CHECK(si.iNumBytes == 40000 && q == 75 && out[6] == 0 && out[7] == 0 &&
          out[8] == 0xee, "jpeg record %d q %u", si.iNumBytes, q);
    if (failures) return 1;
    printf("t31 eval info record tests passed\n");
    return 0;
}
