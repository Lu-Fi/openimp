/*
 * p2_gap_test - T21/T20 vendor bookkeeping calls of the P2 encoder
 * (gap work 2026-10-10): Get/SetChnFrmUsedMode, Get/SetFisheyeEnableStatus,
 * GetGOPSize, Get/SetChangeRef, SetChnHSkipBlackEnhance.  The expected
 * results are the return codes and stores of the vendor libimp (T21 1.0.33,
 * T20/T10 3.12.0) read from its disassembly.
 *
 * Includes src/t40/openimp_p2_encoder.c so the test can mark a channel
 * created without a codec; the codec calls are stubbed.
 * Build with -DPLATFORM_T21, or -DPLATFORM_T20 -DPLATFORM_T21 for T20/T10.
 */
#include "../../src/t40/openimp_p2_encoder.c"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    IMPEncoderAttrFrmUsed fu = { true, ENC_FRM_REUSED, 7 }, got;
    IMPEncoderGOPSizeCfg gop = { 1234 };
    int v = 99;

    EncoderInit();
    /* --- FrmUsedMode: three words stored as given, before create too --- */
    CHECK(IMP_Encoder_SetChnFrmUsedMode(0, &fu) == 0);
    memset(&got, 0, sizeof(got));
    CHECK(IMP_Encoder_GetChnFrmUsedMode(0, &got) == 0);
    CHECK(got.enable && got.frmUsedMode == ENC_FRM_REUSED &&
          got.frmUsedTimes == 7);
    CHECK(IMP_Encoder_SetChnFrmUsedMode(0, NULL) == -1);
    CHECK(IMP_Encoder_GetChnFrmUsedMode(0, NULL) == -1);
    CHECK(IMP_Encoder_SetChnFrmUsedMode(-1, &fu) == -1);
    CHECK(IMP_Encoder_GetChnFrmUsedMode(P2_MAX_CHANNELS, &got) == -1);
    memset(&got, 0xff, sizeof(got));
    CHECK(IMP_Encoder_GetChnFrmUsedMode(1, &got) == 0);   /* untouched = 0 */
    CHECK(!got.enable && got.frmUsedMode == 0 && got.frmUsedTimes == 0);

    /* --- idle channel ---------------------------------------------------- */
    CHECK(IMP_Encoder_SetFisheyeEnableStatus(0, 5) == 0);
    CHECK(IMP_Encoder_GetFisheyeEnableStatus(0, &v) == 0 && v == 1);
    CHECK(IMP_Encoder_SetFisheyeEnableStatus(0, 0) == 0);
    CHECK(IMP_Encoder_GetFisheyeEnableStatus(0, &v) == 0 && v == 0);
    CHECK(IMP_Encoder_GetFisheyeEnableStatus(0, NULL) == -1);
    CHECK(IMP_Encoder_SetChangeRef(1, 1) == -1);
    v = 99;
#if defined(PLATFORM_T20)
    CHECK(IMP_Encoder_GetChangeRef(1, &v) == -1);
    CHECK(IMP_Encoder_GetGOPSize(1, &gop) == -1 && gop.gopsize == 1234);
#else
    CHECK(IMP_Encoder_GetChangeRef(1, &v) == 0 && v == 0);
    CHECK(IMP_Encoder_GetGOPSize(1, &gop) == 0 && gop.gopsize == 1234);
#endif
    CHECK(IMP_Encoder_SetChnHSkipBlackEnhance(1, 1) == 0);
    CHECK(IMP_Encoder_SetChnHSkipBlackEnhance(P2_MAX_CHANNELS, 1) == -1);

    /* --- created channel ------------------------------------------------- */
    p2_channels[2].created = 1;
    p2_channels[2].attr.rcAttr.maxGop = 50;
    CHECK(IMP_Encoder_SetFisheyeEnableStatus(2, 1) == -1);    /* too late */
    CHECK(IMP_Encoder_GetFisheyeEnableStatus(2, &v) == 0 && v == 0);
    CHECK(IMP_Encoder_SetChangeRef(2, 3) == 0);
    CHECK(IMP_Encoder_GetChangeRef(2, &v) == 0 && v == 1);
    CHECK(IMP_Encoder_SetChangeRef(2, 0) == 0);
    CHECK(IMP_Encoder_GetChangeRef(2, &v) == 0 && v == 0);
    CHECK(IMP_Encoder_GetChangeRef(2, NULL) == -1);
    CHECK(IMP_Encoder_GetGOPSize(2, &gop) == 0 && gop.gopsize == 50);
    CHECK(IMP_Encoder_GetGOPSize(2, NULL) == -1);
    CHECK(IMP_Encoder_SetChnHSkipBlackEnhance(2, 9) == 0);
    CHECK(p2_channels[2].attr.rcAttr.attrHSkip.hSkipAttr.bBlackEnhance == 1);
    CHECK(IMP_Encoder_SetChnHSkipBlackEnhance(2, 0) == 0);
    CHECK(p2_channels[2].attr.rcAttr.attrHSkip.hSkipAttr.bBlackEnhance == 0);

    if (failures) {
        fprintf(stderr, "p2_gap_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("p2_gap_test: PASS");
    return 0;
}
