/*
 * isp_osd_devtest - device proof of the T23 ISP-drawn OSD (issue #127).
 *
 * Runs its own pipeline (streamer stopped): FrameSource ch0 W x H (the
 * sensor size; the ISP OSD is drawn into MSCA output 0) and ch1 640x360,
 * and the raptor-hal src/hal_osd.c call sequence on the ISP OSD:
 *   IMP_OSD_Init_ISP, IMP_ISP_Tuning_SetOsdPoolSize, CreateOsdRgn(0, NULL),
 *   SetOsdRgnAttr(ISP_OSD_REG_PIC, ARGB8888, chx 0, pinum = handle,
 *   left/top & ~1, stride = width * 4), ShowOsdRgn(1/0), DestroyOsdRgn.
 * Phases: base; picture A (B,G,R,A bytes, order BGRA = 0, the T23 SDK's
 * name for "byte 0 is blue") at 64,64; picture D (quadrants, 50 % alpha
 * strip) at the same place; moved to (400,200) with picture B; second
 * region C at (5/8, 5/9 of ch0); hide the first; an odd picture width
 * (SetOSDBlock block 2 and SetOsdRgnAttr on C: refused, frames flow);
 * destroy both.  Per phase
 * one NV12 frame of ch0 and ch1, written to stdout as
 * "FILE <name> <size>\n" + bytes (the host splits them), the ch1 frame
 * interval over 40 frames as the fps check, and the driver readback
 * (Tuning_GetOSDAttr / GetOSDBlock).
 *
 * usage: isp_osd_devtest <sensor> <i2c-addr> <ch0-w> <ch0-h>
 * Build: tests/t23/devtest/build.sh (T23 toolchain, the camera's libimp).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>

extern int IMP_OSD_Init_ISP(void);
extern void IMP_OSD_Exit_ISP(void);

#define PW 128
#define PH 64
static uint8_t pic_a[PW * PH * 4], pic_b[PW * PH * 4], pic_c[PW * PH * 4],
    pic_d[PW * PH * 4];
static int W0, H0;
static const int W1 = 640, H1 = 360;

static double now_s(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* A: left half red, right half blue, 8-px white frame, black 16x16 square
 * at (24,24); B: green with a white frame; C: solid yellow; D: quadrants
 * red / green / blue / yellow, 4-px white frame, black 16x16 at (24,8),
 * bottom 8 rows of the right half 50 % alpha white.  Bytes B,G,R,A. */
static void make_pics(void)
{
    int x, y;

    for (y = 0; y < PH; y++) {
        for (x = 0; x < PW; x++) {
            uint8_t *a = &pic_a[(y * PW + x) * 4];
            uint8_t *b = &pic_b[(y * PW + x) * 4];
            uint8_t *c = &pic_c[(y * PW + x) * 4];
            uint8_t *q = &pic_d[(y * PW + x) * 4];
            int frame = x < 8 || y < 8 || x >= PW - 8 || y >= PH - 8;
            uint8_t r = x < PW / 2 ? 255 : 0, g = 0, bl = x < PW / 2 ? 0 : 255;
            int top = y < PH / 2, left = x < PW / 2;
            uint8_t qr = top ? (left ? 255 : 0) : (left ? 0 : 255);
            uint8_t qg = top ? (left ? 0 : 255) : (left ? 0 : 255);
            uint8_t qb = top ? 0 : (left ? 255 : 0);
            uint8_t qa = 255;

            if (frame)
                r = g = bl = 255;
            if (x >= 24 && x < 40 && y >= 24 && y < 40)
                r = g = bl = 0;
            a[0] = bl; a[1] = g; a[2] = r; a[3] = 255;
            b[0] = frame ? 255 : 0; b[1] = 255; b[2] = frame ? 255 : 0; b[3] = 255;
            c[0] = 0; c[1] = 255; c[2] = 255; c[3] = 255;
            if (x < 4 || y < 4 || x >= PW - 4 || y >= PH - 4)
                qr = qg = qb = 255;
            if (x >= 24 && x < 40 && y >= 8 && y < 24)
                qr = qg = qb = 0;
            if (!left && y >= PH - 12 && y < PH - 4 && x < PW - 4) {
                qr = qg = qb = 255;
                qa = 128;
            }
            q[0] = qb; q[1] = qg; q[2] = qr; q[3] = qa;
        }
    }
}

/* raptor-hal hal_isp_osd_set_region_attr, T23 branch (chx 0) */
static int set_rgn(int handle, int x, int y, uint8_t *pic)
{
    IMPIspOsdAttrAsm a;

    memset(&a, 0, sizeof(a));
    a.type = ISP_OSD_REG_PIC;
    a.stsinglepicAttr.chx = 0;
    a.stsinglepicAttr.sensornum = 0;
    a.stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888;
    a.stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_BGRA;
    a.stsinglepicAttr.chnOSDAttr.osd_pixel_alpha_disable = IMPISP_TUNING_OPS_MODE_DISABLE;
    a.stsinglepicAttr.pic.pinum = (uint8_t)handle;
    a.stsinglepicAttr.pic.osd_enable = 1;
    a.stsinglepicAttr.pic.osd_left = (uint16_t)(x & ~1);
    a.stsinglepicAttr.pic.osd_top = (uint16_t)(y & ~1);
    a.stsinglepicAttr.pic.osd_width = PW;
    a.stsinglepicAttr.pic.osd_height = PH;
    a.stsinglepicAttr.pic.osd_image = (char *)pic;
    a.stsinglepicAttr.pic.osd_stride = PW * 4;
    return IMP_ISP_Tuning_SetOsdRgnAttr(0, handle, &a);
}

static void emit(const char *name, const void *data, size_t size)
{
    printf("FILE %s %zu\n", name, size);
    fwrite(data, 1, size, stdout);
    fflush(stdout);
}

static int grab(int ch, const char *phase)
{
    IMPFrameInfo *fr = NULL;
    char name[64];
    int t, w = ch ? W1 : W0, h = ch ? H1 : H0;

    /* drop what is queued, then take a fresh frame */
    for (t = 0; t < 3; t++)
        if (IMP_FrameSource_GetFrame(ch, &fr) == 0 && fr)
            IMP_FrameSource_ReleaseFrame(ch, fr);
    for (t = 0; t < 40; t++) {
        fr = NULL;
        if (IMP_FrameSource_GetFrame(ch, &fr) == 0 && fr)
            break;
        usleep(50000);
    }
    if (!fr)
        return -1;
    {
        size_t len = (size_t)w * h * 3 / 2;

        if (fr->size && fr->size < len)
            len = fr->size;
        snprintf(name, sizeof(name), "%s_ch%d_%dx%d.nv12", phase, ch, w, h);
        emit(name, (void *)(uintptr_t)fr->virAddr, len);
    }
    IMP_FrameSource_ReleaseFrame(ch, fr);
    return 0;
}

static double fps1(void)
{
    IMPFrameInfo *fr;
    double t0 = 0;
    int n = 0, tries = 0;

    while (n < 41 && tries++ < 2000) {
        fr = NULL;
        if (IMP_FrameSource_GetFrame(1, &fr) || !fr) {
            usleep(5000);
            continue;
        }
        IMP_FrameSource_ReleaseFrame(1, fr);
        if (!n)
            t0 = now_s();
        n++;
    }
    return n > 1 ? (n - 1) / (now_s() - t0) : 0;
}

static void readback(const char *phase)
{
    IMPISPOSDAttr a;
    IMPISPOSDBlockAttr b;
    int i, r;

    memset(&a, 0, sizeof(a));
    r = IMP_ISP_Tuning_GetOSDAttr(&a);
    fprintf(stderr, "EVT %s kattr ret %d type %d argb %d alpha_dis %d\n", phase,
            r, a.osd_type, a.osd_argb_type, a.osd_pixel_alpha_disable);
    for (i = 0; i < 3; i++) {
        memset(&b, 0, sizeof(b));
        b.pinum = (uint8_t)i;
        r = IMP_ISP_Tuning_GetOSDBlock(&b);
        fprintf(stderr, "EVT %s kblock%d ret %d pinum %d en %d %u,%u %ux%u img %08x stride %u\n",
                phase, i, r, b.pinum, b.osd_enable, b.osd_left, b.osd_top,
                b.osd_width, b.osd_height, (unsigned)(uintptr_t)b.osd_image,
                b.osd_stride);
    }
}

static int phase(const char *name)
{
    int r = 0;

    usleep(400000);
    readback(name);
    r |= grab(0, name);
    r |= grab(1, name);
    fprintf(stderr, "EVT %s captured %d fps1 %.2f\n", name, r, fps1());
    return r;
}

#define CK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "FAIL %s = %d\n", #x, r_); goto out; } } while (0)

int main(int argc, char **argv)
{
    IMPSensorInfo s;
    IMPFSChnAttr fs;
    int c, h0 = -1, h1 = -1, fps = 15;
    int st_fs[2] = {0}, st_en[2] = {0}, st_osd = 0;

    if (argc < 5) {
        fprintf(stderr, "usage: isp_osd_devtest sensor addr w h\n");
        return 2;
    }
    W0 = atoi(argv[3]);
    H0 = atoi(argv[4]);
    make_pics();
    memset(&s, 0, sizeof(s));
    strncpy(s.name, argv[1], sizeof(s.name) - 1);
    s.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(s.i2c.type, argv[1], sizeof(s.i2c.type) - 1);
    s.i2c.addr = (int)strtol(argv[2], NULL, 0);

    CK(IMP_ISP_Open());
    CK(IMP_ISP_AddSensor(&s));
    CK(IMP_ISP_EnableSensor());
    CK(IMP_System_Init());
    CK(IMP_ISP_EnableTuning());
    for (c = 0; c < 2; c++) {
        int w = c ? W1 : W0, h = c ? H1 : H0;

        memset(&fs, 0, sizeof(fs));
        fs.picWidth = w;
        fs.picHeight = h;
        fs.pixFmt = PIX_FMT_NV12;
        if (c) {
            fs.scaler.enable = 1;
            fs.scaler.outwidth = w;
            fs.scaler.outheight = h;
        }
        fs.outFrmRateNum = fps;
        fs.outFrmRateDen = 1;
        fs.nrVBs = 2;
        fs.type = FS_PHY_CHANNEL;
        CK(IMP_FrameSource_CreateChn(c, &fs)); st_fs[c] = 1;
        CK(IMP_FrameSource_SetChnAttr(c, &fs));
        IMP_FrameSource_SetFrameDepth(c, 1);
    }
    for (c = 0; c < 2; c++) {
        CK(IMP_FrameSource_EnableChn(c));
        st_en[c] = 1;
    }
    sleep(3);

    phase("p0base");

    fprintf(stderr, "EVT osdinit ret %d\n", IMP_OSD_Init_ISP());
    st_osd = 1;
    fprintf(stderr, "EVT poolsize ret %d\n", IMP_ISP_Tuning_SetOsdPoolSize(256 * 1024));
    /* 1: raptor-hal (picture A at 64,64) */
    h0 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    fprintf(stderr, "EVT create h0 %d\n", h0);
    if (h0 < 0)
        goto out;
    fprintf(stderr, "EVT setattr h0 ret %d\n", set_rgn(h0, 64, 64, pic_a));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p1raptorA");

    /* 2: picture D (quadrants, alpha strip) */
    fprintf(stderr, "EVT setattr h0 D ret %d\n", set_rgn(h0, 64, 64, pic_d));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p2quadD");

    /* 3: move + new picture (B, green with a white frame) */
    fprintf(stderr, "EVT update h0 ret %d\n", set_rgn(h0, 400, 200, pic_b));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p3moveB");

    /* 4: second region (C, yellow) */
    h1 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    fprintf(stderr, "EVT create h1 %d\n", h1);
    fprintf(stderr, "EVT setattr h1 ret %d\n", set_rgn(h1, W0 * 5 / 8, H0 * 5 / 9, pic_c));
    fprintf(stderr, "EVT show h1 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h1, 1));
    phase("p4two");

    /* 5: hide the first */
    fprintf(stderr, "EVT hide h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 0));
    phase("p5hide");

    /* 5b: odd picture width (beyond vendor: refused, nothing stored, the
     * ISP keeps running).  Direct SetOSDBlock on block 2, then through the
     * region API on h1. */
    {
        IMPISPOSDBlockAttr ob;

        memset(&ob, 0, sizeof(ob));
        ob.pinum = 2;
        ob.osd_enable = 1;
        ob.osd_left = 200;
        ob.osd_top = 100;
        ob.osd_width = PW - 1;
        ob.osd_height = PH;
        ob.osd_image = (char *)pic_c;
        ob.osd_stride = PW * 4;
        fprintf(stderr, "EVT oddw block2 ret %d\n", IMP_ISP_Tuning_SetOSDBlock(&ob));
    }
    {
        IMPIspOsdAttrAsm a;

        memset(&a, 0, sizeof(a));
        IMP_ISP_Tuning_GetOsdRgnAttr(0, h1, &a);
        a.stsinglepicAttr.pic.osd_width = PW - 1;
        fprintf(stderr, "EVT oddw rgn h1 ret %d\n", IMP_ISP_Tuning_SetOsdRgnAttr(0, h1, &a));
        fprintf(stderr, "EVT oddw show h1 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h1, 1));
    }
    phase("p5oddw");

    /* 6: destroy both */
    fprintf(stderr, "EVT destroy h0 ret %d h1 ret %d\n",
            IMP_ISP_Tuning_DestroyOsdRgn(0, h0), IMP_ISP_Tuning_DestroyOsdRgn(0, h1));
    h0 = h1 = -1;
    phase("p6destroy");
out:
    if (h0 >= 0)
        IMP_ISP_Tuning_DestroyOsdRgn(0, h0);
    if (h1 >= 0)
        IMP_ISP_Tuning_DestroyOsdRgn(0, h1);
    if (st_osd)
        IMP_OSD_Exit_ISP();
    for (c = 0; c < 2; c++)
        if (st_en[c])
            IMP_FrameSource_DisableChn(c);
    for (c = 0; c < 2; c++)
        if (st_fs[c])
            IMP_FrameSource_DestroyChn(c);
    IMP_ISP_DisableTuning();
    IMP_System_Exit();
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&s);
    IMP_ISP_Close();
    fprintf(stderr, "DONE\n");
    return 0;
}
