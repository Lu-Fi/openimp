/*
 * yuv_encode_test - the T40/T41 unbound encoder (IMP_Encoder_YuvInit/
 * YuvEncode/YuvExit, Vbm*, T41: the _Ex calls, SetIvpuBsSize and
 * SetAvpuJpegQp) against a stub codec and a fake rmem arena.  Argument
 * checks and return codes are the vendor's (T41 1.2.5 libimp.a
 * imp_encoder.c.o 0xb7d0.., 0x4b10, 0xb5dc).  Build with -DPLATFORM_T40 or
 * -DPLATFORM_T41.
 */
#define _GNU_SOURCE
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_encoder.h>
#include <imp/imp_system.h>
#include "../../src/t40/openimp_p2_encoder.c"

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

/* ---- fake FrameSource: one frame record per channel, always ready ---- */

typedef struct {
    int32_t index;
    int32_t pool_index;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t size;
    uint32_t physical_address;
    uint32_t virtual_address;
    int64_t timestamp;
} FakeFrame;

static FakeFrame fs_frames[4];
static uint8_t fs_pixels[4][640 * 368 * 3 / 2];
static int fs_gets[4];
static int fs_outstanding[4];

int IMP_FrameSource_GetFrame(int chn, void **frame);
int VBMGetFrame(int chn, void **frame) { return IMP_FrameSource_GetFrame(chn, frame); }
int OpenIMP_T40_FrameSource_TryGetFrame(int chn, void **frame) { return IMP_FrameSource_GetFrame(chn, frame); }
int OpenIMP_T40_FrameSource_WaitFrame(int chn, void **frame, uint32_t wait_ms) { (void)wait_ms; return IMP_FrameSource_GetFrame(chn, frame); }

int IMP_FrameSource_GetFrame(int chn, void **frame)
{
    if (chn < 0 || chn >= 4 || fs_outstanding[chn])
        return -1;
    fs_frames[chn].width = 640;
    fs_frames[chn].height = 360;
    fs_frames[chn].pixel_format = 0x3231564e;
    fs_frames[chn].size = sizeof(fs_pixels[chn]);
    fs_frames[chn].virtual_address = (uint32_t)(uintptr_t)fs_pixels[chn];
    fs_frames[chn].timestamp++;
    fs_gets[chn]++;
    fs_outstanding[chn] = 1;
    *frame = &fs_frames[chn];
    return 0;
}

int IMP_FrameSource_ReleaseFrame(int chn, void *frame)
{
    if (chn < 0 || chn >= 4 || frame != &fs_frames[chn])
        return -1;
    fs_outstanding[chn] = 0;
    return 0;
}

unsigned int VBMReadySequence(int chn) { (void)chn; return 0; }
int VBMWaitReady(int chn, unsigned int seq, uint32_t us)
{
    (void)chn; (void)seq; (void)us;
    return 0;
}
void VBMWakeReaders(int chn) { (void)chn; }
int OpenIMP_T31_HwJpegActive(void) { return 0; }
int openimp_t31_osd_apply(int group, void *frame) { (void)group; (void)frame; return 0; }
int openimp_t31_osd_apply_ex(int group, void *frame, unsigned int flags)
{ (void)group; (void)frame; (void)flags; return 0; }

/* ---- fake rmem: one arena, physical = virtual + RMEM_OFFSET ---- */
#define RMEM_SIZE (4u << 20)
#define RMEM_OFFSET 0x10000000u
static uint8_t rmem[RMEM_SIZE] __attribute__((aligned(4096)));
static uint32_t rmem_next;
static int rmem_live;
static int rmem_flushes;

static int in_rmem(const void *p)
{
    return (const uint8_t *)p >= rmem && (const uint8_t *)p < rmem + RMEM_SIZE;
}
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    uint32_t sz = ((uint32_t)size + 4095u) & ~4095u;

    (void)tag;
    if (rmem_next + sz > RMEM_SIZE)
        return -1;
    memset(info, 0, sizeof(*info));
    info->virt_addr = (uint32_t)(uintptr_t)(rmem + rmem_next);
    info->phys_addr = info->virt_addr + RMEM_OFFSET;
    rmem_next += sz;
    rmem_live++;
    return 0;
}
int DMA_FreePhys(uint32_t phys) { (void)phys; rmem_live--; return 0; }
uint32_t DMA_VirtToPhys(const void *v)
{
    return in_rmem(v) ? (uint32_t)(uintptr_t)v + RMEM_OFFSET
                      : (uint32_t)(uintptr_t)v;
}
void *DMA_PhysToVirt(uint32_t p)
{
    uint8_t *v = (uint8_t *)(uintptr_t)(p - RMEM_OFFSET);

    return in_rmem(v) ? v : NULL;
}
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    (void)size;
    if (in_rmem(virt) && dir == 1)
        rmem_flushes++;
    return 0;
}

/* ---- stub codec: Process records the frame, GetStream returns one ---- */

/* the codec's stream record (P2HWStream) */
typedef struct {
    uint32_t phys_addr;
    uint32_t virt_addr;
    uint32_t length;
    uint64_t timestamp;
    uint32_t frame_type;
    uint32_t slice_type;
    uint32_t reserved[8];
} StubStream;

typedef struct {
    int pending;
    int encoded;
    int skip_allowed;
    int skipped;
    StubStream hw;
    char jpeg[32];
} StubCodec;

/* stub_streams: GetStream hands out jpeg[] (non-PIE build: the 32-bit
 * stream addresses hold the pointers); stub_busy: a JPEG that may be
 * skipped is (AL_Codec_Encode_SetJpegSkip) */
static int stub_streams;
static int stub_busy;
static int stub_released;       /* streams given back to the codec */

int AL_Codec_Encode_Create(void **codec, void *params)
{
    (void)params;
    *codec = calloc(1, sizeof(StubCodec));
    return *codec ? 0 : -1;
}
int AL_Codec_Encode_Destroy(void *codec) { free(codec); return 0; }
static P2SyntheticFrame stub_last_frame;
static int stub_process_fail;
int AL_Codec_Encode_Process(void *codec, void *frame, void *user)
{
    StubCodec *c = codec;

    (void)user;
    if (!frame || stub_process_fail)
        return -1;
    stub_last_frame = *(P2SyntheticFrame *)frame;
    if (stub_busy && c->skip_allowed) {
        c->skipped = 1;
        return -1;
    }
    c->pending = 1;
    c->encoded++;
    return 0;
}
int AL_Codec_Encode_GetStream(void *codec, void **stream, void **user)
{
    StubCodec *c = codec;

    if (!c->pending)
        return 1;
    c->pending = 0;
    *user = NULL;
    if (!stub_streams) {
        *stream = NULL;         /* nothing to hand out: PollingStream's
                                   frame reached the codec, that is all
                                   the source checks need */
        return 0;
    }
    snprintf(c->jpeg, sizeof(c->jpeg), "JPEG#%d", c->encoded);
    memset(&c->hw, 0, sizeof(c->hw));
    c->hw.virt_addr = (uint32_t)(uintptr_t)c->jpeg;
    c->hw.length = (uint32_t)strlen(c->jpeg) + 1u;
    *stream = &c->hw;
    return 0;
}
int AL_Codec_Encode_ReleaseStream(void *codec, void *stream, void *user)
{
    StubCodec *c = codec;

    (void)user;
    if (stream != &c->hw)
        return -1;
    stub_released++;
    return 0;
}
int AL_Codec_Encode_SetJpegSrcPitch(void *codec, uint32_t pitch)
{
    (void)codec;
    (void)pitch;
    return 0;
}

int AL_Codec_Encode_SetJpegSkip(void *codec, int allow)
{
    StubCodec *c = codec;

    c->skip_allowed = allow;
    c->skipped = 0;
    return 0;
}
int AL_Codec_Encode_JpegSkipped(void *codec)
{
    return ((StubCodec *)codec)->skipped;
}
#define STUB0(name) int name(void *codec) { (void)codec; return 0; }
#define STUB1(name, t) int name(void *codec, t a) { (void)codec; (void)a; return 0; }
STUB0(AL_Codec_Encode_RequestIDR)
int AL_Codec_Encode_SetBitRate(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
STUB1(AL_Codec_Encode_SetEntropyMode, int)
STUB1(AL_Codec_Encode_SetGopLength, int)
static int stub_jpeg_quality;
int AL_Codec_Encode_SetJpegQuality(void *c, int q) { (void)c; stub_jpeg_quality = q; return 0; }
int AL_Codec_Encode_SetQp(void *c, void *a) { (void)c; (void)a; return 0; }
static int stub_stream_count;
int AL_Codec_Encode_SetStreamBufferCount(void *codec, int a)
{ (void)codec; stub_stream_count = a; return 0; }
#if defined(PLATFORM_T41)
/* the codec side of IMP_Encoder_SetAvpuBsShare: only the state the encoder
 * layer asks for (the allocation and the copy are device-tested) */
static uint32_t stub_bs_size;
static int stub_bs_fit = 1;
int AL_Codec_Encode_SetAvpuBsShare(int enable, uint32_t size)
{
    stub_bs_size = enable ? (size ? size : 0x23b700u) : 0;
    return 0;
}
uint32_t AL_Codec_Encode_BsShareSize(void) { return stub_bs_size; }
int AL_Codec_Encode_CheckBsShare(void *codec)
{ (void)codec; return stub_bs_size && !stub_bs_fit ? -1 : 0; }
#endif
STUB1(AL_Codec_Encode_SetStreamBufferSize, int)
int AL_Codec_Encode_SetDefaultParam(void *params) { (void)params; return 0; }
int AL_Codec_Encode_SetFrameRate(void *c, void *a) { (void)c; (void)a; return 0; }
int AL_Codec_Encode_SetGopParam(void *c, void *p) { (void)c; (void)p; return 0; }
int AL_Codec_Encode_SetQpBounds(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetQpIPDelta(void *c, int a) { (void)c; (void)a; return 0; }
#if defined(PLATFORM_T41)
int AL_Codec_Encode_SetQpBoundsPerFrame(void *c, int a, int b, int d, int e) { (void)c; (void)a; (void)b; (void)d; (void)e; return 0; }
int AL_Codec_Encode_SetMaxPictureSizeT41(void *c, uint32_t a, uint32_t b, int d) { (void)c; (void)a; (void)b; (void)d; return 0; }
int AL_Codec_Encode_SetRcOptionsT41(void *c, uint32_t a) { (void)c; (void)a; return 0; }
#endif
int AL_Codec_Encode_SetRcParam(void *c, void *p) { (void)c; (void)p; return 0; }
int AL_Codec_Encode_SetRcQualityCap(void *c, int m, unsigned int p) { (void)c; (void)m; (void)p; return 0; }

int AL_Codec_Encode_SetRoiAttr(void *c, const void *a) { (void)c; (void)a; return 0; }
#if defined(PLATFORM_T40)
int AL_Codec_Encode_SetSei(void *c, int m, const uint8_t *u, const uint8_t *d, uint32_t n);
int AL_Codec_Encode_SetSei(void *c, int m, const uint8_t *u, const uint8_t *d, uint32_t n) { (void)c; (void)m; (void)u; (void)d; (void)n; return 0; }
#endif
int AL_Codec_Encode_SetMapRoi(void *c, const void *a) { (void)c; (void)a; return 0; }
int AL_Codec_Encode_SetJpegQl(void *c, int e, const uint8_t t[128])
{ (void)c; (void)e; (void)t; return 0; }
void openimp_t23_osd_apply(int group, void *frame) { (void)group; (void)frame; }

/* ---- test ---- */

static void yuv_in(IMPEncoderYuvIn *in, IMPEncoderEncType type)
{
    memset(in, 0, sizeof(*in));
    in->type = type;
    in->mode = IMP_ENC_RC_MODE_CBR;
    in->frameRate = 25;
    in->gopLength = 50;
    in->targetBitrate = 1000;
    in->maxBitrate = 1500;
    in->initQp = -1;
    in->minQp = 20;
    in->maxQp = 40;
}

static int public_created(void)
{
    int i, n = 0;

    for (i = 0; i < P2_MAX_CHANNELS; i++)
        n += p2_channels[i].created;
    return n;
}

int main(void)
{
    IMPEncoderYuvIn in;
    IMPEncoderYuvOut out;
    IMPFrameInfo frame;
    void *h = NULL, *hs[P2_YUV_SLOTS + 1];
    uint8_t outbuf[64];
    uint8_t *pic;
    int i;

    /* heap below 4 GiB: the codec records hold 32-bit addresses */
    mallopt(M_MMAP_THRESHOLD, 64 << 20);
    stub_streams = 1;
#if defined(PLATFORM_T41)
    /* before EncoderInit: SetIvpuBsSize takes a size, 0 is refused */
    CHECK(IMP_Encoder_SetIvpuBsSize(0) == (int)0x80040002u, "ivpu 0");
    CHECK(IMP_Encoder_SetIvpuBsSize(0x40000) == 0, "ivpu before init");
    CHECK(OpenIMP_P2_IvpuBsSize() == 0x40000, "ivpu kept");
    /* _Ex before the encoder exists in this process: ENOTSUP */
    {
        int eh = -1;

        yuv_in(&in, IMP_ENC_TYPE_AVC);
        errno = 0;
        CHECK(IMP_Encoder_YuvInit_Ex(&eh, 640, 368, &in) == -1 &&
              errno == ENOTSUP, "YuvInit_Ex without owner");
        CHECK(IMP_Encoder_VbmAlloc_Ex(4096, 4096) == NULL, "VbmAlloc_Ex");
    }
#endif
    /* vendor argument checks */
    yuv_in(&in, IMP_ENC_TYPE_AVC);
    CHECK(IMP_Encoder_YuvInit(&h, 641, 368, &in) == -1, "width %% 16");
    CHECK(IMP_Encoder_YuvInit(&h, 640, 364, &in) == -1, "height %% 8");
    CHECK(IMP_Encoder_YuvInit(NULL, 640, 368, &in) == -1, "NULL h");
    CHECK(IMP_Encoder_YuvInit(&h, 640, 368, NULL) == -1, "NULL in");
    in.type = (IMPEncoderEncType)7;
    CHECK(IMP_Encoder_YuvInit(&h, 640, 368, &in) == -1, "bad type");
    CHECK(IMP_Encoder_YuvExit(NULL) == -1, "Exit NULL");
    CHECK(IMP_Encoder_YuvEncode(NULL, frame, &out) == -1, "Encode NULL");

    /* a hidden slot, no public channel */
    yuv_in(&in, IMP_ENC_TYPE_AVC);
    CHECK(IMP_Encoder_YuvInit(&h, 640, 360, &in) == 0 && h, "YuvInit 640x360");
    CHECK(public_created() == 0, "public channel used");
    {
        P2EncoderChannel *ch = &p2_channels[((P2YuvEncoder *)h)->slot];

        CHECK(ch->created && !ch->registered, "slot state");
        CHECK(ch->attr.rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_CBR,
              "rc mode %d", (int)ch->attr.rcAttr.attrRcMode.rcMode);
        CHECK(ch->attr.rcAttr.attrRcMode.attrCbr.uTargetBitRate == 1000 &&
              ch->attr.rcAttr.attrRcMode.attrCbr.iMinQP == 20 &&
              ch->attr.rcAttr.attrRcMode.attrCbr.iMaxQP == 40,
              "cbr attr");
        CHECK(ch->attr.gopAttr.uGopLength == 50, "gop");
    }
    CHECK(IMP_Encoder_CreateChn(P2_MAX_CHANNELS, NULL) == -1,
          "hidden slot not public");

    /* Vbm: page aligned rmem, V2P/P2V round trip, non-rmem -> 0 */
    pic = IMP_Encoder_VbmAlloc(640 * 368 * 3 / 2, 4096);
    CHECK(pic && in_rmem(pic) && ((uintptr_t)pic & 4095u) == 0, "VbmAlloc");
    CHECK(IMP_Encoder_VbmV2P((intptr_t)pic) ==
          (intptr_t)((uint32_t)(uintptr_t)pic + RMEM_OFFSET), "V2P");
    CHECK(IMP_Encoder_VbmP2V(IMP_Encoder_VbmV2P((intptr_t)pic)) ==
          (intptr_t)pic, "P2V");
    CHECK(IMP_Encoder_VbmV2P((intptr_t)outbuf) == 0, "V2P non-rmem");
    CHECK(IMP_Encoder_VbmAlloc(0, 0) == NULL, "size 0");
    CHECK(IMP_Encoder_VbmAlloc(4096, 3) == NULL, "align not pow2");
    CHECK(IMP_Encoder_VbmAlloc(4096, 8192) == NULL, "align > page");

    /* packed 640x360 (chroma at w*h): restaged into the 16-line layout */
    memset(pic, 0x55, 640 * 360);
    memset(pic + 640 * 360, 0xaa, 640 * 360 / 2);
    memset(&frame, 0, sizeof(frame));
    frame.width = 640;
    frame.height = 360;
    frame.size = 640 * 360 * 3 / 2;
    frame.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)pic);
    frame.virAddr = (uint32_t)(uintptr_t)pic;
    frame.timeStamp = 1234;
    memset(&out, 0, sizeof(out));
    out.outAddr = outbuf;
    out.outLen = sizeof(outbuf);
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == 0, "encode packed");
    CHECK(out.outLen == 7 && memcmp(outbuf, "JPEG#1", 7) == 0,
          "AU copied (len %u)", out.outLen);
    CHECK(stub_last_frame.physical_address != frame.phyAddr, "restaged");
    CHECK(stub_last_frame.width == 640 && stub_last_frame.height == 360 &&
          stub_last_frame.pixel_format == 0x3231564eu &&
          stub_last_frame.timestamp == 1234 &&
          stub_last_frame.size == 640 * 368 * 3 / 2, "frame record");
    {
        const uint8_t *st = (const uint8_t *)(uintptr_t)
            stub_last_frame.virtual_address;

        CHECK(st[640 * 360 - 1] == 0x55 && st[640 * 368] == 0xaa &&
              st[640 * 368 + 640 * 180 - 1] == 0xaa, "staged planes");
    }
    /* full 16-line layout in rmem: read in place, flushed first */
    frame.size = 640 * 368 * 3 / 2;
    i = rmem_flushes;
    out.outLen = sizeof(outbuf);
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == 0, "encode in place");
    CHECK(stub_last_frame.physical_address == frame.phyAddr, "in place");
    CHECK(rmem_flushes == i + 1, "flushed");
    /* capacity: outLen 3 cannot hold the AU (beyond vendor) */
    out.outLen = 3;
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == -1 && out.outLen == 0,
          "capacity");
    /* outLen 0 = no capacity given (vendor behaviour) */
    out.outLen = 0;
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == 0 && out.outLen == 7,
          "no capacity");
    /* no physical address: taken from the rmem virtual address */
    frame.phyAddr = 0;
    out.outLen = sizeof(outbuf);
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == 0 &&
          stub_last_frame.physical_address ==
              (uint32_t)(uintptr_t)pic + RMEM_OFFSET, "virt only");
    stub_process_fail = 1;
    CHECK(IMP_Encoder_YuvEncode(h, frame, &out) == -1, "codec failure");
    stub_process_fail = 0;

    /* the slots run out, Exit gives one back */
    hs[0] = h;
    for (i = 1; i < P2_YUV_SLOTS; i++)
        CHECK(IMP_Encoder_YuvInit(&hs[i], 640, 368, &in) == 0, "slot %d", i);
    CHECK(IMP_Encoder_YuvInit(&hs[P2_YUV_SLOTS], 640, 368, &in) == -1,
          "slots exhausted");
    CHECK(IMP_Encoder_YuvExit(hs[1]) == 0, "exit");
    CHECK(IMP_Encoder_YuvInit(&hs[1], 640, 368, &in) == 0, "slot reused");
    for (i = 0; i < P2_YUV_SLOTS; i++)
        CHECK(IMP_Encoder_YuvExit(hs[i]) == 0, "exit %d", i);
    CHECK(public_created() == 0, "public channels untouched");

    /* JPEG through the unbound encoder: FIXQP with initQp as the quality */
    yuv_in(&in, IMP_ENC_TYPE_JPEG);
    in.initQp = 80;
    CHECK(IMP_Encoder_YuvInit(&h, 640, 368, &in) == 0, "jpeg init");
    CHECK(stub_jpeg_quality == 80, "jpeg quality %d", stub_jpeg_quality);
    CHECK(IMP_Encoder_YuvExit(h) == 0, "jpeg exit");
    IMP_Encoder_VbmFree(pic);

#if defined(PLATFORM_T41)
    CHECK(IMP_Encoder_SetIvpuBsSize(0x1000) == (int)0x80040040u,
          "ivpu after init");
    /* SetAvpuJpegQp */
    {
        IMPEncoderCHNAttr attr;

        CHECK(IMP_Encoder_SetAvpuJpegQp(8, 50) == (int)0x80040001u, "chn 8");
        CHECK(IMP_Encoder_SetAvpuJpegQp(0, 101) == (int)0x80040002u, "qp 101");
        CHECK(IMP_Encoder_SetAvpuJpegQp(0, 50) == (int)0x80040008u, "no chn");
        IMP_Encoder_SetDefaultParam(&attr, IMP_ENC_PROFILE_AVC_MAIN,
                                    IMP_ENC_RC_MODE_CBR, 640, 368, 25, 1, 50,
                                    2, -1, 1000);
        CHECK(IMP_Encoder_CreateChn(0, &attr) == 0, "avc chn");
        CHECK(IMP_Encoder_SetAvpuJpegQp(0, 50) == (int)0x80040002u, "not jpeg");
        IMP_Encoder_SetDefaultParam(&attr, IMP_ENC_PROFILE_JPEG,
                                    IMP_ENC_RC_MODE_FIXQP, 640, 368, 25, 1, 50,
                                    2, 75, 0);
        CHECK(IMP_Encoder_CreateChn(1, &attr) == 0, "jpeg chn");
        CHECK(IMP_Encoder_SetAvpuJpegQp(1, 33) == 0 && stub_jpeg_quality == 33,
              "jpeg qp");
        CHECK(IMP_Encoder_DestroyChn(0) == 0 && IMP_Encoder_DestroyChn(1) == 0,
              "destroy");
    }
    /* _Ex in the owning process: int handles */
    {
        int eh = -1;

        yuv_in(&in, IMP_ENC_TYPE_AVC);
        CHECK(IMP_Encoder_YuvInit_Ex(&eh, 640, 368, &in) == 0 && eh >= 0,
              "YuvInit_Ex");
        pic = IMP_Encoder_VbmAlloc_Ex(640 * 368 * 3 / 2, 4096);
        CHECK(pic != NULL, "VbmAlloc_Ex");
        memset(&frame, 0, sizeof(frame));
        frame.size = 640 * 368 * 3 / 2;
        frame.virAddr = (uint32_t)(uintptr_t)pic;
        out.outAddr = outbuf;
        out.outLen = sizeof(outbuf);
        CHECK(IMP_Encoder_YuvEncode_Ex(eh, frame, &out) == 0 &&
              out.outLen == 7, "YuvEncode_Ex");
        CHECK(IMP_Encoder_YuvEncode_Ex(eh + 1, frame, &out) == -1,
              "bad Ex handle");
        CHECK(IMP_Encoder_YuvExit_Ex(eh) == 0, "YuvExit_Ex");
        CHECK(IMP_Encoder_YuvExit_Ex(eh) == -1, "YuvExit_Ex twice");
        IMP_Encoder_VbmFree_Ex(pic);
    }
    /* InputJpege: NV12 with chroma at w*h, width % 32, height % 8 */
    {
        static uint8_t src[640 * 360 * 3 / 2];
        static uint8_t dst[640 * 360 * 2];
        int len = -1, len_q90 = -1;

        memset(src, 0x80, sizeof(src));
        for (i = 0; i < 640 * 360; i++)
            src[i] = (uint8_t)(i % 640);
        CHECK(IMP_Encoder_InputJpege(src, dst, 624, 360, 0, &len) == -1 &&
              len == 0, "width %% 32");
        CHECK(IMP_Encoder_InputJpege(src, dst, 640, 356, 0, &len) == -1,
              "height %% 8");
        CHECK(IMP_Encoder_InputJpege(src, dst, 640, 360, 0, NULL) == -1,
              "NULL length");
        CHECK(IMP_Encoder_InputJpege(src, dst, 640, 360, 0, &len) == 0 &&
              len > 200 && dst[0] == 0xff && dst[1] == 0xd8 &&
              dst[len - 2] == 0xff && dst[len - 1] == 0xd9, "jpeg %d", len);
        CHECK(IMP_Encoder_InputJpege(src, dst, 640, 360, 90, &len_q90) == 0 &&
              len_q90 > len, "q 90 larger (%d vs %d)", len_q90, len);
        /* _Ex: height a multiple of 16 */
        CHECK(IMP_Encoder_InputJpege_Ex(src, dst, 640, 360, 0, &len) == -1,
              "Ex height %% 16");
        CHECK(IMP_Encoder_InputJpege_Ex(src, dst, 640, 352, 0, &len) == 0,
              "Ex 640x352");
        /* the IVPU bitstream size bounds the JPEG */
        p2_ivpu_bs_size = 64;
        CHECK(IMP_Encoder_InputJpege(src, dst, 640, 360, 0, &len) == -1,
              "ivpu size bound");
        p2_ivpu_bs_size = 0;
    }
#endif
    /* IMP_Decoder_*: ENOTSUP (no JPEG decoder on T40/T41) */
    {
        extern int IMP_Decoder_CreateChn(int decChn, const void *attr);
        extern int IMP_Decoder_GetFrame(int decChn, void **frame);
        void *df = (void *)1;

        errno = 0;
        CHECK(IMP_Decoder_CreateChn(0, NULL) == -1 && errno == ENOTSUP,
              "decoder ENOTSUP");
        CHECK(IMP_Decoder_GetFrame(0, &df) == -1 && df == NULL,
              "decoder frame");
    }
    CHECK(rmem_live == 0, "rmem leak %d", rmem_live);
    if (failures) {
        fprintf(stderr, "yuv_encode_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("yuv_encode_test: ok\n");
    return 0;
}
