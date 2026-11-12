/*
 * p2_multisection_test - T23 IMP_Encoder_SetMultiSectionMode 2 through
 * GetStream / ReleaseStream (src/t40/openimp_p2_encoder.c against a stub
 * codec that "codes" a fixed access unit), the OEM 1.3.0 behaviour
 * (t23/t23_multisection.h): a picture larger than the window comes out as
 * one stream per section (first: SPS/PPS/slice head, then one pack each),
 * all with the picture's seq, pack[0].frameEnd 0 until the last section;
 * ReleaseStream of an unfinished section keeps the picture, the last one
 * hands it back to the codec; a picture within one window is whole.  The
 * sections joined are the access unit, byte for byte.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>
#include "dma_alloc.h"
#include "t23/t23_multisection.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the coded picture ---- */
typedef struct {
    uint32_t phys_addr, virt_addr, length;
    uint64_t timestamp;
    uint32_t frame_type, slice_type, reserved[8];
} StubStream;
static uint8_t au[3 * 1024 * 1024];
static uint32_t au_len;
static StubStream stub_stream;
static IMPFrameInfo stub_frame;
static int frames_left, streams_out, streams_back, frames_back;

static void make_au(uint32_t slice_bytes, int idr)
{
    uint32_t i, n = 0;
    static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0x8d};
    static const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};

    if (idr) {
        memcpy(au + n, sps, sizeof(sps)); n += sizeof(sps);
        memcpy(au + n, pps, sizeof(pps)); n += sizeof(pps);
    }
    au[n++] = 0; au[n++] = 0; au[n++] = 0; au[n++] = 1;
    au[n++] = idr ? 0x65 : 0x41;
    for (i = 0; i < slice_bytes; i++)
        au[n++] = (uint8_t)(0x11 + (i * 7u) % 0xe0u);   /* no start code */
    au_len = n;
    memset(&stub_stream, 0, sizeof(stub_stream));
    stub_stream.virt_addr = (uint32_t)(uintptr_t)au;
    stub_stream.phys_addr = 0x02000000u;
    stub_stream.length = au_len;
    stub_stream.timestamp = 123456u;
}

int IMP_FrameSource_ReleaseFrame(int chn, void *frame)
{ (void)chn; (void)frame; frames_back++; return 0; }
int VBMGetFrame(int chn, void **frame)
{
    (void)chn;
    if (frames_left <= 0)
        return -1;
    frames_left--;
    *frame = &stub_frame;
    return 0;
}
int AL_Codec_Encode_Process(void *c, void *f, void *u)
{ (void)c; (void)f; (void)u; return 0; }
int AL_Codec_Encode_GetStream(void *c, void **s, void **u)
{ (void)c; *s = &stub_stream; *u = NULL; streams_out++; return 0; }
int AL_Codec_Encode_ReleaseStream(void *c, void *s, void *u)
{ (void)c; (void)s; (void)u; streams_back++; return 0; }

/* ---- the rest of libimp ---- */
int IMP_FrameSource_GetFrame(int chn, void **frame) { (void)chn; (void)frame; return -1; }
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
int AL_Codec_Encode_JpegSkipped(void *c) { (void)c; return 0; }
int AL_Codec_Encode_FrameLossDropped(void *c) { (void)c; return 0; }
/* T23 encoder waits on the VBM ready event (#151); no frame ever comes */
unsigned int VBMReadySequence(int chn) { (void)chn; return 0; }
int VBMWaitReady(int chn, unsigned int seq, uint32_t us)
{
    (void)chn; (void)seq; (void)us;
    usleep(1000);
    return -1;
}
void VBMWakeReaders(int chn) { (void)chn; }
int AL_Codec_Encode_InsertUserData(void *c, const void *d, unsigned int s, unsigned int n, unsigned int m)
{ (void)c; (void)d; (void)s; (void)n; (void)m; return 0; }
int AL_Codec_Encode_SetJpegSkip(void *c, int a) { (void)c; (void)a; return 0; }
int AL_Codec_Encode_SetJpegSrcPitch(void *c, uint32_t p) { (void)c; (void)p; return 0; }
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
STUB1(AL_Codec_Encode_SetFisheye, int)
STUB1(AL_Codec_Encode_SetFrameRate, void *)
STUB1(AL_Codec_Encode_SetGopParam, void *)
int AL_Codec_Encode_SetBitRate(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetQpBounds(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetDefaultParam(void *p) { (void)p; return 0; }
int AL_Codec_Encode_SetSameSceneGops(void *c, uint32_t g) { (void)c; (void)g; return 0; }
int AL_Codec_Encode_SetMbRC(void *c, int e) { (void)c; mbrc_calls++; mbrc_value = e; return 0; }
int AL_Codec_Encode_SetColor2Grey(void *c, int e) { (void)c; c2g_calls++; c2g_value = e; return 0; }
static uint8_t roi_entry[8][7];
static int roi_calls, chroma_calls, chroma_value;
int AL_Codec_Encode_SetRoi(void *c, uint32_t i, const uint8_t e[7])
{ (void)c; if (i >= 8u) return -1; roi_calls++; memcpy(roi_entry[i], e, 7); return 0; }
int AL_Codec_Encode_SetChromaQpOffset(void *c, int o)
{ (void)c; chroma_calls++; chroma_value = o; return 0; }
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


uint64_t IMP_System_GetTimeStamp(void) { return 0; }
int openimp_t23_osd_apply(int g, void *f) { (void)g; (void)f; return 0; }
#define T23STUB(name) int name() { return 0; }
T23STUB(openimp_t23_enc_push_fps)
T23STUB(openimp_t23_enc_push_rc)
T23STUB(openimp_t23_enc_push_gop)
T23STUB(openimp_t23_enc_set_lossthd)
int openimp_t23_enc_fisheye(int c) { (void)c; return 0; }
T23STUB(openimp_t23_enc_push_color2grey)
T23STUB(openimp_t23_enc_push_denoise)
T23STUB(openimp_t23_enc_push_mbrc)
T23STUB(openimp_t23_enc_push_superframe)
T23STUB(openimp_t23_enc_push_h264trans)
T23STUB(openimp_t23_enc_push_qpgmode)
T23STUB(openimp_t23_enc_push_roi)

static uint32_t section_bytes;
uint32_t openimp_t23_enc_section_bytes(void) { return section_bytes; }

static uint8_t joined[sizeof(au)];

/* one picture through GetStream/ReleaseStream; returns the stream count */
static int run_picture(uint32_t expect_sections)
{
    IMPEncoderStream st;
    uint32_t joined_len = 0, i, seq = 0;
    int n = 0, back = streams_back;

    frames_left = 1;
    for (;;) {
        memset(&st, 0, sizeof(st));
        if (IMP_Encoder_PollingStream(0, 1000) != 0 ||
            IMP_Encoder_GetStream(0, &st, 1) != 0) {
            CHECK(0);
            return n;
        }
        if (n == 0)
            seq = st.seq;
        CHECK(st.seq == seq);
        CHECK(st.packCount >= 1);
        for (i = 0; i < st.packCount; i++) {
            memcpy(joined + joined_len, (const void *)(uintptr_t)st.pack[i].virAddr,
                   st.pack[i].length);
            joined_len += st.pack[i].length;
            CHECK(st.pack[i].timestamp == 123456);
        }
        n++;
        if (expect_sections > 1 && n > 1)
            CHECK(st.packCount == 1 && st.pack[0].length <= section_bytes);
        if (st.packCount > 1)
            CHECK(st.pack[st.packCount - 1].frameEnd);
        if (section_bytes && !st.pack[0].frameEnd) {
            CHECK(n < (int)expect_sections);
            /* the stream stays: GetStream again gives the same section */
            CHECK(IMP_Encoder_ReleaseStream(0, &st) == 0);
            CHECK(streams_back == back);
            continue;
        }
        CHECK(IMP_Encoder_ReleaseStream(0, &st) == 0);
        break;
    }
    CHECK(n == (int)expect_sections);
    CHECK(streams_back == back + 1);
    CHECK(joined_len == au_len && memcmp(joined, au, au_len) == 0);
    return n;
}

int main(void)
{
    IMPEncoderCHNAttr attr;
    uint32_t begin, end;

    /* the pure rule */
    CHECK(t23_ms_mode(0) == 0 && t23_ms_mode(1) == 1 && t23_ms_mode(2) == 2 &&
          t23_ms_mode(3) == 1 && t23_ms_mode(-1) == 0);
    CHECK(t23_ms_section_bytes(1, 300 << 10) == 0);
    CHECK(t23_ms_section_bytes(2, 300 << 10) == 300u << 10);
    CHECK(t23_ms_section_bytes(2, (300 << 10) + 77) == 300u << 10);
    CHECK(t23_ms_section_bytes(2, 1000) == 0);
    make_au(1000, 1);
    CHECK(t23_ms_slice_offset(au, au_len) == 17);
    CHECK(t23_ms_sections(au, au_len, 1000) == 1);
    CHECK(t23_ms_sections(au, au_len, 999) == 2);
    t23_ms_range(au, au_len, 400, 0, &begin, &end);
    CHECK(begin == 0 && end == 17 + 5 + 400);
    t23_ms_range(au, au_len, 400, 2, &begin, &end);
    CHECK(begin == 17 + 5 + 800 && end == au_len);

    /* through the encoder */
    CHECK(IMP_Encoder_CreateGroup(0) == 0);
    section_bytes = 300u << 10;
    make_attr(&attr, 0);
    CHECK(IMP_Encoder_CreateChn(0, &attr) == 0);
    CHECK(IMP_Encoder_RegisterChn(0, 0) == 0);
    CHECK(IMP_Encoder_StartRecvPic(0) == 0);

    make_au(700u << 10, 1);             /* IDR: 3 sections */
    run_picture(3);
    make_au(600u << 10, 0);             /* P, exactly 2 windows */
    run_picture(2);
    make_au(1000, 0);                   /* small P: whole */
    run_picture(1);
    make_au(300u << 10, 1);             /* IDR slice data = one window */
    run_picture(1);
    CHECK(frames_back == 4 && streams_out == 4);

    CHECK(IMP_Encoder_StopRecvPic(0) == 0);
    CHECK(IMP_Encoder_UnRegisterChn(0) == 0);
    CHECK(IMP_Encoder_DestroyChn(0) == 0);

    /* mode off: one stream per picture as before (frameEnd on the last
     * pack only) */
    section_bytes = 0;
    CHECK(IMP_Encoder_CreateChn(0, &attr) == 0);
    CHECK(IMP_Encoder_RegisterChn(0, 0) == 0);
    CHECK(IMP_Encoder_StartRecvPic(0) == 0);
    make_au(700u << 10, 1);
    run_picture(1);
    CHECK(IMP_Encoder_StopRecvPic(0) == 0);
    CHECK(IMP_Encoder_UnRegisterChn(0) == 0);
    CHECK(IMP_Encoder_DestroyChn(0) == 0);
    if (failures) {
        fprintf(stderr, "p2_multisection_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("p2_multisection_test: ok\n");
    return 0;
}
