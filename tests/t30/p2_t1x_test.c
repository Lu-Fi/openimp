/*
 * p2_t1x_test - T20/T21 (T10) encoder channel extras through the P2 API
 * (src/t40/openimp_p2_encoder.c against a stub codec):
 *
 *  - Color2Grey and MbRC reach the codec (HWEncoderParams);
 *  - SuperFrameCfg: T20 hands NONE/REENCODE and the thresholds to the
 *    codec, refuses DISCARD and reports the OEM defaults before a Set;
 *    T21 accepts NONE only;
 *  - ROI, Denoise, H264TransCfg, QpgMode: settings the encoder cannot
 *    apply return -1, the ones it already follows are stored.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>
#include "dma_alloc.h"

int IMP_Encoder_SetChnColor2Grey(int, const IMPEncoderColor2GreyCfg *);
int IMP_Encoder_GetChnColor2Grey(int, IMPEncoderColor2GreyCfg *);
int IMP_Encoder_SetChnROI(int, const IMPEncoderROICfg *);
int IMP_Encoder_SetChnDenoise(int, const IMPEncoderAttrDenoise *);
int IMP_Encoder_GetChnDenoise(int, IMPEncoderAttrDenoise *);
int IMP_Encoder_SetMbRC(int, int);
int IMP_Encoder_SetSuperFrameCfg(int, const IMPEncoderSuperFrmCfg *);
int IMP_Encoder_GetSuperFrameCfg(int, IMPEncoderSuperFrmCfg *);
int IMP_Encoder_SetH264TransCfg(int, const IMPEncoderH264TransCfg *);
int IMP_Encoder_SetQpgMode(int, const IMPEncoderQpgMode *);
int IMP_Encoder_GetQpgMode(int, IMPEncoderQpgMode *);
int IMP_Encoder_SetH265TransCfg(int, const IMPEncoderH265TransCfg *);
int IMP_Encoder_GetH265TransCfg(int, IMPEncoderH265TransCfg *);
int IMP_Encoder_SetJpegeQl(int, IMPEncoderJpegeQl *);
int IMP_Encoder_GetJpegeQl(int, IMPEncoderJpegeQl *);

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the rest of libimp ---- */
int IMP_FrameSource_GetFrame(int chn, void **frame) { (void)chn; (void)frame; return -1; }
int IMP_FrameSource_ReleaseFrame(int chn, void *frame) { (void)chn; (void)frame; return 0; }
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{ (void)info; (void)size; (void)tag; return -1; }
int DMA_FreePhys(uint32_t phys) { (void)phys; return 0; }
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{ (void)virt; (void)size; (void)dir; return 0; }
int OpenIMP_HelixJpeg_Available(void) { return 0; }
int openimp_t23_persist_enabled(void) { return 0; }
int openimp_t23_persist_write(const char *a, const void *b, unsigned int c)
{ (void)a; (void)b; (void)c; return 0; }
int openimp_t31_osd_apply_ex(int group, void *frame, unsigned int flags)
{ (void)group; (void)frame; (void)flags; return 0; }

/* ---- stub codec ---- */
static int c2g_calls, c2g_value = -1;
static int mbrc_calls, mbrc_value = -1;
static int sf_calls;
static uint32_t sf_mode, sf_i, sf_p;

int AL_Codec_Encode_Create(void **codec, void *params)
{ (void)params; *codec = calloc(1, 64); return *codec ? 0 : -1; }
int AL_Codec_Encode_Destroy(void *codec) { free(codec); return 0; }
int AL_Codec_Encode_Process(void *c, void *f, void *u) { (void)c; (void)f; (void)u; return -1; }
int AL_Codec_Encode_GetStream(void *c, void **s, void **u) { (void)c; (void)s; (void)u; return 1; }
int AL_Codec_Encode_ReleaseStream(void *c, void *s, void *u) { (void)c; (void)s; (void)u; return 0; }
int AL_Codec_Encode_JpegSkipped(void *c) { (void)c; return 0; }
int AL_Codec_Encode_SetJpegSkip(void *c, int a) { (void)c; (void)a; return 0; }
static int jql_calls, jql_en = -1;
static uint8_t jql_tab[128];
int AL_Codec_Encode_SetJpegQl(void *c, int a, const uint8_t *b)
{ (void)c; jql_calls++; jql_en = a; if (b) memcpy(jql_tab, b, sizeof(jql_tab)); return 0; }
#define STUB0(name) int name(void *codec) { (void)codec; return 0; }
#define STUB1(name, t) int name(void *codec, t a) { (void)codec; (void)a; return 0; }
STUB0(AL_Codec_Encode_RequestIDR)
STUB1(AL_Codec_Encode_SetEntropyMode, int)
STUB1(AL_Codec_Encode_SetGopLength, int)
STUB1(AL_Codec_Encode_SetQp, void *)
STUB1(AL_Codec_Encode_SetStreamBufferCount, int)
STUB1(AL_Codec_Encode_SetStreamBufferSize, int)
STUB1(AL_Codec_Encode_SetQpIPDelta, int)
STUB1(AL_Codec_Encode_SetRcParam, void *)
STUB1(AL_Codec_Encode_SetRcExtras, const void *)
STUB1(AL_Codec_Encode_SetFrameRate, void *)
STUB1(AL_Codec_Encode_SetGopParam, void *)
int AL_Codec_Encode_SetBitRate(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetQpBounds(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetDefaultParam(void *p) { (void)p; return 0; }
int AL_Codec_Encode_SetSameSceneGops(void *c, uint32_t g) { (void)c; (void)g; return 0; }
int AL_Codec_Encode_SetMbRC(void *c, int e) { (void)c; mbrc_calls++; mbrc_value = e; return 0; }
int AL_Codec_Encode_SetColor2Grey(void *c, int e) { (void)c; c2g_calls++; c2g_value = e; return 0; }
int AL_Codec_Encode_SetSuperFrame(void *c, uint32_t m, uint32_t i, uint32_t p)
{ (void)c; sf_calls++; sf_mode = m; sf_i = i; sf_p = p; return 0; }

static void make_attr(IMPEncoderCHNAttr *attr, int denoise)
{
    memset(attr, 0, sizeof(*attr));
    attr->encAttr.enType = PT_H264;
    attr->encAttr.profile = 1;
    attr->encAttr.picWidth = 640;
    attr->encAttr.picHeight = 360;
    attr->encAttr.bufSize = 0;
    attr->rcAttr.outFrmRate.frmRateNum = 25;
    attr->rcAttr.outFrmRate.frmRateDen = 1;
    attr->rcAttr.maxGop = 50;
    attr->rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_CBR;
    attr->rcAttr.attrRcMode.attrH264Cbr.maxQp = 45;
    attr->rcAttr.attrRcMode.attrH264Cbr.minQp = 20;
    attr->rcAttr.attrRcMode.attrH264Cbr.outBitRate = 1000;
    attr->rcAttr.attrDenoise.enable = denoise;
}

int main(void)
{
    IMPEncoderCHNAttr attr;
    IMPEncoderColor2GreyCfg c2g;
    IMPEncoderROICfg roi;
    IMPEncoderAttrDenoise dn;
    IMPEncoderSuperFrmCfg sf;
    IMPEncoderH264TransCfg tr;
    IMPEncoderQpgMode qpg;
    IMPEncoderJpegeQl jql;

    CHECK(IMP_Encoder_CreateGroup(0) == 0);
    make_attr(&attr, 0);
    CHECK(IMP_Encoder_CreateChn(0, &attr) == 0);
    make_attr(&attr, 1);
    CHECK(IMP_Encoder_CreateChn(1, &attr) == 0);

    /* Color2Grey */
    c2g.enable = 1;
    CHECK(IMP_Encoder_SetChnColor2Grey(0, &c2g) == 0);
    CHECK(c2g_calls == 1 && c2g_value == 1);
    memset(&c2g, 0, sizeof(c2g));
    CHECK(IMP_Encoder_GetChnColor2Grey(0, &c2g) == 0 && c2g.enable);
    c2g.enable = 0;
    CHECK(IMP_Encoder_SetChnColor2Grey(0, &c2g) == 0 && c2g_value == 0);

    /* MbRC */
    CHECK(IMP_Encoder_SetMbRC(0, 1) == 0 && mbrc_value == 1);
    CHECK(IMP_Encoder_SetMbRC(0, 2) == -1);

    /* SuperFrameCfg */
    memset(&sf, 0, sizeof(sf));
    CHECK(IMP_Encoder_GetSuperFrameCfg(0, &sf) == 0);
#if defined(PLATFORM_T20)
    CHECK(sf.superFrmMode == IMP_RC_SUPERFRM_REENCODE &&
          sf.superIFrmBitsThr == 19660800u &&
          sf.superPFrmBitsThr == 14043429u);
    sf.superFrmMode = IMP_RC_SUPERFRM_REENCODE;
    sf.superIFrmBitsThr = 800000u;
    sf.superPFrmBitsThr = 400000u;
    CHECK(IMP_Encoder_SetSuperFrameCfg(0, &sf) == 0);
    CHECK(sf_calls == 1 && sf_mode == 2u && sf_i == 800000u && sf_p == 400000u);
    sf.superFrmMode = IMP_RC_SUPERFRM_NONE;
    CHECK(IMP_Encoder_SetSuperFrameCfg(0, &sf) == 0 && sf_mode == 1u);
    sf.superFrmMode = IMP_RC_SUPERFRM_DISCARD;
    CHECK(IMP_Encoder_SetSuperFrameCfg(0, &sf) == -1 && sf_calls == 2);
    memset(&sf, 0, sizeof(sf));
    CHECK(IMP_Encoder_GetSuperFrameCfg(0, &sf) == 0 &&
          sf.superFrmMode == IMP_RC_SUPERFRM_NONE);
#else
    sf.superFrmMode = IMP_RC_SUPERFRM_REENCODE;
    CHECK(IMP_Encoder_SetSuperFrameCfg(0, &sf) == -1);
    sf.superFrmMode = IMP_RC_SUPERFRM_NONE;
    CHECK(IMP_Encoder_SetSuperFrameCfg(0, &sf) == 0);
    CHECK(sf_calls == 0);
#endif

    /* ROI: a disabled region is stored, an enabled one refused */
    memset(&roi, 0, sizeof(roi));
    roi.u32Index = 1;
    CHECK(IMP_Encoder_SetChnROI(0, &roi) == 0);
    roi.bEnable = 1;
    roi.s32Qp = -4;
    CHECK(IMP_Encoder_SetChnROI(0, &roi) == -1);

    /* Denoise: no effect on a channel created without it; refused where
     * it would act */
    memset(&dn, 0, sizeof(dn));
    dn.enable = 1;
    dn.dnType = 1;
    dn.dnIQp = 30;
    CHECK(IMP_Encoder_SetChnDenoise(0, &dn) == 0);
    CHECK(IMP_Encoder_SetChnDenoise(1, &dn) == -1);
    dn.dnType = 0;
    CHECK(IMP_Encoder_SetChnDenoise(1, &dn) == 0);
    memset(&dn, 0, sizeof(dn));
    CHECK(IMP_Encoder_GetChnDenoise(1, &dn) == 0 && dn.dnType == 0);

    /* H264TransCfg: the PPS offset 0 only */
    tr.chroma_qp_index_offset = 0;
    CHECK(IMP_Encoder_SetH264TransCfg(0, &tr) == 0);
    tr.chroma_qp_index_offset = 3;
    CHECK(IMP_Encoder_SetH264TransCfg(0, &tr) == -1);

    /* QpgMode (vendor T21): any value is kept and read back, no range
     * check; a channel that was not created answers 0 and keeps nothing */
    qpg = ENC_QPG_CLOSE;
    CHECK(IMP_Encoder_SetQpgMode(0, &qpg) == 0);
    qpg = ENC_QPG_SAS;
    CHECK(IMP_Encoder_SetQpgMode(0, &qpg) == 0);
    qpg = (IMPEncoderQpgMode)77;
    CHECK(IMP_Encoder_SetQpgMode(1, &qpg) == 0);
    qpg = ENC_QPG_CLOSE;
    CHECK(IMP_Encoder_GetQpgMode(0, &qpg) == 0 && qpg == ENC_QPG_SAS);
    CHECK(IMP_Encoder_GetQpgMode(1, &qpg) == 0 && (int)qpg == 77);
    qpg = ENC_QPG_SAS;
    CHECK(IMP_Encoder_SetQpgMode(5, &qpg) == 0);
    qpg = ENC_QPG_CLOSE;
    CHECK(IMP_Encoder_GetQpgMode(5, &qpg) == 0 && qpg == ENC_QPG_CLOSE);
    CHECK(IMP_Encoder_SetQpgMode(0, NULL) == -1 &&
          IMP_Encoder_SetQpgMode(99, &qpg) == -1);

    /* H265TransCfg (vendor T21): checked and dropped, Get returns zeros */
    {
        IMPEncoderH265TransCfg h265 = { 99, -99 };

        CHECK(IMP_Encoder_SetH265TransCfg(0, &h265) == 0);
        CHECK(IMP_Encoder_SetH265TransCfg(5, &h265) == 0);
        CHECK(IMP_Encoder_SetH265TransCfg(0, NULL) == -1);
        CHECK(IMP_Encoder_SetH265TransCfg(99, &h265) == -1);
        CHECK(IMP_Encoder_GetH265TransCfg(0, &h265) == 0 &&
              h265.chroma_cr_qp_offset == 0 && h265.chroma_cb_qp_offset == 0);
        CHECK(IMP_Encoder_GetH265TransCfg(0, NULL) == -1);
    }

    /* JpegeQl: applied to the running codec at once (the vendor hands it to
     * its JPEG core), stored for Get; bad channel / NULL fail */
    memset(&jql, 0, sizeof(jql));
    jql.user_ql_en = 1;
    memset(jql.qmem_table, 7, 128);
    CHECK(IMP_Encoder_SetJpegeQl(0, &jql) == 0);
    CHECK(jql_calls == 1 && jql_en == 1 && jql_tab[0] == 7 && jql_tab[127] == 7);
    memset(&jql, 0xaa, sizeof(jql));
    CHECK(IMP_Encoder_GetJpegeQl(0, &jql) == 0 && jql.user_ql_en &&
          jql.qmem_table[5] == 7);
    jql.user_ql_en = 0;
    CHECK(IMP_Encoder_SetJpegeQl(0, &jql) == 0 && jql_calls == 2 && jql_en == 0);
    CHECK(IMP_Encoder_SetJpegeQl(0, NULL) == -1);
    CHECK(IMP_Encoder_GetJpegeQl(0, NULL) == -1);
    CHECK(IMP_Encoder_SetJpegeQl(99, &jql) == -1 && jql_calls == 2);

    CHECK(IMP_Encoder_DestroyChn(0) == 0);
    CHECK(IMP_Encoder_DestroyChn(1) == 0);
    if (failures) {
        fprintf(stderr, "p2_t1x_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("p2_t1x_test: ok\n");
    return 0;
}
