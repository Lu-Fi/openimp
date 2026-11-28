/* t33_jointtest.c (#784 joint port proof): FrameSource joint of chn 1 (640x360) and chn 2 (320x180) side by side,
 * output chn 1 (960x360), vendor 2.0.2.1 vs OpenIMP.  Built like t32_trace_sample (OpenIMP headers, PLATFORM_T32).
 * args: MODE N  (MODE b0 / b1 = SetJointBackground 0 / 1), env JT_NOCOPY=1 (depth copy type 1: GetFrame = the
 * capture buffer), JT_DUMP=path (Y plane of the last joint frame, W x H bytes).
 * Lines "JT ...": return codes, joint size, frame sizes, mean luma of the regions A (chn 1 part), B (chn 2 part),
 * C (background below chn 2), chn 2's own frame, a 12 x 6 luma grid of the joint frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>

typedef struct { uint8_t position[12][12]; int16_t width, height; int32_t handler; } JointAttr;
int IMP_FrameSource_CreateJoint(JointAttr *a);
int IMP_FrameSource_DestroyJoint(int32_t h);
int IMP_FrameSource_GetJointByHandler(JointAttr *a);
int IMP_FrameSource_SetJointBackground(uint32_t c);
int IMP_FrameSource_SetFrameDepthCopyType(int chn, int nocopy);

#define P(name, expr) ({ int r_ = (expr); printf("JT %s 0x%x\n", name, (unsigned)r_); fflush(stdout); r_; })

static double mean(const uint8_t *y, int stride, int x0, int y0, int w, int h)
{
    long s = 0; int r, c;
    for (r = 0; r < h; r += 2) for (c = 0; c < w; c += 2) s += y[(long)(y0 + r) * stride + x0 + c];
    return (double)s / ((double)((h + 1) / 2) * ((w + 1) / 2));
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "b0";
    int n = argc > 2 ? atoi(argv[2]) : 5, nocopy = getenv("JT_NOCOPY") && atoi(getenv("JT_NOCOPY"));
    static const int sz[3][2] = { { 0, 0 }, { 640, 360 }, { 320, 180 } };
    IMPSensorInfo si; IMPFSChnAttr fs; JointAttr j, g;
    int c, i, ok = 0, W = 0, H = 0;

    {   /* which libimp ran (library check): its mapped path */
        char l[256]; FILE *m = fopen("/proc/self/maps", "r");
        while (m && fgets(l, sizeof l, m)) if (strstr(l, "libimp")) { printf("JT lib %s", strchr(l, '/') ? strchr(l, '/') : l); break; }
        if (m) fclose(m);
    }
    memset(&si, 0, sizeof si);
    snprintf(si.name, sizeof si.name, "cv2005"); si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof si.i2c.type, "cv2005"); si.i2c.addr = 0x35; si.i2c.i2c_adapter = 0;
    si.rst_gpio = si.pwdn_gpio = si.power_gpio = -1; si.switch_gpio = -1;
    if (IMP_ISP_Open() || IMP_ISP_AddSensor(IMPVI_MAIN, &si) || IMP_ISP_EnableSensor(IMPVI_MAIN, &si) || IMP_System_Init()) {
        printf("JT bringup FAIL\n"); return 1; }
    memset(&fs, 0, sizeof fs);
    fs.pixFmt = PIX_FMT_NV12; fs.scaler.enable = 1; fs.outFrmRateNum = 15; fs.outFrmRateDen = 1; fs.nrVBs = 2;
    fs.type = FS_PHY_CHANNEL;
    for (c = 1; c <= 2; c++) {
        IMPFSChnAttr a = fs;
        char t[24];
        a.picWidth = a.scaler.outwidth = sz[c][0];
        a.picHeight = a.scaler.outheight = sz[c][1];
        snprintf(t, sizeof t, "createchn%d", c);
        P(t, IMP_FrameSource_CreateChn(c, &a));
    }
    P("setbg", IMP_FrameSource_SetJointBackground(mode[1] == '1' ? 1 : 0));
    memset(&j, 0xff, sizeof j);
    j.position[0][0] = 1; j.position[0][1] = 2;
    if (getenv("JT_NOJOINT")) { j.width = 960; j.height = 360; j.handler = 0; printf("JT control: no joint\n"); }
    else P("createjoint", IMP_FrameSource_CreateJoint(&j));
    W = j.width; H = j.height;
    printf("JT joint %dx%d\n", W, H);
    memset(&g, 0, sizeof g); g.handler = j.handler;
    P("getjoint", IMP_FrameSource_GetJointByHandler(&g));
    printf("JT getjoint row0 %d %d %d\n", g.position[0][0], g.position[0][1], g.position[0][2]);
    for (c = 1; c <= 2; c++) {
        char t[24];
        snprintf(t, sizeof t, "enable%d", c);
        if (P(t, IMP_FrameSource_EnableChn(c))) goto out;
        if (nocopy) { snprintf(t, sizeof t, "nocopy%d", c); P(t, IMP_FrameSource_SetFrameDepthCopyType(c, 1)); }
        snprintf(t, sizeof t, "depth%d", c);
        P(t, IMP_FrameSource_SetFrameDepth(c, 1));
    }
    for (i = 0; i < n; i++) {
        IMPFrameInfo *f1 = NULL, *f2 = NULL;
        int r1 = IMP_FrameSource_GetFrame(1, (void **)&f1), r2 = IMP_FrameSource_GetFrame(2, (void **)&f2);
        if (r1 || r2) { printf("JT frame %d get %x %x\n", i, (unsigned)r1, (unsigned)r2); }
        if (!r1) {
            const uint8_t *y = (const uint8_t *)(uintptr_t)f1->virAddr;
            printf("JT frame %d chn1 %ux%u size %u", i, f1->width, f1->height, f1->size);
            if ((int)f1->width == W && (int)f1->height >= H && W >= 960 && H >= 360) {
                printf(" A %.1f B %.1f C %.1f", mean(y, W, 0, 0, 640, 360), mean(y, W, 640, 0, 320, 180),
                       mean(y, W, 640, 180, 320, 180));
                if (nocopy) {
                    const uint8_t *uv = y + ((W + 15) & ~15) * ((H + 15) & ~15);
                    printf(" uvC %.1f", mean(uv, W, 640, 90, 320, 90));
                }
                if (i == n - 1) {
                    int gr, gc;
                    for (gr = 0; gr < 6; gr++) {
                        printf("\nJT grid%d", gr);
                        for (gc = 0; gc < 12; gc++) printf(" %3.0f", mean(y, W, gc * W / 12, gr * H / 6, W / 12, H / 6));
                    }
                    if (getenv("JT_DUMP")) { FILE *o = fopen(getenv("JT_DUMP"), "wb"); if (o) { fwrite(y, 1, (size_t)W * H, o); fclose(o); } }
                }
                ok++;
            }
            printf("\n");
        }
        if (!r2) {
            const uint8_t *y = (const uint8_t *)(uintptr_t)f2->virAddr;
            printf("JT frame %d chn2 %ux%u size %u own %.1f", i, f2->width, f2->height, f2->size,
                   y ? mean(y, nocopy ? W : (int)((f2->width + 15) & ~15), 0, 0, f2->width, f2->height) : -1.0);
            if (nocopy && !r1) printf(" pa-delta %d", (int)(f2->phyAddr - f1->phyAddr));
            printf("\n");
        }
        fflush(stdout);
        if (!r1) IMP_FrameSource_ReleaseFrame(1, f1);
        if (!r2) IMP_FrameSource_ReleaseFrame(2, f2);
    }
out:
    printf("JT joint frames ok %d/%d\n", ok, n);
    fflush(stdout);
    for (c = 2; c >= 1; c--) { char t[24]; snprintf(t, sizeof t, "disable%d", c); P(t, IMP_FrameSource_DisableChn(c)); }
    P("destroyjoint", IMP_FrameSource_DestroyJoint(j.handler));
    for (c = 2; c >= 1; c--) IMP_FrameSource_DestroyChn(c);
    IMP_System_Exit();
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN, &si);
    IMP_ISP_Close();
    printf("JT end\n");
    return 0;
}
