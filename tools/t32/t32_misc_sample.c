/*
 * T32 IVS / System-alloc / sysutils / audio probe for QEMU -M ingenic-t32
 * (#299, #298).  Built like t32_trace_sample.c (PLATFORM_T32, OpenIMP public
 * headers) and linked against whichever libimp / libsysutils the guest has,
 * so the same binary runs on vendor libimp 2.2.0 + libsysutils 2.0.0 and on
 * OpenIMP; the host diffs the "T32x" lines.  Use it through
 * tools/t32/qemu-ioctl-trace.sh with SAMPLE_SRC=tools/t32/t32_misc_sample.c.
 *
 *   ivsdirect N   move and base-move interfaces called directly (init,
 *                 T32_IVS_SENSE / T32_IVS_SKIP (move, default 4 / 0),
 *                 T32_BASE_SENSE / T32_BASE_SKIP / T32_BASE_REF (3 / 0 / 1),
 *                 preProcessSync, processAsync, getResult) on N synthetic
 *                 640x360 NV12 frames (a bright box moving over a fixed
 *                 noisy background, generated here): frame-exact output
 *                 lines "T32V <i> move <52 bits>" / "T32V <i> base ret ...
 *                 sum ..." for a vendor-vs-OpenIMP diff
 *   ivs N         ISP + FrameSource channel 0 (640x360) bound to IVS group 0,
 *                 a move channel over a 4x4 ROI grid, N results
 *                 ("T32V res <i> <16 bits>"); run with a moving QEMU scene
 *                 (SENSOR_OPTS=",scene=<y4m>")
 *   buf           IMP_Buf_* pools, IMP_System_MemPoolRequest/Free and
 *                 Read/WriteReg32: return codes and pool-relative offsets
 *                 ("T32B ...")
 *   su            SU_* against the guest's kernel nodes (RTC, ADC, keys,
 *                 cipher; no reboot, suspend or shutdown), each line with the
 *                 library that provided the function ("T32U ...")
 *   audio N       AI (+ AENC G.711) capture of N frames and AO playback of
 *                 N silent frames, volumes/gains; "T32A ..." (#298)
 *   dmic N        IMP_DMIC_* capture of N frames ("T32D ...")
 *   audiox N      vendor 2.x audio extras and timing (#329): AI device 1
 *                 (the vendor's LPF/DRC flags only act on device 1) in N-frame
 *                 phases (plain, volume 50, mute, LPF 500 Hz, DRC/EQ, NS,
 *                 algorithm order), AO tone phases (volume 50, NS, DRC/EQ)
 *                 with send/flush times and queue states ("T32X ..."); the AO
 *                 tone goes to the QEMU audiodev (a wav file), no real sound
 *   dmicx N       dmic N plus IMP_DMIC_Enable Ns/Agc/Hpf phases (#329/#330)
 *   acodec N      AENC/ADEC: N frames of a fixed synthetic 16 kHz PCM through
 *                 the built-in G.711A/G.711U/G.726/ADPCM encoders and back
 *                 through the decoders, plus a user encoder/decoder pair and
 *                 error returns; per-frame lengths and FNV-1a hashes
 *                 ("T32C ...", #298). No audio device is opened.
 *
 * usage: t32_trace_sample <mode> [n]   (installed under that name)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_audio.h>
#include <imp/imp_dmic.h>

#if !defined(PLATFORM_T32)
#error "build with -DPLATFORM_T32"
#endif

/* the vendor T32 2.x IVS objects (imp_ivs.h, imp_ivs_base_move.h of SDK 2.0.0) */
typedef struct T32Ivs T32Ivs;
struct T32Ivs {
    void *param;
    int paramSize;
    int pixfmt;
    int (*init)(T32Ivs *);
    void (*exit)(T32Ivs *);
    int (*preProcessSync)(T32Ivs *, IMPFrameInfo *);
    int (*processAsync)(T32Ivs *, IMPFrameInfo *);
    int (*getResult)(T32Ivs *, void **);
    int (*releaseResult)(T32Ivs *, void *);
    int (*getParam)(T32Ivs *, void *);
    int (*setParam)(T32Ivs *, void *);
    int (*flushFrame)(T32Ivs *);
    void *priv;
};
typedef struct {
    int skipFrameCnt, referenceNum, sadMode, sense;
    IMPFrameInfo frameInfo;
} T32BaseMoveParam;
typedef struct {
    int ret;
    uint8_t *data;
    int datalen;
    int64_t timeStamp;
} T32BaseMoveOutput;
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4   /* the target; tests/t32 builds it on the host too */
_Static_assert(sizeof(T32Ivs) == 52, "IMPIVSInterface");
_Static_assert(offsetof(IMP_IVS_MoveParam, frameInfo) == 0xd8, "MoveParam.frameInfo");
_Static_assert(offsetof(IMP_IVS_MoveParam, roiRect) == 0x108, "MoveParam.roiRect");
_Static_assert(sizeof(IMP_IVS_MoveParam) == 0x450, "MoveParam");
_Static_assert(sizeof(T32BaseMoveParam) == 0x40, "BaseMoveParam");
_Static_assert(sizeof(T32BaseMoveOutput) == 0x18, "BaseMoveOutput");
#endif

T32Ivs *IMP_IVS_CreateBaseMoveInterface(T32BaseMoveParam *param);
void IMP_IVS_DestroyBaseMoveInterface(T32Ivs *inf);

/* sysutils 2.x (su_base.h / su_adc.h / su_misc.h of SDK 2.0.0) */
typedef struct { int sec, min, hour, mday, mon, year; } T32SUTime;
typedef struct { char chr[96]; } T32SUInfo;
int SU_Base_GetVersion(T32SUInfo *v);
int SU_Base_GetModelNumber(T32SUInfo *v);
int SU_Base_GetDevID(T32SUInfo *v);
int SU_Base_GetTime(T32SUTime *t);
int SU_Base_SetTime(T32SUTime *t);
int SU_Base_SetAlarm(T32SUTime *t);
int SU_Base_GetAlarm(T32SUTime *t);
int SU_Base_EnableAlarm(void);
int SU_Base_DisableAlarm(void);
int SU_Base_PollingAlarm(uint32_t ms);
int SU_ADC_Init(void);
int SU_ADC_Exit(void);
int SU_ADC_EnableChn(uint32_t chn);
int SU_ADC_DisableChn(uint32_t chn);
int SU_ADC_GetChnValue(uint32_t chn, int *value);
int SU_Key_OpenEvent(void);
int SU_Key_CloseEvent(int fd);
int SU_CIPHER_Init(void);
int SU_CIPHER_Exit(void);

#define W 640
#define H 360

static void (*mark)(const char *);

static int step(const char *name, int rc)
{
    printf("T32S %-40s rc=%d\n", name, rc);
    fflush(stdout);
    return rc;
}
#define STEP(call) (mark ? mark(#call) : (void)0, step(#call, (call)))

/* ---- synthetic frames ----------------------------------------------------- */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 16;
}

/* frame i: a fixed textured background with +-2 noise, a 64x64 box of luma
 * 220 moving 24 px a frame along the top band and back */
static void synth_frame(uint8_t *nv12, int i)
{
    uint32_t seed = 0x1234u + (uint32_t)i * 7919u;
    int x, y, n = 40, pos = i % (2 * n), bx;

    if (pos >= n)
        pos = 2 * n - 1 - pos;
    bx = 16 + pos * 12;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int v = 60 + ((x / 32 + y / 32) & 1) * 40 + (int)(lcg(&seed) % 5) - 2;
            if (y >= 40 && y < 104 && x >= bx && x < bx + 64)
                v = 220;
            nv12[y * W + x] = (uint8_t)v;
        }
    memset(nv12 + W * H, 128, W * H / 2);
}

static void fill_info(IMPFrameInfo *f, uint8_t *buf, int i)
{
    memset(f, 0, sizeof(*f));
    f->index = i & 7;
    f->width = W;
    f->height = H;
    f->pixfmt = PIX_FMT_NV12;
    f->size = W * H * 3 / 2;
    f->virAddr = (uint32_t)(uintptr_t)buf;
    f->timeStamp = 1000000 + (int64_t)i * 66666;
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);

    return v && *v ? atoi(v) : def;
}

static void move_param(IMP_IVS_MoveParam *p, int sense)
{
    int r, c;

    memset(p, 0, sizeof(*p));
    p->skipFrameCnt = env_int("T32_IVS_SKIP", 0);
    p->frameInfo.width = W;
    p->frameInfo.height = H;
    p->roiRectCnt = 16;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            /* the vendor IMPRect is {p0, p1}; the generic one has the
             * same four ints as {x, y, width, height} */
            IMPRect *q = &p->roiRect[r * 4 + c];
            q->x = c * W / 4;
            q->y = r * H / 4;
            q->width = (c + 1) * W / 4 - 1;     /* p1.x */
            q->height = (r + 1) * H / 4 - 1;    /* p1.y */
            p->sense[r * 4 + c] = sense;
        }
}

static int ivs_direct(int n)
{
    static IMP_IVS_MoveParam mp;
    static T32BaseMoveParam bp;
    uint8_t *buf = malloc(W * H * 3 / 2);
    T32Ivs *mv, *bm;
    int i, rc;

    move_param(&mp, env_int("T32_IVS_SENSE", 4));
    memset(&bp, 0, sizeof(bp));
    bp.skipFrameCnt = env_int("T32_BASE_SKIP", 0);
    bp.referenceNum = env_int("T32_BASE_REF", 1);
    bp.sadMode = 0;
    bp.sense = env_int("T32_BASE_SENSE", 3);
    bp.frameInfo.width = W;
    bp.frameInfo.height = H;
    mv = (T32Ivs *)IMP_IVS_CreateMoveInterface(&mp);
    bm = IMP_IVS_CreateBaseMoveInterface(&bp);
    printf("T32V param base h %u\n", bp.frameInfo.height);
    printf("T32V create move %d base %d paramSize %d %d pixfmt %d %d\n", mv != NULL, bm != NULL,
           mv ? mv->paramSize : -1, bm ? bm->paramSize : -1, mv ? mv->pixfmt : -1,
           bm ? bm->pixfmt : -1);
    if (!buf || !mv || !bm)
        return 1;
    printf("T32V init move %d base %d\n", mv->init ? mv->init(mv) : -9, bm->init ? bm->init(bm) : -9);
    for (i = 0; i < n; i++) {
        IMPFrameInfo f;
        void *res = NULL;
        int k;

        synth_frame(buf, i);
        fill_info(&f, buf, i);
        rc = mv->preProcessSync ? mv->preProcessSync(mv, &f) : -9;
        fill_info(&f, buf, i);
        k = mv->processAsync ? mv->processAsync(mv, &f) : -9;
        printf("T32V %d move pre %d proc %d", i, rc, k);
        if (k == 0 && mv->getResult(mv, &res) == 0 && res) {
            IMP_IVS_MoveOutput *o = res;
            int r;
            printf(" ");
            for (r = 0; r < 16; r++)
                printf("%d", o->retRoi[r]);
            if (mv->releaseResult)
                mv->releaseResult(mv, res);
        }
        printf("\n");
        fill_info(&f, buf, i);
        rc = bm->preProcessSync ? bm->preProcessSync(bm, &f) : -9;
        fill_info(&f, buf, i);
        k = bm->processAsync ? bm->processAsync(bm, &f) : -9;
        printf("T32V %d base pre %d proc %d", i, rc, k);
        res = NULL;
        if (k == 0 && bm->getResult(bm, &res) == 0 && res) {
            T32BaseMoveOutput *o = res;
            uint32_t sum = 0, nz = 0;
            int j;
            for (j = 0; o->data && j < o->datalen; j++) {
                sum = sum * 31u + o->data[j];
                nz += o->data[j] != 0;
            }
            printf(" ret %d len %d nz %u sum %08x ts %lld", o->ret, o->datalen, nz, sum,
                   (long long)o->timeStamp);
            if (bm->releaseResult)
                bm->releaseResult(bm, res);
        }
        printf("\n");
        fflush(stdout);
    }
    if (mv->exit)
        mv->exit(mv);
    if (bm->exit)
        bm->exit(bm);
    IMP_IVS_DestroyMoveInterface((IMPIVSInterface *)mv);
    IMP_IVS_DestroyBaseMoveInterface(bm);
    free(buf);
    printf("T32S end ok=1\n");
    return 0;
}

/* ---- bound IVS ------------------------------------------------------------ */
static int isp_up(IMPSensorInfo *si)
{
    memset(si, 0, sizeof(*si));
    snprintf(si->name, sizeof si->name, "cv8001");
    si->cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si->i2c.type, sizeof si->i2c.type, "cv8001");
    si->i2c.addr = 0x35;
    si->rst_gpio = si->pwdn_gpio = si->power_gpio = -1;
    si->switch_gpio = -1;
    if (STEP(IMP_ISP_Open()) || STEP(IMP_ISP_AddSensor(IMPVI_MAIN, si)) ||
        STEP(IMP_ISP_EnableSensor(IMPVI_MAIN, si)) || STEP(IMP_System_Init()))
        return -1;
    STEP(IMP_ISP_EnableTuning());
    return 0;
}

static void isp_down(IMPSensorInfo *si)
{
    STEP(IMP_ISP_DisableTuning());
    STEP(IMP_System_Exit());
    STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
    STEP(IMP_ISP_DelSensor(IMPVI_MAIN, si));
    STEP(IMP_ISP_Close());
}

static int ivs_bound(int n)
{
    static IMP_IVS_MoveParam mp;
    IMPSensorInfo si;
    IMPFSChnAttr fs;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_IVS, 0, 0 };
    IMPIVSInterface *mv;
    int i, got = 0, moving = 0, ok = 0;
    int chn = getenv("T32_IVS_FS") ? atoi(getenv("T32_IVS_FS")) : 0;

    src.groupID = chn;
    if (isp_up(&si))
        goto fail;
    memset(&fs, 0, sizeof fs);
    fs.picWidth = fs.scaler.outwidth = W;
    fs.picHeight = fs.scaler.outheight = H;
    fs.pixFmt = PIX_FMT_NV12;
    fs.scaler.enable = 1;
    fs.outFrmRateNum = 15;
    fs.outFrmRateDen = 1;
    fs.nrVBs = 3;
    fs.type = FS_PHY_CHANNEL;
    move_param(&mp, 4);
    if (STEP(IMP_FrameSource_CreateChn(chn, &fs)) || STEP(IMP_FrameSource_SetChnAttr(chn, &fs)))
        goto fail;
    if (STEP(IMP_IVS_CreateGroup(0)) || STEP(IMP_System_Bind(&src, &dst)))
        goto fail;
    {
        IMPCell back;
        memset(&back, 0xff, sizeof back);
        printf("T32V getbind rc %d src %d-%d-%d\n", IMP_System_GetBindbyDest(&dst, &back),
               back.deviceID, back.groupID, back.outputID);
    }
    mv = IMP_IVS_CreateMoveInterface(&mp);
    if (!mv || STEP(IMP_IVS_CreateChn(0, mv)) || STEP(IMP_IVS_RegisterChn(0, 0)) ||
        STEP(IMP_IVS_StartRecvPic(0)) || STEP(IMP_FrameSource_EnableChn(chn)))
        goto fail;
    if (mark)
        mark("results");
    for (i = 0; i < n; i++) {
        IMP_IVS_MoveOutput *o = NULL;
        int r, any = 0;

        if (IMP_IVS_PollingResult(0, 3000) || IMP_IVS_GetResult(0, (void **)&o) || !o) {
            printf("T32V res %d none\n", i);
            continue;
        }
        printf("T32V res %d ", i);
        for (r = 0; r < 16; r++) {
            printf("%d", o->retRoi[r]);
            any |= o->retRoi[r];
        }
        printf("\n");
        fflush(stdout);
        got++;
        moving += any != 0;
        IMP_IVS_ReleaseResult(0, o);
    }
    printf("T32V results %d/%d moving %d\n", got, n, moving);
    {
        IMP_IVS_MoveParam q;
        memset(&q, 0, sizeof q);
        printf("T32V getparam rc %d roiRectCnt %d sense0 %d w %u\n", IMP_IVS_GetParam(0, &q),
               q.roiRectCnt, q.sense[0], q.frameInfo.width);
    }
    STEP(IMP_IVS_StopRecvPic(0));
    STEP(IMP_FrameSource_DisableChn(chn));
    STEP(IMP_IVS_UnRegisterChn(0));
    STEP(IMP_IVS_DestroyChn(0));
    STEP(IMP_System_UnBind(&src, &dst));
    STEP(IMP_IVS_DestroyGroup(0));
    IMP_IVS_DestroyMoveInterface(mv);
    STEP(IMP_FrameSource_DestroyChn(chn));
    isp_down(&si);
    ok = got == n;
    printf("T32S end ok=%d\n", ok);
    return ok ? 0 : 2;
fail:
    printf("T32S end failed\n");
    return 1;
}

/* ---- IMP_Buf / MemPool / registers --------------------------------------- */
static int buf_test(void)
{
    uint32_t id = 0, id2 = 0, pa0, pa;
    char stackbuf[16];
    void *a, *b, *c, *d, *e;

#define OFF(p) ((p) ? (long)(IMP_Buf_Vaddr2Paddr(p) - pa0) : -1L)
    printf("T32B create null %#x\n", (unsigned)IMP_Buf_CreatePool(NULL, 65536, "p0", 16));
    printf("T32B create noname %#x\n", (unsigned)IMP_Buf_CreatePool(&id, 65536, NULL, 16));
    printf("T32B create align0 %#x\n", (unsigned)IMP_Buf_CreatePool(&id, 65536, "p0", 0));
    printf("T32B create align8 %#x\n", (unsigned)IMP_Buf_CreatePool(&id, 65536, "p0", 8));
    printf("T32B create p0 %#x id!=0 %d\n", (unsigned)IMP_Buf_CreatePool(&id, 65536, "p0", 16), id != 0);
    printf("T32B create p1 %#x id!=0 %d differs %d\n", (unsigned)IMP_Buf_CreatePool(&id2, 8192, "p1", 64),
           id2 != 0, id2 != id);
    a = IMP_Buf_Alloc(id, "p0", 1000, "a", 16);
    pa0 = a ? IMP_Buf_Vaddr2Paddr(a) : 0;
    printf("T32B alloc a %d pa%%4096 %u va==p2v %d\n", a != NULL, pa0 % 4096,
           a && IMP_Buf_Paddr2Vaddr(pa0) == a);
    b = IMP_Buf_Alloc(id, "p0", 100, "b", 64);
    c = IMP_Buf_Alloc(id, "p0", 5000, "c", 4096);
    printf("T32B alloc b off %ld c off %ld\n", OFF(b), OFF(c));
    printf("T32B free b %#x\n", (unsigned)IMP_Buf_Free(id, b));
    d = IMP_Buf_Alloc(id, "p0", 50, "d", 16);
    e = IMP_Buf_Alloc(0, "p0", 32, "e", 16);
    printf("T32B alloc d off %ld e(byname) off %ld\n", OFF(d), OFF(e));
    printf("T32B alloc align8 %d noname %d big %d\n", IMP_Buf_Alloc(id, "p0", 32, "x", 8) != NULL,
           IMP_Buf_Alloc(id, "p0", 32, NULL, 16) != NULL, IMP_Buf_Alloc(id, "p0", 70000, "big", 16) != NULL);
    {
        void *r = IMP_Buf_Alloc(0, "nosuch", 4096, "root", 16);
        printf("T32B alloc root %d inpool %d v2p!=0 %d free %#x\n", r != NULL,
               r && IMP_Buf_Vaddr2Paddr(r) - pa0 < 65536, r && IMP_Buf_Vaddr2Paddr(r) != 0,
               r ? (unsigned)IMP_Buf_Free(0, r) : 0u);
    }
    {
        void *p1 = IMP_Buf_Alloc(id2, "p1", 100, "q", 64);
        printf("T32B alloc p1 %d inp0 %d\n", p1 != NULL, p1 && IMP_Buf_Vaddr2Paddr(p1) - pa0 < 65536);
        printf("T32B free p1 wrongid %#x\n", (unsigned)IMP_Buf_Free(id, p1));
    }
    printf("T32B v2p stack %u p2v 0x1000 %d\n", IMP_Buf_Vaddr2Paddr(stackbuf),
           IMP_Buf_Paddr2Vaddr(0x1000) != NULL);
    printf("T32B free stack %#x\n", (unsigned)IMP_Buf_Free(id, stackbuf));
    printf("T32B free d byglobal %#x\n", (unsigned)IMP_Buf_Free(0, d));
    printf("T32B free d again %#x\n", (unsigned)IMP_Buf_Free(id, d));
    printf("T32B free null %#x\n", (unsigned)IMP_Buf_Free(id, NULL));
    pa = c ? IMP_Buf_Vaddr2Paddr((uint8_t *)c + 100) : 0;
    printf("T32B v2p inner %ld\n", pa ? (long)(pa - pa0) : -1L);
    printf("T32B destroy busy %#x\n", (unsigned)IMP_Buf_DestroyPool(id, "p0"));
    printf("T32B destroy again %#x\n", (unsigned)IMP_Buf_DestroyPool(id, "p0"));
    printf("T32B destroy byname %#x\n", (unsigned)IMP_Buf_DestroyPool(0, "p1"));
    printf("T32B v2p after destroy %d\n", a ? IMP_Buf_Vaddr2Paddr(a) != 0 : -1);
    printf("T32B mempool req %d\n", IMP_System_MemPoolRequest(0, 1 << 20, "mp0"));
    printf("T32B mempool req again %d\n", IMP_System_MemPoolRequest(0, 1 << 20, "mp0"));
    printf("T32B mempool req neg %d\n", IMP_System_MemPoolRequest(-1, 1 << 20, "mpx"));
    printf("T32B mempool req 0size %d\n", IMP_System_MemPoolRequest(1, 0, "mpz"));
    printf("T32B mempool free %d\n", IMP_System_MemPoolFree(0));
    printf("T32B mempool free again %d\n", IMP_System_MemPoolFree(0));
    printf("T32B mempool free 5 %d\n", IMP_System_MemPoolFree(5));
    /* EFUSE / HARB0 CPU id word of the QEMU T32 board, read-only */
    printf("T32B reg cpuid %08x\n", IMP_System_ReadReg32(0x1300002c));
    printf("T32S end ok=1\n");
    return 0;
#undef OFF
}

/* ---- sysutils ------------------------------------------------------------- */
static const char *who(void *fn)
{
    Dl_info di;
    const char *p;

    if (!fn || !dladdr(fn, &di) || !di.dli_fname)
        return "?";
    p = strrchr(di.dli_fname, '/');
    return p ? p + 1 : di.dli_fname;
}

static int su_test(void)
{
    T32SUTime t, a, set = { 30, 15, 12, 11, 11, 2026 };
    T32SUInfo info;
    int rc, v = -1, fd;

#define SU(fn, call) do { rc = (call); printf("T32U %-24s %-14s rc=%d", #fn, who((void *)fn), rc); } while (0)
    SU(SU_Base_GetVersion, (memset(&info, 0, sizeof info), SU_Base_GetVersion(&info)));
    printf(" '%.32s'\n", info.chr);
    SU(SU_Base_GetModelNumber, (memset(&info, 0, sizeof info), SU_Base_GetModelNumber(&info)));
    printf(" '%.32s'\n", info.chr);
    SU(SU_Base_GetDevID, (memset(&info, 0, sizeof info), SU_Base_GetDevID(&info)));
    printf(" len %zu\n", strnlen(info.chr, sizeof info.chr));
    SU(SU_Base_SetTime, SU_Base_SetTime(&set));
    printf("\n");
    memset(&t, 0, sizeof t);
    SU(SU_Base_GetTime, SU_Base_GetTime(&t));
    printf(" %04d-%02d-%02d %02d:%02d (sec>=30 %d)\n", t.year, t.mon, t.mday, t.hour, t.min, t.sec >= 30);
    a = t;
    a.min += 2;
    SU(SU_Base_SetAlarm, SU_Base_SetAlarm(&a));
    printf("\n");
    memset(&a, 0, sizeof a);
    SU(SU_Base_GetAlarm, SU_Base_GetAlarm(&a));
    printf(" %02d:%02d:%02d\n", a.hour, a.min, a.sec);
    SU(SU_Base_EnableAlarm, SU_Base_EnableAlarm());
    printf("\n");
    SU(SU_Base_PollingAlarm, SU_Base_PollingAlarm(200));
    printf("\n");
    SU(SU_Base_DisableAlarm, SU_Base_DisableAlarm());
    printf("\n");
    SU(SU_ADC_Init, SU_ADC_Init());
    printf("\n");
    SU(SU_ADC_EnableChn, SU_ADC_EnableChn(0));
    printf("\n");
    SU(SU_ADC_GetChnValue, SU_ADC_GetChnValue(0, &v));
    printf(" value %d\n", v);
    SU(SU_ADC_DisableChn, SU_ADC_DisableChn(0));
    printf("\n");
    SU(SU_ADC_EnableChn, SU_ADC_EnableChn(9));
    printf(" (chn 9)\n");
    SU(SU_ADC_Exit, SU_ADC_Exit());
    printf("\n");
    SU(SU_Key_OpenEvent, fd = SU_Key_OpenEvent());
    printf("\n");
    if (fd >= 0)
        SU_Key_CloseEvent(fd);
    SU(SU_CIPHER_Init, SU_CIPHER_Init());
    printf("\n");
    if (rc == 0)
        SU_CIPHER_Exit();
    printf("T32S end ok=1\n");
    return 0;
#undef SU
}

/* ---- audio (#298) --------------------------------------------------------- */
static void pcm_stats(const char *tag, int i, const IMPAudioFrame *f)
{
    const int16_t *s = (const int16_t *)(uintptr_t)f->virAddr;
    int n = f->len / 2, k, peak = 0;
    int64_t sum = 0;
    int zc = 0;

    for (k = 0; s && k < n; k++) {
        int v = s[k] < 0 ? -s[k] : s[k];
        peak = v > peak ? v : peak;
        sum += v;
        if (k && ((s[k] < 0) != (s[k - 1] < 0)))
            zc++;
    }
    printf("T32A %s %d bw %d sm %d len %d seq %u peak %d mean %d zc %d\n", tag, i, f->bitwidth,
           f->soundmode, f->len, f->seq, peak, n ? (int)(sum / n) : 0, zc);
}

static int audio_test(int n)
{
    IMPAudioIOAttr attr;
    IMPAudioIChnParam cp;
    int i, got = 0, vol = -1, gain = -1, ok;
    static int16_t silence[320];

    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_AI_SetPubAttr(0, &attr));
    memset(&attr, 0, sizeof attr);
    STEP(IMP_AI_GetPubAttr(0, &attr));
    printf("T32A ai attr %d %d %d %d %d %d\n", attr.samplerate, attr.bitwidth, attr.soundmode,
           attr.frmNum, attr.numPerFrm, attr.chnCnt);
    STEP(IMP_AI_Enable(0));
    memset(&cp, 0, sizeof cp);
    cp.usrFrmDepth = 20;
    STEP(IMP_AI_SetChnParam(0, 0, &cp));
    STEP(IMP_AI_EnableChn(0, 0));
    STEP(IMP_AI_SetVol(0, 0, 70));
    STEP(IMP_AI_GetVol(0, 0, &vol));
    STEP(IMP_AI_SetGain(0, 0, 20));
    STEP(IMP_AI_GetGain(0, 0, &gain));
    printf("T32A ai vol %d gain %d\n", vol, gain);
    STEP(IMP_AI_SetVolMute(0, 0, 1));
    STEP(IMP_AI_SetVolMute(0, 0, 0));
    if (mark)
        mark("ai frames");
    for (i = 0; i < n; i++) {
        IMPAudioFrame f;
        if (IMP_AI_PollingFrame(0, 0, 1000) || IMP_AI_GetFrame(0, 0, &f, BLOCK)) {
            printf("T32A ai %d none\n", i);
            continue;
        }
        if (i < 3 || i == n - 1)
            pcm_stats("ai", i, &f);
        got++;
        IMP_AI_ReleaseFrame(0, 0, &f);
    }
    printf("T32A ai frames %d/%d\n", got, n);
    STEP(IMP_AI_DisableChn(0, 0));
    STEP(IMP_AI_Disable(0));
    ok = got == n;

    /* AO: silence only (no audible output anywhere; QEMU writes a wav) */
    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_AO_SetPubAttr(0, &attr));
    STEP(IMP_AO_Enable(0));
    STEP(IMP_AO_EnableChn(0, 0));
    STEP(IMP_AO_SetVol(0, 0, 0));
    vol = gain = -1;
    STEP(IMP_AO_GetVol(0, 0, &vol));
    STEP(IMP_AO_GetGain(0, 0, &gain));
    printf("T32A ao vol %d gain %d\n", vol, gain);
    STEP(IMP_AO_SetVolMute(0, 0, 1));
    STEP(IMP_AO_SetVolMute(0, 0, 0));
    if (mark)
        mark("ao frames");
    got = 0;
    for (i = 0; i < n; i++) {
        IMPAudioFrame f;
        memset(&f, 0, sizeof f);
        f.bitwidth = AUDIO_BIT_WIDTH_16;
        f.soundmode = AUDIO_SOUND_MODE_MONO;
        f.virAddr = (uint32_t *)silence;
        f.len = sizeof silence;
        got += IMP_AO_SendFrame(0, 0, &f, BLOCK) == 0;
    }
    printf("T32A ao frames %d/%d\n", got, n);
    {
        IMPAudioOChnState st;
        memset(&st, 0, sizeof st);
        STEP(IMP_AO_QueryChnStat(0, 0, &st));
        printf("T32A ao stat total %d free %d busy %d\n", st.chnTotalNum, st.chnFreeNum, st.chnBusyNum);
    }
    STEP(IMP_AO_FlushChnBuf(0, 0));
    STEP(IMP_AO_DisableChn(0, 0));
    STEP(IMP_AO_Disable(0));
    ok &= got == n;
    printf("T32S end ok=%d\n", ok);
    return ok ? 0 : 2;
}

static int dmic_test(int n)
{
    IMPDmicAttr attr;
    IMPDmicChnParam cp;
    int i, got = 0, vol = -1, gain = -1;

    memset(&attr, 0, sizeof attr);
    attr.samplerate = DMIC_SAMPLE_RATE_16000;
    attr.bitwidth = DMIC_BIT_WIDTH_16;
    attr.soundmode = DMIC_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_DMIC_SetUserInfo(0, 0, 0));
    STEP(IMP_DMIC_SetPubAttr(0, &attr));
    STEP(IMP_DMIC_Enable(0));
    memset(&cp, 0, sizeof cp);
    cp.usrFrmDepth = 20;
    STEP(IMP_DMIC_SetChnParam(0, 0, &cp));
    STEP(IMP_DMIC_EnableChn(0, 0));
    STEP(IMP_DMIC_SetVol(0, 0, 60));
    STEP(IMP_DMIC_GetVol(0, 0, &vol));
    STEP(IMP_DMIC_SetGain(0, 0, 22));
    STEP(IMP_DMIC_GetGain(0, 0, &gain));
    printf("T32D vol %d gain %d\n", vol, gain);
    if (mark)
        mark("dmic frames");
    for (i = 0; i < n; i++) {
        IMPDmicChnFrame f;
        if (IMP_DMIC_PollingFrame(0, 0, 1000) || IMP_DMIC_GetFrame(0, 0, &f, BLOCK)) {
            printf("T32D %d none\n", i);
            continue;
        }
        if (i < 3 || i == n - 1) {
            IMPAudioFrame a;
            memset(&a, 0, sizeof a);
            a.bitwidth = (IMPAudioBitWidth)f.rawFrame.bitwidth;
            a.soundmode = (IMPAudioSoundMode)f.rawFrame.soundmode;
            a.virAddr = f.rawFrame.virAddr;
            a.len = f.rawFrame.len;
            a.seq = f.rawFrame.seq;
            pcm_stats("dmic", i, &a);
        }
        got++;
        IMP_DMIC_ReleaseFrame(0, 0, &f);
    }
    printf("T32D frames %d/%d\n", got, n);
    STEP(IMP_DMIC_DisableChn(0, 0));
    STEP(IMP_DMIC_Disable(0));
    printf("T32S end ok=%d\n", got == n);
    return got == n ? 0 : 2;
}

/* ---- audio extras and timing (#329) -------------------------------------- */
static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* n frames from AI device dev: stats of the last one, frame spacing (us) */
static int ai_phase(int dev, const char *tag, int n)
{
    int64_t first = 0, last = 0, t0 = now_us(), wait_first = -1;
    int i, got = 0;

    for (i = 0; i < n; i++) {
        IMPAudioFrame f;

        if (IMP_AI_PollingFrame(dev, 0, 1000) || IMP_AI_GetFrame(dev, 0, &f, BLOCK)) {
            printf("T32X ai %s %d none\n", tag, i);
            continue;
        }
        if (!got) {
            first = f.timeStamp;
            wait_first = now_us() - t0;
        }
        last = f.timeStamp;
        if (i == n - 1)
            pcm_stats(tag, i, &f);
        got++;
        IMP_AI_ReleaseFrame(dev, 0, &f);
    }
    printf("T32X ai %s frames %d/%d spacing_us %lld first_wait_ms %lld\n", tag, got, n,
           got > 1 ? (long long)((last - first) / (got - 1)) : -1LL,
           (long long)(wait_first / 1000));
    return got == n;
}

static int ao_phase(const char *tag, int n, int16_t *pcm, int samples)
{
    IMPAudioOChnState st;
    int64_t t0 = now_us(), t1, t2;
    int i, got = 0;

    for (i = 0; i < n; i++) {
        IMPAudioFrame f;

        memset(&f, 0, sizeof f);
        f.bitwidth = AUDIO_BIT_WIDTH_16;
        f.soundmode = AUDIO_SOUND_MODE_MONO;
        f.virAddr = (uint32_t *)pcm;
        f.len = samples * 2;
        got += IMP_AO_SendFrame(0, 0, &f, BLOCK) == 0;
    }
    t1 = now_us();
    memset(&st, 0, sizeof st);
    IMP_AO_QueryChnStat(0, 0, &st);
    printf("T32X ao %s sent %d/%d send_ms %lld stat %d/%d/%d", tag, got, n,
           (long long)((t1 - t0) / 1000), st.chnTotalNum, st.chnFreeNum, st.chnBusyNum);
    i = IMP_AO_FlushChnBuf(0, 0);
    t2 = now_us();
    memset(&st, 0, sizeof st);
    IMP_AO_QueryChnStat(0, 0, &st);
    printf(" flush rc=%d ms %lld stat %d/%d/%d\n", i, (long long)((t2 - t1) / 1000),
           st.chnTotalNum, st.chnFreeNum, st.chnBusyNum);
    return got == n;
}

static int audiox_test(int n)
{
    static float eq_table[20];
    static int16_t tone[320], quiet[320];
    IMPAudioDrcEqConfig drc = { 500, 1000000, 0.3f, 0, 0, 8000, 20, eq_table };
    IMPAudioIOAttr attr;
    IMPAudioIChnParam cp;
    AudioAI_Type order[3] = { AI_MODULE_HPF, AI_MODULE_NS, AI_MODULE_AGC }, got_order[3];
    AudioAO_Type ao_order[3];
    int ok = 1, i, vol = -1;
    const int dev = 1;

    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_AI_SetPubAttr(dev, &attr));
    STEP(IMP_AI_GetPubAttr(dev, &attr));
    printf("T32X ai attr %d %d %d %d %d %d\n", attr.samplerate, attr.bitwidth, attr.soundmode,
           attr.frmNum, attr.numPerFrm, attr.chnCnt);
    STEP(IMP_AI_Enable(dev));
    memset(&cp, 0, sizeof cp);
    cp.usrFrmDepth = 20;
    STEP(IMP_AI_SetChnParam(dev, 0, &cp));
    STEP(IMP_AI_EnableChn(dev, 0));
    STEP(IMP_AI_GetVol(dev, 0, &vol));
    printf("T32X ai vol after EnableChn %d\n", vol);
    memset(got_order, 0xff, sizeof got_order);
    STEP(IMP_AI_GetAlgoOrder(dev, 0, got_order));
    printf("T32X ai order %d %d %d\n", got_order[0], got_order[1], got_order[2]);
    if (mark)
        mark("ai plain");
    ok &= ai_phase(dev, "plain", n);
    STEP(IMP_AI_SetVol(dev, 0, 50));
    ok &= ai_phase(dev, "vol50", n);
    STEP(IMP_AI_SetVolMute(dev, 0, 1));
    ok &= ai_phase(dev, "mute", n);
    STEP(IMP_AI_SetVolMute(dev, 0, 0));
    STEP(IMP_AI_GetVol(dev, 0, &vol));
    printf("T32X ai vol after unmute %d\n", vol);
    STEP(IMP_AI_SetVol(dev, 0, 60));
    STEP(IMP_AI_SetLpfCoFrequency(500));
    STEP(IMP_AI_EnableLpf(&attr));
    ok &= ai_phase(dev, "lpf500", n);
    STEP(IMP_AI_DisableLpf());
    STEP(IMP_AI_EnableDrcAndEq(&attr, &drc, 1, 1));
    ok &= ai_phase(dev, "drceq", n);
    STEP(IMP_AI_DisableDrcAndEq());
    STEP(IMP_AI_EnableNs(&attr, 2));
    ok &= ai_phase(dev, "ns2", n);
    STEP(IMP_AI_SetAlgoOrder(dev, 0, order));
    STEP(IMP_AI_GetAlgoOrder(dev, 0, got_order));
    printf("T32X ai order %d %d %d\n", got_order[0], got_order[1], got_order[2]);
    ok &= ai_phase(dev, "ns2-order", n);
    STEP(IMP_AI_DisableNs());
    ok &= ai_phase(dev, "after", n);
    STEP(IMP_AI_DisableChn(dev, 0));
    STEP(IMP_AI_Disable(dev));

    /* AO: a 1 kHz tone (-12 dBFS) into the QEMU wav, no device */
    for (i = 0; i < 320; i++) {
        static const int16_t q[16] = { 0, 3135, 5793, 7568, 8191, 7568, 5793, 3135,
                                       0, -3135, -5793, -7568, -8191, -7568, -5793, -3135 };
        tone[i] = q[i % 16];
    }
    memset(&attr, 0, sizeof attr);
    attr.samplerate = AUDIO_SAMPLE_RATE_16000;
    attr.bitwidth = AUDIO_BIT_WIDTH_16;
    attr.soundmode = AUDIO_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_AO_SetPubAttr(0, &attr));
    STEP(IMP_AO_GetPubAttr(0, &attr));
    printf("T32X ao attr numPerFrm %d\n", attr.numPerFrm);
    STEP(IMP_AO_Enable(0));
    STEP(IMP_AO_EnableChn(0, 0));
    STEP(IMP_AO_GetVol(0, 0, &vol));
    printf("T32X ao vol after EnableChn %d\n", vol);
    memset(ao_order, 0xff, sizeof ao_order);
    STEP(IMP_AO_GetAlgoOrder(0, 0, ao_order));
    printf("T32X ao order %d %d %d\n", ao_order[0], ao_order[1], ao_order[2]);
    if (mark)
        mark("ao phases");
    ok &= ao_phase("quiet", 5, quiet, 320);
    ok &= ao_phase("tone60", n, tone, 320);
    ok &= ao_phase("quiet", 5, quiet, 320);
    STEP(IMP_AO_SetVol(0, 0, 50));
    ok &= ao_phase("tone50", n, tone, 320);
    ok &= ao_phase("quiet", 5, quiet, 320);
    STEP(IMP_AO_SetVol(0, 0, 60));
    STEP(IMP_AO_EnableNs(&attr, 2));
    ok &= ao_phase("ns2", n, tone, 320);
    STEP(IMP_AO_DisableNs());
    ok &= ao_phase("quiet", 5, quiet, 320);
    STEP(IMP_AO_EnableDrcAndEq(&attr, &drc, 1, 1));
    ok &= ao_phase("drceq", n, tone, 320);
    STEP(IMP_AO_DisableDrcAndEq());
    ok &= ao_phase("quiet", 5, quiet, 320);
    STEP(IMP_AO_CacheSwitch(0, 0, 0));
    ok &= ao_phase("nocache", n, tone, 320);
    STEP(IMP_AO_DisableChn(0, 0));
    STEP(IMP_AO_Disable(0));
    printf("T32S end ok=%d\n", ok);
    return ok ? 0 : 2;
}

static int dmicx_phase(const char *tag, int n)
{
    int i, got = 0;

    for (i = 0; i < n; i++) {
        IMPDmicChnFrame f;

        if (IMP_DMIC_PollingFrame(0, 0, 1000) || IMP_DMIC_GetFrame(0, 0, &f, BLOCK)) {
            printf("T32D %s %d none\n", tag, i);
            continue;
        }
        if (i == n - 1) {
            IMPAudioFrame a;

            memset(&a, 0, sizeof a);
            a.virAddr = f.rawFrame.virAddr;
            a.len = f.rawFrame.len;
            a.seq = f.rawFrame.seq;
            pcm_stats(tag, i, &a);
        }
        got++;
        IMP_DMIC_ReleaseFrame(0, 0, &f);
    }
    printf("T32D %s frames %d/%d\n", tag, got, n);
    return got == n;
}

static int dmicx_test(int n)
{
    IMPDmicAttr attr;
    IMPDmicChnParam cp;
    IMPAudioAgcConfig agc = { 3, 15 };
    int ok = 1;

    memset(&attr, 0, sizeof attr);
    attr.samplerate = DMIC_SAMPLE_RATE_16000;
    attr.bitwidth = DMIC_BIT_WIDTH_16;
    attr.soundmode = DMIC_SOUND_MODE_MONO;
    attr.frmNum = 20;
    attr.numPerFrm = 320;
    attr.chnCnt = 1;
    STEP(IMP_DMIC_EnableNs(&attr, 2));          /* device not enabled yet */
    STEP(IMP_DMIC_SetUserInfo(0, 0, 0));
    STEP(IMP_DMIC_SetPubAttr(0, &attr));
    STEP(IMP_DMIC_Enable(0));
    memset(&cp, 0, sizeof cp);
    cp.usrFrmDepth = 20;
    STEP(IMP_DMIC_SetChnParam(0, 0, &cp));
    STEP(IMP_DMIC_EnableChn(0, 0));
    STEP(IMP_DMIC_SetVol(0, 0, 70));
    ok &= dmicx_phase("dmic-vol70", n);
    STEP(IMP_DMIC_SetVol(0, 0, 60));
    ok &= dmicx_phase("dmic-plain", n);
    STEP(IMP_DMIC_EnableNs(&attr, 2));
    ok &= dmicx_phase("dmic-ns2", n);
    STEP(IMP_DMIC_DisableNs());
    STEP(IMP_DMIC_EnableAgc(&attr, agc));
    ok &= dmicx_phase("dmic-agc", n);
    STEP(IMP_DMIC_DisableAgc());
    STEP(IMP_DMIC_SetHpfCoFrequency(0));
    STEP(IMP_DMIC_EnableHpf(&attr));
    ok &= dmicx_phase("dmic-hpf", n);
    STEP(IMP_DMIC_DisableHpf());
    ok &= dmicx_phase("dmic-after", n);
    STEP(IMP_DMIC_DisableChn(0, 0));
    STEP(IMP_DMIC_Disable(0));
    printf("T32S end ok=%d\n", ok);
    return ok ? 0 : 2;
}

/* ---- AENC / ADEC (#298) ---------------------------------------------------- */
static uint32_t fnv(const void *p, int len)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; b && i < len; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* fixed PCM: a slow sweep with noise, peaks near full scale (exercises the
 * codecs' segment/step tables over their whole range) */
static void acodec_pcm(int16_t *pcm, int samples, int frame)
{
    static uint32_t seed = 12345;
    static int phase;
    int k;

    if (frame == 0) {
        seed = 12345;
        phase = 0;
    }
    for (k = 0; k < samples; k++) {
        int t = phase++;
        int tri = (t * (3 + frame)) & 0xffff;               /* sawtooth, rising pitch */
        int v = (tri < 0x8000 ? tri : 0xffff - tri) * 2 - 0x8000;
        int amp = 1 + ((t >> 9) & 7);                        /* 1/8 .. 8/8 */
        v = v * amp / 8 + (int)(lcg(&seed) >> 22) - 512;
        pcm[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

#define ACODEC_SAMPLES 320
#define ACODEC_MAXSTREAM 1024

static int user_enc_calls, user_dec_calls;
static int user_open_enc(void *attr, void *enc) { (void)attr; (void)enc; return 0; }
static int user_close(void *x) { (void)x; return 0; }
static int user_enc_frm(void *enc, IMPAudioFrame *f, unsigned char *out, int *outLen)
{
    const uint8_t *in = (const uint8_t *)(uintptr_t)f->virAddr;
    int i;

    (void)enc;
    user_enc_calls++;
    for (i = 0; i < f->len / 2; i++)        /* high bytes only */
        out[i] = in[2 * i + 1];
    *outLen = f->len / 2;
    return 0;
}
static int user_open_dec(void *attr, void *dec) { (void)attr; (void)dec; return 0; }
static int user_dec_frm(void *dec, unsigned char *in, int inLen, unsigned short *out, int *outLen, int *chns)
{
    int i;

    (void)dec;
    user_dec_calls++;
    for (i = 0; i < inLen; i++)
        out[i] = (unsigned short)(in[i] << 8);
    *outLen = inLen * 2;
    if (chns)
        *chns = 1;
    return 0;
}
static int user_frm_info(void *dec, void *info) { (void)dec; (void)info; return 0; }

static int acodec_run(const char *tag, int ch, int encType, int decType, int n)
{
    static uint8_t streams[16][ACODEC_MAXSTREAM];
    static int slen[16];
    static int16_t pcm[ACODEC_SAMPLES];
    IMPAudioEncChnAttr ea;
    IMPAudioDecChnAttr da;
    uint32_t henc = 2166136261u, hdec = 2166136261u;
    int i, got = 0, dgot = 0, ts_same = 0;

    if (n > 16)
        n = 16;
    memset(&ea, 0, sizeof ea);
    ea.type = encType;
    ea.bufSize = 20;
    printf("T32C %s enc create rc=%d\n", tag, IMP_AENC_CreateChn(ch, &ea));
    for (i = 0; i < n; i++) {
        IMPAudioFrame f;
        IMPAudioStream s;
        int rc;

        acodec_pcm(pcm, ACODEC_SAMPLES, i);
        memset(&f, 0, sizeof f);
        f.bitwidth = AUDIO_BIT_WIDTH_16;
        f.soundmode = AUDIO_SOUND_MODE_MONO;
        f.virAddr = (uint32_t *)pcm;
        f.len = sizeof pcm;
        f.seq = i;
        f.timeStamp = 1000 + 20000LL * i;
        memset(&s, 0xa5, sizeof s);      /* shows which fields GetStream writes */
        rc = IMP_AENC_SendFrame(ch, &f);
        if (rc || IMP_AENC_PollingStream(ch, 1000) || IMP_AENC_GetStream(ch, &s, BLOCK)) {
            printf("T32C %s enc %d send rc=%d none\n", tag, i, rc);
            slen[i] = 0;
            continue;
        }
        slen[i] = s.len > ACODEC_MAXSTREAM ? ACODEC_MAXSTREAM : s.len;
        memcpy(streams[i], s.stream, slen[i]);
        ts_same += s.timeStamp == f.timeStamp;
        if (i < 2 || i == n - 1)
            printf("T32C %s enc %d len %d seq %#x phy %#x ts %s h %08x\n", tag, i, s.len,
                   (unsigned)s.seq, (unsigned)s.phyAddr,
                   s.timeStamp == f.timeStamp ? "frame" : "other", fnv(s.stream, s.len));
        henc = fnv(s.stream, s.len) ^ (henc * 31u);
        got++;
        IMP_AENC_ReleaseStream(ch, &s);
    }
    printf("T32C %s enc frames %d/%d all %08x ts=frame %d\n", tag, got, n, henc, ts_same);
    printf("T32C %s enc destroy rc=%d\n", tag, IMP_AENC_DestroyChn(ch));

    memset(&da, 0, sizeof da);
    da.type = decType;
    da.bufSize = 20;
    da.mode = ADEC_MODE_PACK;
    printf("T32C %s dec create rc=%d\n", tag, IMP_ADEC_CreateChn(ch, &da));
    for (i = 0; i < n; i++) {
        IMPAudioStream in, out;
        int rc;

        if (!slen[i])
            continue;
        memset(&in, 0, sizeof in);
        in.stream = streams[i];
        in.len = slen[i];
        in.seq = i;
        in.timeStamp = 5000 + i;
        memset(&out, 0xa5, sizeof out);
        rc = IMP_ADEC_SendStream(ch, &in, BLOCK);
        if (rc || IMP_ADEC_PollingStream(ch, 1000) || IMP_ADEC_GetStream(ch, &out, BLOCK)) {
            printf("T32C %s dec %d send rc=%d none\n", tag, i, rc);
            continue;
        }
        if (i < 2 || i == n - 1)
            printf("T32C %s dec %d len %d seq %#x phy %#x ts %#llx h %08x\n", tag, i, out.len,
                   (unsigned)out.seq, (unsigned)out.phyAddr, (unsigned long long)out.timeStamp,
                   fnv(out.stream, out.len));
        hdec = fnv(out.stream, out.len) ^ (hdec * 31u);
        dgot++;
        IMP_ADEC_ReleaseStream(ch, &out);
    }
    printf("T32C %s dec frames %d/%d all %08x\n", tag, dgot, got, hdec);
    printf("T32C %s dec clear rc=%d\n", tag, IMP_ADEC_ClearChnBuf(ch));
    printf("T32C %s dec destroy rc=%d\n", tag, IMP_ADEC_DestroyChn(ch));
    return got == n && dgot == got;
}

static int acodec_test(int n)
{
    static const struct { const char *tag; int type; } t[] = {
        { "g711a", PT_G711A }, { "g711u", PT_G711U }, { "g726", PT_G726 }, { "adpcm", PT_ADPCM },
    };
    IMPAudioEncEncoder ue;
    IMPAudioDecDecoder ud;
    IMPAudioEncChnAttr ea;
    int i, ok = 1, eh = -1, dh = -1;

    for (i = 0; i < 4; i++)
        ok &= acodec_run(t[i].tag, i, t[i].type, t[i].type, n);

    memset(&ue, 0, sizeof ue);
    ue.type = PT_PCM;
    ue.maxFrmLen = ACODEC_MAXSTREAM;
    strcpy(ue.name, "t32hi");
    ue.openEncoder = user_open_enc;
    ue.encoderFrm = user_enc_frm;
    ue.closeEncoder = user_close;
    memset(&ud, 0, sizeof ud);
    ud.type = PT_PCM;
    strcpy(ud.name, "t32hi");
    ud.openDecoder = user_open_dec;
    ud.decodeFrm = user_dec_frm;
    ud.getFrmInfo = user_frm_info;
    ud.closeDecoder = user_close;
    printf("T32C user enc register rc=%d\n", IMP_AENC_RegisterEncoder(&eh, &ue));
    printf("T32C user dec register rc=%d\n", IMP_ADEC_RegisterDecoder(&dh, &ud));
    printf("T32C user handles enc %d dec %d\n", eh, dh);
    ok &= acodec_run("user", 4, eh, dh, n);
    printf("T32C user calls enc %d dec %d\n", user_enc_calls, user_dec_calls);
    printf("T32C user enc unregister rc=%d\n", IMP_AENC_UnRegisterEncoder(&eh));
    printf("T32C user dec unregister rc=%d\n", IMP_ADEC_UnRegisterDecoder(&dh));

    /* error returns */
    memset(&ea, 0, sizeof ea);
    ea.type = PT_G711A;
    ea.bufSize = 20;
    printf("T32C err create twice rc=%d", IMP_AENC_CreateChn(0, &ea));
    printf(" %d\n", IMP_AENC_CreateChn(0, &ea));
    printf("T32C err destroy twice rc=%d", IMP_AENC_DestroyChn(0));
    printf(" %d\n", IMP_AENC_DestroyChn(0));
    printf("T32C err dec destroy unused rc=%d\n", IMP_ADEC_DestroyChn(0));
    printf("T32S end ok=%d\n", ok);
    return ok ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "buf";
    int n = argc > 2 ? atoi(argv[2]) : 10;

    mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "oi_trace_mark");
    if (!strcmp(mode, "ivsdirect"))
        return ivs_direct(n);
    if (!strcmp(mode, "ivs"))
        return ivs_bound(n);
    if (!strcmp(mode, "buf"))
        return buf_test();
    if (!strcmp(mode, "su"))
        return su_test();
    if (!strcmp(mode, "audio"))
        return audio_test(n);
    if (!strcmp(mode, "dmic"))
        return dmic_test(n);
    if (!strcmp(mode, "audiox"))
        return audiox_test(n);
    if (!strcmp(mode, "dmicx"))
        return dmicx_test(n);
    if (!strcmp(mode, "acodec"))
        return acodec_test(n);
    printf("T32S unknown mode %s\n", mode);
    return 1;
}
