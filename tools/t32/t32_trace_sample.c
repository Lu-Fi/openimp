/*
 * Minimal T32 stream bring-up for the ioctl trace oracle (#287).
 *
 * Built against the OpenIMP public headers with PLATFORM_T32 and linked
 * against whichever libimp is installed (vendor 2.2.0 for the reference,
 * OpenIMP later), it runs: ISP open, AddSensor, EnableSensor, System_Init,
 * EnableTuning, FrameSource channel 0 (1920x1080 NV12), one encoder
 * channel bound to it (h264 default, jpeg or h265 by argument), N streams,
 * then the full teardown.  Each API step is announced to the LD_PRELOAD
 * recorder (oi_trace_mark) so the ioctl log is phase-tagged.
 *
 * "isp" runs only the ISP / sensor lifecycle (#288): ISP open, AddSensor,
 * EnableSensor, System_Init, the /proc/jz/isp/isp-m0 status lines
 * (prefix T32P), then System_Exit, DisableSensor, DelSensor, ISP close;
 * "isp N" repeats the whole cycle N times in one process.
 * "bin" is "isp" plus a night and a day IMP_ISP_Tuning_SetISPRunningMode
 * (IQ-bin switch, #289) between System_Init and the teardown; "bin N" ends
 * each of the N-1 extra cycles at night (what the driver reports after a
 * sensor release, #303).
 * "none" takes N frames with IMP_FrameSource_GetFrame (depth 1); the first
 * frame's colour bands are printed (T32F: mean Y/U/V of 8 vertical bands,
 * NV12 with UV after ALIGN16(height) rows) and its buffer address
 * (T32F snap, for a QEMU pmemsave).  "bind" (OpenIMP only, #290) binds
 * FrameSource channel 0 to a test receiver (OpenIMP_FrameSource_RegisterSink
 * on DEV_ID_ENC group 0) and counts N bound frames, then does the same
 * GetFrame run as "none".
 *
 * "fs3" runs channels 0-2 at once (1920x1080, 1280x720, 640x360, the
 * driver's three framechans) and takes N GetFrame frames from each.
 *
 * "vpu N" (#291) runs N encoder channel cycles (CreateGroup, JPEG
 * CreateChn, Register/UnRegister, DestroyChn, DestroyGroup: soc_vpu
 * CHANNEL_REQ / CHANNEL_REL) between the ISP bring-up and the teardown.
 *
 * T32_STREAM_DUMP=<file> appends every encoder stream (all packs, in order)
 * to the file and prints "T32J <n> <bytes> packs <k> seq <s>" (#292: the
 * trace script copies the file out, the host decodes and checks the JPEGs;
 * pictures stay local).
 *
 * T32_ENC_QP=<n> is SetDefaultParam's iInitialQP (default -1; the JPEG qp);
 * the channel attribute it gives is printed as "T32A <words>".
 * T32_ENC_RC=<n> is SetDefaultParam's rc mode (IMPEncoderRcMode number;
 * default FIXQP for jpeg, CBR otherwise; #293 compares H.264 at FIXQP).
 * T32_ENC_W / T32_ENC_H: FrameSource channel 0 and encoder size (1920x1080).
 * T32_ENC_QPS=a-b repeats the FrameSource + encoder channel cycle (N
 * streams each) for every initial QP a..b in one process ("T32Q <qp> done").
 *
 * "osd" (#297) is "h264" with an OSD group between FrameSource channel 0
 * and the encoder (Bind FS -> OSD 0 -> encoder 0): a BGRA text picture, a
 * cover, a rectangle and a 2-bit picture, plus the region-attribute and
 * error-code checks (lines "T32O ..."); the same run with vendor libimp
 * 2.2.0 is the reference for the /dev/ipu requests (T32_TRACE_DUMP=200).
 *
 * "isposd" (#336) is "jpeg" with the ISP-drawn OSD: IMP_ISP_Tuning_SetOsdPoolSize
 * before System_Init, then (FrameSource channel 0 running) region handles
 * until the library refuses one, an ARGB8888 and an ARGB1555 picture
 * (IMP_ISP_Tuning_Set/Get/ShowOsdRgn), a line, a range and a cover
 * (IMP_OSD_SetRgnAttr_ISP), the error paths, IMP_OSD_GetRgnAttr_ISP (lines
 * "T32I ..."), N streams, then hide and destroy the pictures.  Vendor libimp
 * 2.2.0 on the vendor and on the open tx-isp-t32.ko is the reference for the
 * isp-m0 requests and the MSCA register writes.
 *
 * usage: t32_trace_sample [h264|h265|jpeg|osd|isposd|none|bind|fs3|vpu|isp|bin] [streams] [sensor] [i2c addr]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
/* the T32 2.x OSD types (272-byte IMPOSDRgnAttr), OpenIMP's copy of the
 * vendor layout; the generic imp_osd.h is not the T32 ABI */
#include "t23/openimp_t23_osd_abi.h"

int IMP_OSD_SetPoolSize(int size);
int IMP_OSD_CreateGroup(int grpNum);
int IMP_OSD_DestroyGroup(int grpNum);
IMPRgnHandle IMP_OSD_CreateRgn(IMPOSDRgnAttr *prAttr);
void IMP_OSD_DestroyRgn(IMPRgnHandle handle);
int IMP_OSD_RegisterRgn(IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr);
int IMP_OSD_UnRegisterRgn(IMPRgnHandle handle, int grpNum);
int IMP_OSD_SetRgnAttr(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr);
int IMP_OSD_GetRgnAttr(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr);
int IMP_OSD_GetGrpRgnAttr(IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr);
int IMP_OSD_ShowRgn(IMPRgnHandle handle, int grpNum, int showFlag);
int IMP_OSD_Start(int grpNum);
int IMP_OSD_Stop(int grpNum);
int IMP_OSD_RgnCreate_Query(IMPRgnHandle handle, IMPOSDRgnCreateStat *pstStatus);

#if !defined(PLATFORM_T32)
#error "build with -DPLATFORM_T32"
#endif

/* T32 profile words come from imp_encoder.h (type byte JPEG 0, AVC 1,
 * HEVC 2 on PLATFORM_T32, asserted there; #302) */
#define T32_PROFILE_JPEG      IMP_ENC_PROFILE_JPEG
#define T32_PROFILE_AVC_MAIN  IMP_ENC_PROFILE_AVC_MAIN
#define T32_PROFILE_HEVC_MAIN IMP_ENC_PROFILE_HEVC_MAIN

static void (*mark)(const char *);

/* mean Y/U/V of 8 vertical bands over the middle rows of an NV12 frame */
static void frame_report(const char *tag, const IMPFrameInfo *f)
{
    const uint8_t *y = (const uint8_t *)(uintptr_t)f->virAddr;
    uint32_t stride = (f->width + 15) & ~15u, w = f->width, h = f->height;
    const uint8_t *uv = y + stride * ((h + 15) & ~15u);
    int b;

    printf("T32F %s %ux%u fmt %u size %u pa 0x%08x bands(Y,U,V):", tag, w, h,
           f->pixfmt, f->size, f->phyAddr);
    for (b = 0; b < 8; b++) {
        uint32_t x0 = w * b / 8 + w / 32, x1 = w * (b + 1) / 8 - w / 32, x, r;
        uint64_t sy = 0, su = 0, sv = 0, n = 0, nc = 0;

        for (r = h / 4; r < h * 3 / 4; r += 8)
            for (x = x0; x < x1; x += 4) {
                sy += y[r * stride + x];
                n++;
            }
        for (r = h / 8; r < h * 3 / 8; r += 4)
            for (x = x0 & ~1u; x < x1; x += 8) {
                su += uv[r * stride + x];
                sv += uv[r * stride + x + 1];
                nc++;
            }
        printf(" %u,%u,%u", (unsigned)(n ? sy / n : 0), (unsigned)(nc ? su / nc : 0),
               (unsigned)(nc ? sv / nc : 0));
    }
    printf("\n");
    fflush(stdout);
}

/* T32_STREAM_DUMP=<file>: every stream's bytes appended to the file */
static void stream_dump(int n, const IMPEncoderStream *st)
{
    static FILE *f;
    uint32_t total = 0, p;

    if (!f)
        f = fopen(getenv("T32_STREAM_DUMP"), "wb");
    for (p = 0; p < st->packCount; p++) {
        total += st->pack[p].length;
        if (f)
            fwrite((const void *)(uintptr_t)st->pack[p].virAddr, 1, st->pack[p].length, f);
    }
    if (f)
        fflush(f);
    printf("T32J %d %u packs %u seq %u\n", n, total, st->packCount, st->seq);
    fflush(stdout);
}

/* after the teardown: the dump file to physical 0x7800000 (rmem top, unused
 * now) for the host's pmemsave (/dev/mem read/write stop at kernel RAM's
 * end; mmap does not) */
static void stream_copy_out(void)
{
    const char *path = getenv("T32_STREAM_DUMP");
    FILE *f = path ? fopen(path, "rb") : NULL;
    long size;
    int fd;
    void *m;

    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    m = fd < 0 || size <= 0 ? MAP_FAILED :
        mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x7800000);
    if (m != MAP_FAILED) {
        size_t n = fread(m, 1, (size_t)size, f);
        msync(m, (size_t)size, MS_SYNC);
        munmap(m, (size_t)size);
        printf("T32J out %ld copied %zu\n", size, n);
    } else {
        printf("T32J out failed\n");
    }
    if (fd >= 0)
        close(fd);
    fclose(f);
}

/* "bind": the test receiver behind DEV_ID_ENC group 0 */
typedef int (*sink_fn)(void *ctx, int fs_chn, IMPFrameInfo *frame);
static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sink_cond = PTHREAD_COND_INITIALIZER;
static int sink_frames;

static int test_sink(void *ctx, int fs_chn, IMPFrameInfo *frame)
{
    (void)ctx;
    pthread_mutex_lock(&sink_lock);
    if (!sink_frames++) {
        printf("T32F bound chn %d index %d\n", fs_chn, frame->index);
        frame_report("bound", frame);
    }
    pthread_cond_broadcast(&sink_cond);
    pthread_mutex_unlock(&sink_lock);
    return 0;
}

static void proc_status(void)
{
    char line[160];
    FILE *f;

    if (getenv("T32_NO_PROC"))          /* vendor tx-isp-t32.ko oopses reading it */
        return;
    f = fopen("/proc/jz/isp/isp-m0", "r");

    if (!f) {
        printf("T32P no /proc/jz/isp/isp-m0\n");
        return;
    }
    while (fgets(line, sizeof line, f))
        printf("T32P %s", line);
    fclose(f);
    fflush(stdout);
}

/* ---- "osd" (#297) ------------------------------------------------------- */
#define OSD_TEXT_W 320                  /* multiple of 16: same IPU src_w as the vendor */
#define OSD_TEXT_H 36
#define OSD_2BIT_W 128
#define OSD_2BIT_H 32
#define OSD_NRGN 4
static IMPRgnHandle osd_rgn[OSD_NRGN];
static uint32_t osd_text[OSD_TEXT_W * OSD_TEXT_H];
static uint8_t osd_2bit[OSD_2BIT_W * OSD_2BIT_H / 4];

/* 5x7 glyphs, rows top down, bit 4 = left column */
static const struct { char c; uint8_t row[7]; } osd_font[] = {
    { '0', { 14, 17, 19, 21, 25, 17, 14 } }, { '1', { 4, 12, 4, 4, 4, 4, 14 } },
    { '2', { 14, 17, 1, 2, 4, 8, 31 } },     { '3', { 31, 2, 4, 2, 1, 17, 14 } },
    { '4', { 2, 6, 10, 18, 31, 2, 2 } },     { '5', { 31, 16, 30, 1, 1, 17, 14 } },
    { '6', { 6, 8, 16, 30, 17, 17, 14 } },   { '7', { 31, 1, 2, 4, 8, 8, 8 } },
    { '8', { 14, 17, 17, 14, 17, 17, 14 } }, { '9', { 14, 17, 17, 15, 1, 2, 12 } },
    { ':', { 0, 12, 12, 0, 12, 12, 0 } },    { '-', { 0, 0, 0, 31, 0, 0, 0 } },
    { 'D', { 28, 18, 17, 17, 17, 18, 28 } }, { 'O', { 14, 17, 17, 17, 17, 17, 14 } },
    { 'S', { 15, 16, 16, 14, 1, 1, 30 } },   { 'T', { 31, 4, 4, 4, 4, 4, 4 } },
};

/* white text (scale 4) on a half-transparent black box, BGRA bytes */
static void osd_render_text(const char *text)
{
    int i, x0 = 8, k;

    for (i = 0; i < OSD_TEXT_W * OSD_TEXT_H; i++)
        osd_text[i] = 0x80000000u;              /* A=0x80, black */
    for (; *text && x0 + 20 <= OSD_TEXT_W; text++, x0 += 24)
        for (k = 0; k < (int)(sizeof osd_font / sizeof osd_font[0]); k++) {
            int r, c;
            if (osd_font[k].c != *text)
                continue;
            for (r = 0; r < 7 * 4; r++)
                for (c = 0; c < 5 * 4; c++)
                    if (osd_font[k].row[r / 4] & (16 >> (c / 4)))
                        osd_text[(r + 4) * OSD_TEXT_W + x0 + c] = 0xffffffffu;
        }
}

static void osd_attr_print(const char *tag, const IMPOSDRgnAttr *a, const IMPOSDRgnAttr *b)
{
    const uint32_t *x = (const uint32_t *)(const void *)a, *y = (const uint32_t *)(const void *)b;
    unsigned k;

    printf("T32O %s size %u diff:", tag, (unsigned)sizeof *a);
    for (k = 0; k < sizeof *a / 4; k++)
        if (x[k] != y[k])
            printf(" w%u", k);
    printf("\n");
}

static int step(const char *name, int rc);
#define STEP(call) (mark ? mark(#call) : (void)0, step(#call, (call)))

/* regions + group 0; returns 0 when every step worked */
static int osd_setup(void)
{
    IMPOSDRgnAttr a[OSD_NRGN], back;
    IMPOSDGrpRgnAttr g;
    IMPOSDRgnCreateStat cs;
    int i, ok = 1;

    memset(a, 0, sizeof a);
    osd_render_text("T32 OSD 11-11");
    a[0].type = OSD_REG_PIC;
    a[0].fmt = T23_OSD_PIX_BGRA;
    a[0].rect.p0.x = 64;
    a[0].rect.p0.y = 64;
    a[0].rect.p1.x = 64 + OSD_TEXT_W - 1;
    a[0].rect.p1.y = 64 + OSD_TEXT_H - 1;
    a[0].data.picData.pData = osd_text;
    a[1].type = OSD_REG_COVER;
    a[1].fmt = T23_OSD_PIX_BGRA;
    a[1].rect.p0.x = 640;
    a[1].rect.p0.y = 240;
    a[1].rect.p1.x = 640 + 255;
    a[1].rect.p1.y = 240 + 159;
    a[1].data.coverData.color = 0xff00ff00u;
    a[2].type = OSD_REG_RECT;
    a[2].fmt = T23_OSD_PIX_MONOWHITE;     /* vendor 2.2.0 draws lines only with fmt 8 */
    a[2].rect.p0.x = 1100;
    a[2].rect.p0.y = 500;
    a[2].rect.p1.x = 1499;
    a[2].rect.p1.y = 799;
    a[2].data.lineRectData.color = 0xffff0000u;
    a[2].data.lineRectData.linewidth = 6;
    a[3].type = OSD_REG_PIC;
    a[3].fmt = T23_OSD_PIX_2BIT;
    a[3].rect.p0.x = 64;
    a[3].rect.p0.y = 800;
    a[3].rect.p1.x = 64 + OSD_2BIT_W - 1;
    a[3].rect.p1.y = 800 + OSD_2BIT_H - 1;
    for (i = 0; i < (int)sizeof osd_2bit; i++)
        osd_2bit[i] = (uint8_t)((i / (OSD_2BIT_W / 16)) & 1 ? 0x55 : 0xaa);
    a[3].data.picData.pData = osd_2bit;
    a[3].bitAttr.twobit_mask = 0x3;
    a[3].bitAttr.twobit_bit0_fmt = 0xffffff00u;
    a[3].bitAttr.twobit_bit1_fmt = 0xff0000ffu;

    ok &= STEP(IMP_OSD_CreateGroup(0)) == 0;
    for (i = 0; i < OSD_NRGN; i++) {
        osd_rgn[i] = IMP_OSD_CreateRgn(NULL);
        printf("T32O create %d -> %d\n", i, osd_rgn[i]);
        ok &= osd_rgn[i] >= 0;
        ok &= STEP(IMP_OSD_SetRgnAttr(osd_rgn[i], &a[i])) == 0;
        memset(&back, 0x5a, sizeof back);
        printf("T32O getattr %d rc=0x%x\n", i, (unsigned)IMP_OSD_GetRgnAttr(osd_rgn[i], &back));
        osd_attr_print(i == 0 ? "pic" : i == 1 ? "cover" : i == 2 ? "rect" : "2bit", &a[i], &back);
        memset(&g, 0, sizeof g);
        g.show = 1;
        g.gAlphaEn = 0;
        g.fgAlhpa = 0xff;
        g.layer = i;
        g.scalex = g.scaley = 1.0f;
        ok &= STEP(IMP_OSD_RegisterRgn(osd_rgn[i], 0, &g)) == 0;
    }
    /* error codes (vendor 2.2.0 reference) */
    printf("T32O rc getattr(200)=0x%x getattr(100)=0x%x creategroup(12)=0x%x register(.,12)=0x%x"
           " show(100,0)=0x%x start(12)=0x%x\n",
           (unsigned)IMP_OSD_GetRgnAttr(200, &back), (unsigned)IMP_OSD_GetRgnAttr(100, &back),
           (unsigned)IMP_OSD_CreateGroup(12), (unsigned)IMP_OSD_RegisterRgn(osd_rgn[0], 12, &g),
           (unsigned)IMP_OSD_ShowRgn(100, 0, 1), (unsigned)IMP_OSD_Start(12));
    memset(&cs, 0, sizeof cs);
    printf("T32O query(%d)=0x%x status %d\n", osd_rgn[0],
           (unsigned)IMP_OSD_RgnCreate_Query(osd_rgn[0], &cs), cs.status);
    memset(&g, 0, sizeof g);
    printf("T32O getgrp rc=0x%x show %d layer %d fg %d\n",
           (unsigned)IMP_OSD_GetGrpRgnAttr(osd_rgn[2], 0, &g), g.show, g.layer, g.fgAlhpa);
    ok &= STEP(IMP_OSD_Start(0)) == 0;
    return ok ? 0 : -1;
}

static void osd_teardown(void)
{
    int i;

    STEP(IMP_OSD_Stop(0));
    for (i = 0; i < OSD_NRGN; i++) {
        STEP(IMP_OSD_ShowRgn(osd_rgn[i], 0, 0));
        STEP(IMP_OSD_UnRegisterRgn(osd_rgn[i], 0));
        IMP_OSD_DestroyRgn(osd_rgn[i]);
    }
    STEP(IMP_OSD_DestroyGroup(0));
}

/* ---- "isposd" (#336): pictures, lines/rects and covers drawn by the ISP --- */
/* vendor 2.2.0 layouts, filled by byte offset (imp_isp.h / imp_osd.h 2.x):
 * IMPIspOsdAttrAsm 56 B = type, IMPISPOSDAttr (32) at 4, IMPISPOSDBlockAttr
 * (20) at 36; IMPISPDrawBlockAttr 36 B (pinum, msk_size, type @4,
 * color_type @8, cfg @12); IMPISPMaskBlockAttr 24 B; region attribute 272 B
 * with the draw block at 48 and the mask block at 104. */
int IMP_ISP_Tuning_SetOsdPoolSize(int size);
int IMP_ISP_Tuning_CreateOsdRgn(int chn, void *attr);
int IMP_ISP_Tuning_SetOsdRgnAttr(int chn, int handle, void *attr);
int IMP_ISP_Tuning_GetOsdRgnAttr(int chn, int handle, void *attr);
int IMP_ISP_Tuning_ShowOsdRgn(int chn, int handle, int show);
int IMP_ISP_Tuning_DestroyOsdRgn(int chn, int handle);
int IMP_OSD_SetRgnAttr_ISP(int sensornum, IMPOSDRgnAttr *attr, int show);
int IMP_OSD_GetRgnAttr_ISP(int sensornum, IMPOSDRgnAttr *attr, int *show);

#define ISPOSD_MAXRGN 10
#define ISPOSD_W0 64                    /* picture 0: ARGB8888 BGRA 64x32 */
#define ISPOSD_H0 32
#define ISPOSD_W1 32                    /* picture 1: ARGB1555 32x32, group 1 */
#define ISPOSD_H1 32
static int isposd_h[ISPOSD_MAXRGN], isposd_n;
static uint32_t isposd_pic0[ISPOSD_W0 * ISPOSD_H0];
static uint16_t isposd_pic1[ISPOSD_W1 * ISPOSD_H1];

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static void isposd_pic_attr(uint8_t *a, int type, int group, int pinum, int x, int y,
                            int w, int h, const void *data, int stride)
{
    memset(a, 0, 56);
    put32(a, 1);                        /* ISP_OSD_REG_PIC */
    put32(a + 4, (uint32_t)type);       /* osd_type: 0 ARGB8888, 1 ARGB1555 */
    put32(a + 8, 0);                    /* IMP_ISP_ARGB_TYPE_BGRA */
    put32(a + 12, 0);                   /* osd_pixel_alpha_disable: off */
    a[33] = (uint8_t)group;             /* osd_group (osd_overlap at 32: 0) */
    a[36] = (uint8_t)pinum;
    a[37] = 1;
    put16(a + 38, (unsigned)x);
    put16(a + 40, (unsigned)y);
    put16(a + 42, (unsigned)w);
    put16(a + 44, (unsigned)h);
    put32(a + 48, (uint32_t)(uintptr_t)data);
    put16(a + 52, (unsigned)stride);
}

static void isposd_dump(const char *tag, const uint8_t *p, int n)
{
    int k;

    printf("T32I %s", tag);
    for (k = 0; k < n; k++)
        printf("%s%02x", k % 4 ? "" : " ", p[k]);
    printf("\n");
}

static int isposd_setup(void)
{
    static uint8_t a[56], back[56];
    static IMPOSDRgnAttr r;
    uint8_t *d = (uint8_t *)&r + 48, *m = (uint8_t *)&r + 104;
    int i, show = -1, ok = 1;

    for (i = 0; i < ISPOSD_W0 * ISPOSD_H0; i++)  /* BGRA bytes: white/blue bars */
        isposd_pic0[i] = (i % ISPOSD_W0) / 8 & 1 ? 0xff0000ffu : 0xffffffffu;
    for (i = 0; i < ISPOSD_W1 * ISPOSD_H1; i++)  /* 1555: opaque checker */
        isposd_pic1[i] = ((i % ISPOSD_W1) / 8 + (i / ISPOSD_W1) / 8) & 1 ? 0x801f : 0x83e0;
    /* region handles until the library says no (its limit per sensor) */
    for (isposd_n = 0; isposd_n < ISPOSD_MAXRGN; isposd_n++) {
        isposd_h[isposd_n] = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
        printf("T32I create %d -> %d\n", isposd_n, isposd_h[isposd_n]);
        if (isposd_h[isposd_n] < 0)
            break;
    }
    printf("T32I handles %d\n", isposd_n);
    for (i = isposd_n - 1; i >= 2; i--)  /* keep two */
        STEP(IMP_ISP_Tuning_DestroyOsdRgn(0, isposd_h[i]));
    isposd_n = isposd_n < 2 ? isposd_n : 2;
    if (isposd_n < 2)
        return -1;
    printf("T32I create attr %d\n", IMP_ISP_Tuning_CreateOsdRgn(0, a));
    printf("T32I set bad chn %d\n", IMP_ISP_Tuning_SetOsdRgnAttr(5, isposd_h[0], a));
    printf("T32I show free %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, 7, 1));

    isposd_pic_attr(a, 0, 0, isposd_h[0], 64, 48, ISPOSD_W0, ISPOSD_H0, isposd_pic0, ISPOSD_W0 * 4);
    ok &= STEP(IMP_ISP_Tuning_SetOsdRgnAttr(0, isposd_h[0], a)) == 0;
    ok &= STEP(IMP_ISP_Tuning_ShowOsdRgn(0, isposd_h[0], 1)) == 0;
    /* pinum != handle first (the library skips such a picture), then pinum = handle */
    isposd_pic_attr(a, 1, 1, isposd_h[1] + 4, 400, 200, ISPOSD_W1, ISPOSD_H1, isposd_pic1, ISPOSD_W1 * 2);
    ok &= STEP(IMP_ISP_Tuning_SetOsdRgnAttr(0, isposd_h[1], a)) == 0;
    ok &= STEP(IMP_ISP_Tuning_ShowOsdRgn(0, isposd_h[1], 1)) == 0;
    isposd_pic_attr(a, 1, 1, isposd_h[1], 400, 200, ISPOSD_W1, ISPOSD_H1, isposd_pic1, ISPOSD_W1 * 2);
    ok &= STEP(IMP_ISP_Tuning_SetOsdRgnAttr(0, isposd_h[1], a)) == 0;
    ok &= STEP(IMP_ISP_Tuning_ShowOsdRgn(0, isposd_h[1], 1)) == 0;
    memset(back, 0xee, sizeof back);
    printf("T32I getattr rc=%d\n", IMP_ISP_Tuning_GetOsdRgnAttr(0, isposd_h[0], back));
    isposd_dump("attr0", back, 56);
    isposd_pic_attr(a, 0, 0, isposd_h[0], 1900, 48, ISPOSD_W0, ISPOSD_H0, isposd_pic0, ISPOSD_W0 * 4);
    printf("T32I set outside %d\n", IMP_ISP_Tuning_SetOsdRgnAttr(0, isposd_h[0], a));

    /* ISP draw blocks: a line (pinum 0) and a range (pinum 1), then a cover */
    memset(&r, 0, sizeof r);
    r.type = OSD_REG_ISP_LINE_RECT;
    d[0] = 0;                           /* pinum */
    put32(d + 4, 2);                    /* IMP_ISP_DRAW_LINE */
    put32(d + 8, 1);                    /* IMPISP_MASK_TYPE_YUV */
    d[12] = 1;
    put16(d + 14, 200); put16(d + 16, 600); put16(d + 18, 800); put16(d + 20, 600);
    d[25] = 255; d[26] = 0; d[27] = 0;  /* y, u, v */
    d[28] = 5; d[29] = 4;               /* width, alpha */
    ok &= STEP(IMP_OSD_SetRgnAttr_ISP(0, &r, 1)) == 0;
    memset(d, 0, 36);
    d[0] = 1;
    put32(d + 4, 1);                    /* IMP_ISP_DRAW_RANGE */
    put32(d + 8, 0);                    /* RGB colour, the library adds the YUV */
    d[12] = 1;
    put16(d + 14, 1200); put16(d + 16, 600); put16(d + 18, 300); put16(d + 20, 200);
    d[22] = 0; d[23] = 255; d[24] = 0;  /* green */
    d[28] = 5; d[29] = 4; put16(d + 30, 50);
    ok &= STEP(IMP_OSD_SetRgnAttr_ISP(0, &r, 1)) == 0;
    memset(&r, 0, sizeof r);
    r.type = OSD_REG_ISP_COVER;
    m[0] = 0; m[1] = 0; m[2] = 1;       /* chx, pinum, enable */
    put16(m + 4, 300); put16(m + 6, 900); put16(m + 8, 200); put16(m + 10, 120);
    put32(m + 12, 1);                   /* YUV */
    m[19] = 40; m[20] = 200; m[21] = 60;
    ok &= STEP(IMP_OSD_SetRgnAttr_ISP(0, &r, 1)) == 0;
    r.type = OSD_REG_ISP_PIC;
    printf("T32I set isp-pic %d\n", IMP_OSD_SetRgnAttr_ISP(0, &r, 1));
    memset(&r, 0xee, sizeof r);
    printf("T32I getattr isp rc=%d show=%d\n", IMP_OSD_GetRgnAttr_ISP(0, &r, &show), show);
    isposd_dump("draw", (uint8_t *)&r + 48, 80);
    return ok ? 0 : -1;
}

static void isposd_teardown(void)
{
    int i;

    for (i = 0; i < isposd_n; i++) {
        STEP(IMP_ISP_Tuning_ShowOsdRgn(0, isposd_h[i], 0));
        STEP(IMP_ISP_Tuning_DestroyOsdRgn(0, isposd_h[i]));
    }
}

static int step(const char *name, int rc)
{
    printf("T32S %-40s rc=%d\n", name, rc);
    fflush(stdout);
    return rc;
}

#define MUST(call) do { if (STEP(call)) goto fail; } while (0)

int main(int argc, char **argv)
{
    const char *codec = argc > 1 ? argv[1] : "h264";
    int streams = argc > 2 ? atoi(argv[2]) : 5;
    const char *sensor = argc > 3 ? argv[3] : "cv8001";
    int addr = argc > 4 ? (int)strtol(argv[4], NULL, 0) : 0x35;
    int bin_test = !strcmp(codec, "bin");
    int bind_test = !strcmp(codec, "bind");
    int fs3 = !strcmp(codec, "fs3");
    int vpu_cycles = !strcmp(codec, "vpu");
    int osd = !strcmp(codec, "osd");
    int isposd = !strcmp(codec, "isposd");
    int isp_only = !strcmp(codec, "isp") || bin_test;
    int enc = strcmp(codec, "none") != 0 && !bind_test && !fs3 && !isp_only && !vpu_cycles;
    IMPSensorInfo si;
    IMPFSChnAttr fs;
    static IMPEncoderCHNAttr ea;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_ENC, 0, 0 }, ocell = { DEV_ID_OSD, 0, 0 };
    IMPEncoderProfile prof = !strcmp(codec, "jpeg") || isposd ? T32_PROFILE_JPEG :
                             !strcmp(codec, "h265") ? T32_PROFILE_HEVC_MAIN : T32_PROFILE_AVC_MAIN;
    IMPEncoderRcMode rc = getenv("T32_ENC_RC") ? (IMPEncoderRcMode)atoi(getenv("T32_ENC_RC")) :
                          prof == T32_PROFILE_JPEG ? IMP_ENC_RC_MODE_FIXQP : IMP_ENC_RC_MODE_CBR;
    int enc_w = getenv("T32_ENC_W") ? atoi(getenv("T32_ENC_W")) : 1920;
    int enc_h = getenv("T32_ENC_H") ? atoi(getenv("T32_ENC_H")) : 1080;
    int qp_cur = getenv("T32_ENC_QP") ? atoi(getenv("T32_ENC_QP")) : -1, qp_last = qp_cur;
    int i, got = 0;

    if (getenv("T32_ENC_QPS"))          /* "a-b" */
        sscanf(getenv("T32_ENC_QPS"), "%d-%d", &qp_cur, &qp_last);
    mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "oi_trace_mark");

    memset(&si, 0, sizeof si);
    snprintf(si.name, sizeof si.name, "%s", sensor);
    si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof si.i2c.type, "%s", sensor);
    si.i2c.addr = addr;
    si.i2c.i2c_adapter = 0;
    si.rst_gpio = si.pwdn_gpio = si.power_gpio = -1;
    si.switch_gpio = -1;

    memset(&fs, 0, sizeof fs);
    fs.picWidth = enc_w;
    fs.picHeight = enc_h;
    fs.pixFmt = PIX_FMT_NV12;
    fs.scaler.enable = 1;
    fs.scaler.outwidth = enc_w;
    fs.scaler.outheight = enc_h;
    fs.outFrmRateNum = 15;
    fs.outFrmRateDen = 1;
    fs.nrVBs = 2;
    fs.type = FS_PHY_CHANNEL;

    if (isp_only && streams > 1) {      /* "isp N": N-1 extra full cycles first */
        for (i = 1; i < streams; i++) {
            MUST(IMP_ISP_Open());
            MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
            MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
            MUST(IMP_System_Init());
            proc_status();
            if (bin_test) {             /* "bin N": leave each extra cycle at night (#303) */
                IMPISPRunningMode rm = IMPISP_RUNNING_MODE_NIGHT;
                STEP(IMP_ISP_EnableTuning());
                STEP(IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &rm));
                STEP(IMP_ISP_DisableTuning());
            }
            MUST(IMP_System_Exit());
            MUST(IMP_ISP_DisableSensor(IMPVI_MAIN));
            MUST(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
            MUST(IMP_ISP_Close());
        }
    }
    MUST(IMP_ISP_Open());
    MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
    MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
    if (osd)
        STEP(IMP_OSD_SetPoolSize(512 * 1024));
    if (isposd)
        STEP(IMP_ISP_Tuning_SetOsdPoolSize(512 * 1024));
    MUST(IMP_System_Init());
    if (bin_test) {                     /* #289: day/night bin switch */
        IMPISPRunningMode rm = IMPISP_RUNNING_MODE_NIGHT;
        STEP(IMP_ISP_EnableTuning());   /* vendor needs it; OpenIMP T32: stub */
        if (getenv("T32_BIN_GET_FIRST")) {      /* #303: the mode before any switch */
            IMPISPRunningMode before = (IMPISPRunningMode)-1;
            STEP(IMP_ISP_Tuning_GetISPRunningMode(IMPVI_MAIN, &before));
            printf("T32S running mode before %d\n", (int)before);
        }
        STEP(IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &rm));
        STEP(IMP_ISP_Tuning_GetISPRunningMode(IMPVI_MAIN, &rm));
        printf("T32S running mode %d\n", (int)rm);
        proc_status();
        rm = IMPISP_RUNNING_MODE_DAY;
        STEP(IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &rm));
        STEP(IMP_ISP_DisableTuning());
    }
    if (isp_only) {
        proc_status();
        STEP(IMP_System_Exit());
        STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
        STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
        STEP(IMP_ISP_Close());
        proc_status();
        printf("T32S end ok=1\n");
        return 0;
    }
    STEP(IMP_ISP_EnableTuning());       /* OpenIMP T32: stub until the tuning issue */
    if (vpu_cycles) {                   /* #291: soc_vpu request/release cycles */
        int ok = 1;
        static IMPEncoderCHNAttr va;
        for (i = 0; i < streams && ok; i++) {
            ok &= STEP(IMP_Encoder_CreateGroup(0)) == 0;
            ok &= STEP(IMP_Encoder_SetDefaultParam(&va, T32_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP,
                                                   1920, 1080, 15, 1, 15, 0, -1, 0)) == 0;
            ok &= STEP(IMP_Encoder_CreateChn(0, &va)) == 0;
            ok &= STEP(IMP_Encoder_RegisterChn(0, 0)) == 0;
            ok &= STEP(IMP_Encoder_UnRegisterChn(0)) == 0;
            ok &= STEP(IMP_Encoder_DestroyChn(0)) == 0;
            ok &= STEP(IMP_Encoder_DestroyGroup(0)) == 0;
        }
        printf("T32S vpu cycles %d/%d\n", ok ? streams : i - 1, streams);
        got = ok ? streams : 0;
        goto teardown;
    }
    if (fs3) {                          /* #290: the three framechans at once */
        static const int sz[3][2] = { { 1920, 1080 }, { 1280, 720 }, { 640, 360 } };
        int c, ok = 1;
        for (c = 0; c < 3; c++) {
            IMPFSChnAttr a = fs;
            a.picWidth = a.scaler.outwidth = sz[c][0];
            a.picHeight = a.scaler.outheight = sz[c][1];
            MUST(IMP_FrameSource_CreateChn(c, &a));
            MUST(IMP_FrameSource_EnableChn(c));
            MUST(IMP_FrameSource_SetFrameDepth(c, 1));
        }
        for (c = 0; c < 3; c++) {
            int n = 0;
            for (i = 0; i < streams; i++) {
                IMPFrameInfo *frame;
                if (IMP_FrameSource_GetFrame(c, (void **)&frame))
                    continue;
                if (!n) {
                    char tag[8];
                    snprintf(tag, sizeof tag, "chn%d", c);
                    frame_report(tag, frame);
                }
                n++;
                IMP_FrameSource_ReleaseFrame(c, frame);
            }
            printf("T32S chn%d frames %d/%d\n", c, n, streams);
            ok &= n == streams;
        }
        for (c = 2; c >= 0; c--) {
            STEP(IMP_FrameSource_DisableChn(c));
            STEP(IMP_FrameSource_DestroyChn(c));
        }
        got = ok ? streams : 0;
        goto teardown;
    }
again:
    MUST(IMP_FrameSource_CreateChn(0, &fs));
    MUST(IMP_FrameSource_SetChnAttr(0, &fs));
    if (enc) {
        MUST(IMP_Encoder_CreateGroup(0));
        MUST(IMP_Encoder_SetDefaultParam(&ea, prof, rc, enc_w, enc_h, 15, 1, 15, 0, qp_cur, 1000));
        {                               /* the default channel attribute, as words */
            const uint32_t *w = (const uint32_t *)(const void *)&ea;
            unsigned k;
            printf("T32A");
            for (k = 0; k < sizeof ea / 4; k++)
                printf(" %08x", w[k]);
            printf("\n");
        }
        MUST(IMP_Encoder_CreateChn(0, &ea));
        MUST(IMP_Encoder_RegisterChn(0, 0));
        if (osd) {
            MUST(osd_setup());
            MUST(IMP_System_Bind(&src, &ocell));
            MUST(IMP_System_Bind(&ocell, &dst));
        } else {
            MUST(IMP_System_Bind(&src, &dst));
        }
    }
    MUST(IMP_FrameSource_EnableChn(0));
    if (isposd) {
        if (mark)
            mark("isposd");
        MUST(isposd_setup());
    }
    if (enc) {
        MUST(IMP_Encoder_StartRecvPic(0));
        if (mark)
            mark("streams");
        for (i = 0; i < streams; i++) {
            IMPEncoderStream st;
            if (IMP_Encoder_PollingStream(0, 3000) || IMP_Encoder_GetStream(0, &st, 1))
                continue;
            if (getenv("T32_STREAM_DUMP"))
                stream_dump(got, &st);
            got++;
            IMP_Encoder_ReleaseStream(0, &st);
        }
        printf("T32S streams %d/%d\n", got, streams);
        if (isposd)
            isposd_teardown();
        STEP(IMP_Encoder_StopRecvPic(0));
        STEP(IMP_FrameSource_DisableChn(0));
        if (osd) {
            STEP(IMP_System_UnBind(&ocell, &dst));
            STEP(IMP_System_UnBind(&src, &ocell));
            osd_teardown();
        } else {
            STEP(IMP_System_UnBind(&src, &dst));
        }
        STEP(IMP_Encoder_UnRegisterChn(0));
        STEP(IMP_Encoder_DestroyChn(0));
        STEP(IMP_Encoder_DestroyGroup(0));
    } else {
        IMPFrameInfo *frame;
        if (bind_test) {
            int (*reg)(int, int, sink_fn, void *) =
                (int (*)(int, int, sink_fn, void *))dlsym(RTLD_DEFAULT, "OpenIMP_FrameSource_RegisterSink");
            int bound;
            if (!reg) {
                printf("T32S bind: no OpenIMP_FrameSource_RegisterSink (not OpenIMP)\n");
                goto fail;
            }
            MUST(reg(DEV_ID_ENC, 0, test_sink, NULL));
            MUST(IMP_System_Bind(&src, &dst));
            pthread_mutex_lock(&sink_lock);
            while (sink_frames < streams) {
                struct timespec t;
                clock_gettime(CLOCK_REALTIME, &t);
                t.tv_sec += 3;
                if (pthread_cond_timedwait(&sink_cond, &sink_lock, &t))
                    break;
            }
            bound = sink_frames;
            pthread_mutex_unlock(&sink_lock);
            STEP(IMP_System_UnBind(&src, &dst));
            reg(DEV_ID_ENC, 0, NULL, NULL);
            printf("T32S bound frames %d/%d\n", bound < streams ? bound : streams, streams);
            if (bound < streams)
                got = -1;
        }
        MUST(IMP_FrameSource_SetFrameDepth(0, 1));
        if (mark)
            mark("frames");
        for (i = 0; i < streams; i++) {
            if (IMP_FrameSource_GetFrame(0, (void **)&frame))
                continue;
            if (got == 0) {
                frame_report("frame", frame);
                printf("T32F snap pa=0x%08x size=%u w=%u h=%u\n", frame->phyAddr,
                       ((frame->width + 15) & ~15u) * ((frame->height + 15) & ~15u) * 3 / 2,
                       frame->width, frame->height);
            }
            if (got >= 0)
                got++;
            IMP_FrameSource_ReleaseFrame(0, frame);
        }
        printf("T32S frames %d/%d\n", got, streams);
        STEP(IMP_FrameSource_SetFrameDepth(0, 0));
        STEP(IMP_FrameSource_DisableChn(0));
    }
    STEP(IMP_FrameSource_DestroyChn(0));
    if (enc && qp_cur < qp_last) {      /* T32_ENC_QPS: the next QP's channel cycle */
        if (got != streams)
            goto fail;
        printf("T32Q %d done\n", qp_cur);
        qp_cur++;
        got = 0;
        goto again;
    }
teardown:
    STEP(IMP_ISP_DisableTuning());
    STEP(IMP_System_Exit());
    STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
    STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
    STEP(IMP_ISP_Close());
    stream_copy_out();
    printf("T32S end ok=%d\n", got == streams);
    return got == streams ? 0 : 2;
fail:
    printf("T32S end failed\n");
    return 1;
}
