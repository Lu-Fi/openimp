/*
 * isp_osd_devtest - device proof of the T41 ISP-drawn OSD.
 *
 * Runs its own pipeline (timps stopped): FrameSource ch0 W x H (scaled from
 * the sensor if needed) and ch1 640x360, a JPEG encoder bound to ch0, and
 * the raptor-hal src/hal_osd.c call sequence on the ISP OSD:
 *   IMP_ISP_Tuning_SetOsdPoolSize, CreateOsdRgn(0, NULL),
 *   SetOsdRgnAttr(ISP_OSD_REG_PIC, ARGB8888/BGRA, pinum = handle,
 *   left/top & ~1, stride = width * 4), ShowOsdRgn(1/0), DestroyOsdRgn.
 * Phases: base; raptor exactly (BGRA, picture A at 64,64); picture D with
 * order ARGB (the B,G,R,A bytes of a little-endian ARGB8888 bitmap); moved to
 * (400,200) with picture B; second region C at (1200,600) (ch0
 * coordinates); hide the first; destroy both.  Per phase: one NV12 frame of ch0 and ch1 and one JPEG of
 * ch0, written to stdout as "FILE <name> <size>\n" + bytes (the host
 * splits them); ch1 frame interval over 40 frames as the fps check.
 * Also prints the driver readback (Tuning_GetOSDAttr / GetOSDBlock).
 *
 * usage: isp_osd_devtest <sensor> <i2c-addr> <ch0-w> <ch0-h> <sens-w> <sens-h>
 * Build: mipsel gcc -I<T41 vendor headers> ... -L<openimp build/t41> -limp
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>

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
 * at (24,24); B: green with a white frame; C: solid yellow, alpha 255. */
static void make_pics(void)
{
    int x, y;

    for (y = 0; y < PH; y++) {
        for (x = 0; x < PW; x++) {
            uint8_t *a = &pic_a[(y * PW + x) * 4];
            uint8_t *b = &pic_b[(y * PW + x) * 4];
            uint8_t *c = &pic_c[(y * PW + x) * 4];
            int frame = x < 8 || y < 8 || x >= PW - 8 || y >= PH - 8;
            uint8_t r = x < PW / 2 ? 255 : 0, g = 0, bl = x < PW / 2 ? 0 : 255;

            if (frame)
                r = g = bl = 255;
            if (x >= 24 && x < 40 && y >= 24 && y < 40)
                r = g = bl = 0;
            a[0] = bl; a[1] = g; a[2] = r; a[3] = 255;
            b[0] = frame ? 255 : 0; b[1] = 255; b[2] = frame ? 255 : 0; b[3] = 255;
            c[0] = 0; c[1] = 255; c[2] = 255; c[3] = 255;
            /* D: quadrants red / green / blue / yellow, 4-px white frame,
             * black 16x16 at (24,8), bottom 8 rows of the right half 50 %
             * alpha white; bytes B,G,R,A */
            {
                uint8_t *q = &pic_d[(y * PW + x) * 4];
                int top = y < PH / 2, left = x < PW / 2;
                uint8_t qr = top ? (left ? 255 : 0) : (left ? 0 : 255);
                uint8_t qg = top ? (left ? 0 : 255) : (left ? 0 : 255);
                uint8_t qb = top ? 0 : (left ? 255 : 0);
                uint8_t qa = 255;

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
}

/* raptor-hal hal_isp_osd_set_region_attr, T41 branch */
static int set_rgn_t(int handle, int x, int y, uint8_t *pic, int argb)
{
    IMPIspOsdAttrAsm a;

    memset(&a, 0, sizeof(a));
    a.type = ISP_OSD_REG_PIC;
    a.stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888;
    a.stsinglepicAttr.chnOSDAttr.osd_argb_type = (IMPISPARGBType)argb;
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

static int set_rgn(int handle, int x, int y, uint8_t *pic)
{
    return set_rgn_t(handle, x, y, pic, IMP_ISP_ARGB_TYPE_BGRA);
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
    /* NV12 with the chroma plane after a 16-line aligned luma plane */
    {
        size_t len = (size_t)w * ((h + 15) & ~15) * 3 / 2;

        if (fr->size && fr->size < len)
            len = fr->size;
        snprintf(name, sizeof(name), "%s_ch%d_%dx%d.nv12", phase, ch, w, h);
        emit(name, (void *)(uintptr_t)fr->virAddr, len);
    }
    IMP_FrameSource_ReleaseFrame(ch, fr);
    return 0;
}

static int jpeg(const char *phase)
{
    IMPEncoderStream st;
    static uint8_t buf[1 << 20];
    size_t n = 0;
    char name[64];
    int i, ret = -1;

    if (IMP_Encoder_StartRecvPic(0))
        return -1;
    /* skip one picture: it may predate the OSD change */
    for (i = 0; i < 2; i++) {
        n = 0;
        if (IMP_Encoder_PollingStream(0, 2000) || IMP_Encoder_GetStream(0, &st, 1))
            goto out;
        for (unsigned k = 0; k < st.packCount; k++) {
            uint32_t off = st.pack[k].offset, len = st.pack[k].length;
            uint32_t rem = st.streamSize - off;
            const uint8_t *b = (const uint8_t *)(uintptr_t)st.virAddr;

            if (n + len > sizeof(buf))
                break;
            if (len > rem) {
                memcpy(buf + n, b + off, rem);
                memcpy(buf + n + rem, b, len - rem);
            } else {
                memcpy(buf + n, b + off, len);
            }
            n += len;
        }
        IMP_Encoder_ReleaseStream(0, &st);
    }
    snprintf(name, sizeof(name), "%s_ch0.jpg", phase);
    emit(name, buf, n);
    ret = 0;
out:
    IMP_Encoder_StopRecvPic(0);
    return ret;
}

static double fps1(void)
{
    IMPFrameInfo *fr;
    double t0 = 0;
    int n = 0;

    while (n < 41) {
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
    return 40.0 / (now_s() - t0);
}

static void readback(const char *phase)
{
    IMPISPOSDAttr a;
    IMPISPOSDBlockAttr b;
    int i, r;

    memset(&a, 0, sizeof(a));
    r = IMP_ISP_Tuning_GetOSDAttr(IMPVI_MAIN, &a);
    fprintf(stderr, "EVT %s kattr ret %d type %d argb %d alpha_dis %d\n", phase,
            r, a.osd_type, a.osd_argb_type, a.osd_pixel_alpha_disable);
    for (i = 0; i < 3; i++) {
        memset(&b, 0, sizeof(b));
        b.pinum = (uint8_t)i;
        r = IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, &b);
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
    r |= jpeg(name);
    fprintf(stderr, "EVT %s captured %d fps1 %.2f\n", name, r, fps1());
    return r;
}

#define CK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "FAIL %s = %d\n", #x, r_); goto out; } } while (0)

int main(int argc, char **argv)
{
    IMPSensorInfo s;
    IMPFSChnAttr fs;
    IMPEncoderChnAttr ea;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_ENC, 0, 0 };
    int c, sw, sh, h0 = -1, h1 = -1, fps = 15;
    int st_fs[2] = {0}, st_en[2] = {0}, st_grp = 0, st_chn = 0, st_bind = 0;

    if (argc < 7) {
        fprintf(stderr, "usage: isp_osd_devtest sensor addr w h sensw sensh\n");
        return 2;
    }
    W0 = atoi(argv[3]);
    H0 = atoi(argv[4]);
    sw = atoi(argv[5]);
    sh = atoi(argv[6]);
    make_pics();
    memset(&s, 0, sizeof(s));
    strncpy(s.name, argv[1], sizeof(s.name) - 1);
    s.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(s.i2c.type, argv[1], sizeof(s.i2c.type) - 1);
    s.i2c.addr = (int)strtol(argv[2], NULL, 0);
    s.rst_gpio = s.pwdn_gpio = s.power_gpio = -1;

    fprintf(stderr, "EVT poolsize ret %d\n", IMP_ISP_Tuning_SetOsdPoolSize(256 * 1024));
    CK(IMP_ISP_Open());
    CK(IMP_ISP_AddSensor(IMPVI_MAIN, &s));
    CK(IMP_ISP_EnableSensor(IMPVI_MAIN, &s));
    CK(IMP_System_Init());
    CK(IMP_ISP_EnableTuning());
    for (c = 0; c < 2; c++) {
        int w = c ? W1 : W0, h = c ? H1 : H0;

        memset(&fs, 0, sizeof(fs));
        fs.picWidth = w;
        fs.picHeight = h;
        fs.pixFmt = PIX_FMT_NV12;
        if (w != sw || h != sh) {
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
    CK(IMP_Encoder_CreateGroup(0)); st_grp = 1;
    CK(IMP_Encoder_SetDefaultParam(&ea, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP,
                                   W0, H0, fps, 1, 0, 0, 30, 0));
    CK(IMP_Encoder_CreateChn(0, &ea)); st_chn = 1;
    CK(IMP_Encoder_RegisterChn(0, 0));
    CK(IMP_System_Bind(&src, &dst)); st_bind = 1;
    for (c = 0; c < 2; c++) {
        CK(IMP_FrameSource_EnableChn(c));
        st_en[c] = 1;
    }
    sleep(3);

    phase("p0base");

    /* 1: raptor-hal exactly (BGRA order, picture A at 64,64) */
    h0 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    fprintf(stderr, "EVT create h0 %d\n", h0);
    if (h0 < 0)
        goto out;
    fprintf(stderr, "EVT setattr h0 ret %d\n", set_rgn(h0, 64, 64, pic_a));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p1raptor");

    /* 2: the B,G,R,A bytes with component order ARGB: picture D */
    fprintf(stderr, "EVT setattr h0 D ret %d\n", set_rgn_t(h0, 64, 64, pic_d, IMP_ISP_ARGB_TYPE_ARGB));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p2argbD");

    /* 3: move + new picture (B, green with a white frame) */
    fprintf(stderr, "EVT update h0 ret %d\n", set_rgn_t(h0, 400, 200, pic_b, IMP_ISP_ARGB_TYPE_ARGB));
    fprintf(stderr, "EVT show h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 1));
    phase("p3moveB");

    /* 4: second region (C, yellow) */
    h1 = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
    fprintf(stderr, "EVT create h1 %d\n", h1);
    fprintf(stderr, "EVT setattr h1 ret %d\n", set_rgn_t(h1, W0 * 5 / 8, H0 * 5 / 9, pic_c, IMP_ISP_ARGB_TYPE_ARGB));
    fprintf(stderr, "EVT show h1 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h1, 1));
    phase("p4two");

    /* 5: hide the first */
    fprintf(stderr, "EVT hide h0 ret %d\n", IMP_ISP_Tuning_ShowOsdRgn(0, h0, 0));
    phase("p5hide");

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
    for (c = 0; c < 2; c++)
        if (st_en[c])
            IMP_FrameSource_DisableChn(c);
    if (st_bind)
        IMP_System_UnBind(&src, &dst);
    if (st_chn) {
        IMP_Encoder_UnRegisterChn(0);
        IMP_Encoder_DestroyChn(0);
    }
    if (st_grp)
        IMP_Encoder_DestroyGroup(0);
    for (c = 0; c < 2; c++)
        if (st_fs[c])
            IMP_FrameSource_DestroyChn(c);
    IMP_ISP_DisableTuning();
    IMP_System_Exit();
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN, &s);
    IMP_ISP_Close();
    fprintf(stderr, "DONE\n");
    return 0;
}
