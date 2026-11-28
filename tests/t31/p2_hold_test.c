/*
 * p2_hold_test - #942 IMP_Encoder_SetMaxStreamCnt(N) on T31: the
 * application can hold exactly N distinct streams (vendor libimp 1.1.6:
 * CreateChn 0x936e0 sem_init(chn+0x178, 0, N), encode thread 0x8ece0
 * sem_trywait before each picture, ReleaseStream 0x8ebc0 sem_post).
 * With N held, PollingStream times out; a release (in any order) lets one
 * more picture through; the pack data of held streams is not overwritten.
 *
 * Stubs from p2_bufshare_test.c, with a stub codec that has as many stream
 * buffers as SetStreamBufferCount and refuses to encode with all of them out.
 */
#define _GNU_SOURCE
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
/* #974: StartRecvPic gives the ready frames published before it back */
static unsigned int stub_tag = 7;
static int stub_expire_calls, stub_expire_chn = -1;
static unsigned int stub_expire_tag;
unsigned int VBMReadyTag(int chn) { (void)chn; return stub_tag; }
int VBMExpireReadyUpTo(int chn, unsigned int tag)
{
    stub_expire_calls++;
    stub_expire_chn = chn;
    stub_expire_tag = tag;
    return 0;
}
int OpenIMP_T31_HwJpegActive(void) { return 0; }
int openimp_t31_osd_apply(int group, void *frame) { (void)group; (void)frame; return 0; }
int openimp_t31_osd_apply_ex(int group, void *frame, unsigned int flags)
{ (void)group; (void)frame; (void)flags; return 0; }

int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    (void)info; (void)size; (void)tag;
    return -1;                  /* JPEG copies stay in normal memory */
}
int DMA_FreePhys(uint32_t phys) { (void)phys; return 0; }
int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    (void)virt; (void)size; (void)dir;
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

#define STUB_SLOTS 16

typedef struct {
    int pending;
    int encoded;
    int skip_allowed;
    int skipped;
    int out[STUB_SLOTS];        /* stream slot handed to the encoder layer */
    StubStream hw[STUB_SLOTS];
    uint8_t data[STUB_SLOTS][16];
} StubCodec;

static int stub_released;       /* streams given back to the codec */
static int stub_out_max;        /* most slots out at once */
static int stub_count;          /* SetStreamBufferCount */

int AL_Codec_Encode_Create(void **codec, void *params)
{
    (void)params;
    *codec = calloc(1, sizeof(StubCodec));
    return *codec ? 0 : -1;
}
int AL_Codec_Encode_Destroy(void *codec) { free(codec); return 0; }
int AL_Codec_Encode_Process(void *codec, void *frame, void *user)
{
    StubCodec *c = codec;

    (void)user;
    if (!frame)
        return -1;
    c->pending = 1;
    c->encoded++;
    return 0;
}
/* a P slice per picture, the picture number in its payload; a codec with
 * every stream buffer out has nothing to encode into */
int AL_Codec_Encode_GetStream(void *codec, void **stream, void **user)
{
    StubCodec *c = codec;
    int i, n = 0;
    static const uint8_t nal[] = { 0, 0, 0, 1, 0x41, 0x9a, 0, 0 };

    if (!c->pending)
        return 1;
    for (i = 0; i < stub_count && i < STUB_SLOTS && c->out[i]; i++)
        ;
    if (i >= stub_count || i >= STUB_SLOTS) {
        fprintf(stderr, "stub: no free stream buffer (count %d)\n", stub_count);
        return 1;
    }
    c->pending = 0;
    c->out[i] = 1;
    memcpy(c->data[i], nal, sizeof(nal));
    c->data[i][6] = (uint8_t)c->encoded;
    c->data[i][7] = 0x80;
    memset(&c->hw[i], 0, sizeof(c->hw[i]));
    c->hw[i].virt_addr = (uint32_t)(uintptr_t)c->data[i];
    c->hw[i].length = sizeof(nal);
    c->hw[i].frame_type = 1;
    c->hw[i].reserved[3] = 1;
    *stream = &c->hw[i];
    *user = NULL;
    for (i = 0; i < STUB_SLOTS; i++)
        n += c->out[i];
    if (n > stub_out_max)
        stub_out_max = n;
    return 0;
}
int AL_Codec_Encode_ReleaseStream(void *codec, void *stream, void *user)
{
    StubCodec *c = codec;
    int i;

    (void)user;
    for (i = 0; i < STUB_SLOTS && stream != &c->hw[i]; i++)
        ;
    if (i == STUB_SLOTS || !c->out[i])
        return -1;
    c->out[i] = 0;
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
    (void)codec;
    (void)allow;
    return 0;
}
int AL_Codec_Encode_JpegSkipped(void *codec)
{
    (void)codec;
    return 0;
}
#define STUB0(name) int name(void *codec) { (void)codec; return 0; }
#define STUB1(name, t) int name(void *codec, t a) { (void)codec; (void)a; return 0; }
STUB0(AL_Codec_Encode_RequestIDR)
STUB0(AL_Codec_Encode_RestartGop)
int AL_Codec_Encode_SetBitRate(void *c, int a, int b) { (void)c; (void)a; (void)b; return 0; }
int AL_Codec_Encode_SetFisheye(void *c, int e) { (void)c; (void)e; return 0; }
int openimp_t31_enc_fisheye(int c) { (void)c; return 0; }
STUB1(AL_Codec_Encode_SetEntropyMode, int)
STUB1(AL_Codec_Encode_SetGopLength, int)
STUB1(AL_Codec_Encode_SetJpegQuality, int)
int AL_Codec_Encode_SetQp(void *c, void *a) { (void)c; (void)a; return 0; }
int AL_Codec_Encode_SetStreamBufferCount(void *codec, int a)
{ (void)codec; stub_count = a; return 0; }
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
#if defined(PLATFORM_T31)
int AL_Codec_Encode_SetMaxPictureSizeT31(void *c, uint32_t b) { (void)c; (void)b; return 0; }
#endif
#if defined(PLATFORM_T40) && !defined(PLATFORM_T41)
int AL_Codec_Encode_SetMaxPictureSizeT40(void *c, uint32_t b) { (void)c; (void)b; return 0; }
int AL_Codec_Encode_SetRcOptionsT40(void *c, uint32_t o) { (void)c; (void)o; return 0; }
#endif
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

extern int IMP_System_Bind(IMPCell *source, IMPCell *destination);

static int up(int n)
{
    IMPEncoderCHNAttr a;
    IMPCell fs = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };

    memset(&a, 0, sizeof(a));
    a.encAttr.profile = IMP_ENC_PROFILE_AVC_MAIN;
    a.encAttr.uWidth = 640;
    a.encAttr.uHeight = 360;
    a.rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_FIXQP;
    a.rcAttr.attrRcMode.attrFixQp.iInitialQP = 30;
    a.rcAttr.outFrmRate.frmRateNum = 100;   /* short pacing waits */
    a.rcAttr.outFrmRate.frmRateDen = 1;
    if (IMP_Encoder_CreateGroup(0) != 0)
        return -1;
    if (n > 0 && IMP_Encoder_SetMaxStreamCnt(0, n) != 0)
        return -2;
    if (IMP_Encoder_CreateChn(0, &a) != 0 || IMP_Encoder_RegisterChn(0, 0) != 0)
        return -3;
    if (IMP_System_Bind(&fs, &enc) != 0)
        return -4;
    return IMP_Encoder_StartRecvPic(0) != 0 ? -5 : 0;
}

static void down(void)
{
    IMPCell fs = { DEV_ID_FS, 0, 0 }, enc = { DEV_ID_ENC, 0, 0 };

    IMP_Encoder_StopRecvPic(0);
    CHECK(IMP_Encoder_UnRegisterChn(0) == 0, "unregister");
    CHECK(IMP_Encoder_DestroyChn(0) == 0, "destroy");
    IMP_System_UnBind(&fs, &enc);
    IMP_Encoder_DestroyGroup(0);
}

static int fetch(IMPEncoderStream *s)
{
    if (IMP_Encoder_PollingStream(0, 100) != 0)
        return -2;
    return IMP_Encoder_GetStream(0, s, false);
}

/* hold streams until the poll refuses; returns the count held */
static int hold_all(int n, IMPEncoderStream *s, int max)
{
    int h = 0, k;

    while (h < max && fetch(&s[h]) == 0)
        h++;
    for (k = 0; k < h; k++) {
        int j;

        CHECK(s[k].packCount == 1 && s[k].pack, "N=%d held %d packs %u", n, k, s[k].packCount);
        for (j = 0; j < k; j++)
            CHECK(s[j].pack != s[k].pack && s[j].seq != s[k].seq,
                  "N=%d held %d and %d share packs/seq", n, j, k);
    }
    return h;
}

int main(void)
{
    int n;

    for (n = 0; n <= 3; n++) {
        IMPEncoderStream s[8];
        uint8_t first[8];
        int h, k, want = n ? n : 2, rel0 = stub_released, r;

        memset(s, 0, sizeof(s));
        stub_out_max = 0;
        stub_expire_calls = 0;
        stub_tag = 7u + (unsigned int)n;
        r = up(n);
        CHECK(stub_expire_calls == 1 && stub_expire_chn == 0 && stub_expire_tag == stub_tag,
              "N=%d #974 StartRecvPic gave back old frames: calls %d chn %d tag %u", n, stub_expire_calls,
              stub_expire_chn, stub_expire_tag);
        CHECK(r == 0, "N=%d up %d", n, r);
        h = hold_all(n, s, 6);
        printf("N=%d: held %d (vendor %d)\n", n, h, want);
        CHECK(h == want, "N=%d (0 = default): held %d, vendor %d", n, h, want);
        CHECK(stub_out_max == want, "N=%d codec buffers out at once %d", n, stub_out_max);
        for (k = 0; k < h; k++)
            first[k] = ((const uint8_t *)(uintptr_t)s[k].virAddr)[s[k].pack[0].offset + 6];
        for (k = 1; k < h; k++)
            CHECK(first[k] != first[k - 1], "N=%d streams %d/%d same picture", n, k - 1, k);
        CHECK(IMP_Encoder_GetStream(0, &s[7], false) != 0, "N=%d GetStream with all held", n);
        if (h >= 2) {
            /* release the oldest first: one more picture, the newest stays intact */
            const IMPEncoderPack *p = s[h - 1].pack;
            uint32_t len = p[0].length;

            CHECK(IMP_Encoder_ReleaseStream(0, &s[0]) == 0, "N=%d release oldest", n);
            CHECK(fetch(&s[0]) == 0, "N=%d poll after a release", n);
            CHECK(fetch(&s[7]) != 0, "N=%d all held again", n);
            CHECK(s[h - 1].pack == p && p[0].length == len, "N=%d newest packs kept", n);
            CHECK(s[0].pack != s[h - 1].pack, "N=%d new stream own packs", n);
        }
        CHECK(IMP_Encoder_StopRecvPic(0) == 0, "N=%d stop", n);
        CHECK(IMP_Encoder_UnRegisterChn(0) != 0, "N=%d unregister with held streams refused", n);
        for (k = h - 1; k >= 0; k--)
            CHECK(IMP_Encoder_ReleaseStream(0, &s[k]) == 0, "N=%d release %d", n, k);
        CHECK(stub_released - rel0 == h + (h >= 2), "N=%d released %d", n, stub_released - rel0);
        stub_expire_calls = 0;
        CHECK(IMP_Encoder_StartRecvPic(0) == 0, "N=%d restart", n);
        CHECK(stub_expire_calls == 1, "N=%d #974 restart gave back old frames: calls %d", n, stub_expire_calls);
        /* a second StartRecvPic of a receiving channel takes nothing from its own queue */
        CHECK(IMP_Encoder_StartRecvPic(0) == 0 && stub_expire_calls == 1, "N=%d #974 repeated start: calls %d", n,
              stub_expire_calls);
        if (n == 0) {   /* #974: a second channel on the receiving source leaves the ready frames to it */
            IMPEncoderCHNAttr a2;

            memset(&a2, 0, sizeof(a2));
            a2.encAttr.profile = IMP_ENC_PROFILE_AVC_MAIN;
            a2.encAttr.uWidth = 640;
            a2.encAttr.uHeight = 360;
            a2.rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_FIXQP;
            a2.rcAttr.attrRcMode.attrFixQp.iInitialQP = 30;
            a2.rcAttr.outFrmRate.frmRateNum = 100;
            a2.rcAttr.outFrmRate.frmRateDen = 1;
            CHECK(IMP_Encoder_CreateChn(1, &a2) == 0 && IMP_Encoder_RegisterChn(0, 1) == 0, "#974 second channel up");
            CHECK(IMP_Encoder_StartRecvPic(1) == 0 && stub_expire_calls == 1,
                  "#974 second channel on a receiving source: give-back calls %d (want none)", stub_expire_calls - 1);
            CHECK(IMP_Encoder_StopRecvPic(1) == 0 && IMP_Encoder_UnRegisterChn(1) == 0 && IMP_Encoder_DestroyChn(1) == 0,
                  "#974 second channel down");
        }
        /* normal flow: poll/get/release keeps one buffer out */
        stub_out_max = 0;
        for (k = 0; k < 20; k++) {
            CHECK(fetch(&s[0]) == 0, "N=%d flow %d", n, k);
            CHECK(IMP_Encoder_ReleaseStream(0, &s[0]) == 0, "N=%d flow release %d", n, k);
        }
        CHECK(stub_out_max == 1, "N=%d flow buffers out %d", n, stub_out_max);
        down();
    }
    if (failures) {
        fprintf(stderr, "p2_hold_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("p2_hold_test: ok\n");
    return 0;
}
