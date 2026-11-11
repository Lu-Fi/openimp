/*
 * T32 ISP tuning probe for QEMU -M ingenic-t32 (#296).
 *
 * Runs the ISP / sensor bring-up (Open, AddSensor, EnableSensor,
 * System_Init, EnableTuning) and then every IMP_ISP_Tuning_* entry point of
 * the vendor 2.x API that reaches /dev/isp-m0 (plus Csccr / HB): each Set
 * with a fixed, valid input, each Get right after its Set (round trip).
 * The functions are looked up with dlsym, so the same binary runs on vendor
 * libimp 2.2.0 and on OpenIMP; parameters are opaque buffers of the vendor
 * structure size (layouts: vendor 2.x imp_isp.h, sizes in the table).
 * Before each call the payload size is handed to the LD_PRELOAD recorder
 * (oi_trace_tuning_size), which then logs the bytes behind the request's
 * value pointer ("P" lines; tools/t32/t32_tuning_table.py makes the table).
 * stdout: "T32T <function> rc=<rc> [out=<hex>]" per call.
 * Use it through tools/t32/qemu-ioctl-trace.sh with
 * SAMPLE_SRC=tools/t32/t32_tuning_sample.c (mode "tuning").
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>

#if !defined(PLATFORM_T32)
#error "build with -DPLATFORM_T32"
#endif

enum { K_GET, K_SET, K_SET1, K_GETV };  /* (num, p) Get / Set, Set(p) without num */

struct tcase {
    const char *name;
    int kind;
    unsigned size;      /* vendor structure size */
    unsigned pay;       /* bytes behind the request's pointer (0 = size) */
    const char *input;  /* "w:v,v,.." 32-bit words / "f:.." floats from word 2 (CCM) /
                           "c:.." CSC / "b:x" one byte / NULL pattern */
};

/* order = run order; a Get follows its Set */
static const struct tcase cases[] = {
    { "IMP_ISP_Tuning_SetSensorFPS", K_SET, 8, 0, "w:10,1" },
    { "IMP_ISP_Tuning_GetSensorFPS", K_GET, 8, 0, NULL },
    { "IMP_ISP_Tuning_SetAntiFlickerAttr", K_SET, 8, 5, "w:1,50" },
    { "IMP_ISP_Tuning_GetAntiFlickerAttr", K_GET, 8, 5, NULL },
    { "IMP_ISP_Tuning_SetHVFLIP", K_SET, 16, 0, "w:1,0,1,0" },
    { "IMP_ISP_Tuning_GetHVFLIP", K_GET, 16, 0, NULL },
    { "IMP_ISP_Tuning_SetBrightness", K_SET, 1, 0, "b:0x90" },
    { "IMP_ISP_Tuning_GetBrightness", K_GET, 1, 0, NULL },
    { "IMP_ISP_Tuning_SetContrast", K_SET, 1, 0, "b:0x91" },
    { "IMP_ISP_Tuning_GetContrast", K_GET, 1, 0, NULL },
    { "IMP_ISP_Tuning_SetSharpness", K_SET, 1, 0, "b:0x92" },
    { "IMP_ISP_Tuning_GetSharpness", K_GET, 1, 0, NULL },
    { "IMP_ISP_Tuning_SetSaturation", K_SET, 1, 0, "b:0x93" },
    { "IMP_ISP_Tuning_GetSaturation", K_GET, 1, 0, NULL },
    { "IMP_ISP_Tuning_SetBcshHue", K_SET, 1, 0, "b:0x94" },
    { "IMP_ISP_Tuning_GetBcshHue", K_GET, 1, 0, NULL },
    { "IMP_ISP_Tuning_SetBcshRGBOffsetAttr", K_SET, 6, 0, NULL },
    { "IMP_ISP_Tuning_GetBcshRGBOffsetAttr", K_GET, 6, 0, NULL },
    { "IMP_ISP_Tuning_SetModuleControl", K_SET, 4, 0, NULL },
    { "IMP_ISP_Tuning_GetModuleControl", K_GET, 4, 0, NULL },
    { "IMP_ISP_Tuning_SetModule_Ratio", K_SET, 128, 0, NULL },
    { "IMP_ISP_Tuning_GetModule_Ratio", K_GET, 128, 0, NULL },
    { "IMP_ISP_Tuning_SetISPCSCAttr", K_SET, 92, 0, "c:" },
    { "IMP_ISP_Tuning_GetISPCSCAttr", K_GET, 92, 0, NULL },
    { "IMP_ISP_Tuning_SetCCMAttr", K_SET, 44, 40, "f:1,0,1.5,-0.25,-0.25,-0.5,1.75,-0.25,0,-0.75,1.75" },
    { "IMP_ISP_Tuning_GetCCMAttr", K_GET, 44, 40, NULL },
    { "IMP_ISP_Tuning_SetGammaAttr", K_SET, 264, 0, "w:1" },
    { "IMP_ISP_Tuning_GetGammaAttr", K_GET, 264, 0, NULL },
    { "IMP_ISP_Tuning_SetStatisConfig", K_SET, 72, 0, NULL },
    { "IMP_ISP_Tuning_GetStatisConfig", K_GET, 72, 0, NULL },
    { "IMP_ISP_Tuning_SetAeExprInfo", K_SET, 204, 0, NULL },
    { "IMP_ISP_Tuning_GetAeExprInfo", K_GET, 204, 0, NULL },
    { "IMP_ISP_Tuning_GetAeOnlyReadAttr", K_GET, 48, 0, NULL },
    { "IMP_ISP_Tuning_SetAeTargetAttr", K_SET, 264, 0, NULL },
    { "IMP_ISP_Tuning_GetAeTargetAttr", K_GET, 264, 0, NULL },
    { "IMP_ISP_Tuning_SetAeLightCompensation", K_SET, 8, 0, NULL },
    { "IMP_ISP_Tuning_GetAeLightCompensation", K_GET, 8, 0, NULL },
    { "IMP_ISP_Tuning_SetAwbAttr", K_SET, 60, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbAttr", K_GET, 60, 0, NULL },
    { "IMP_ISP_Tuning_SetAfWeight", K_SET, 225, 0, NULL },
    { "IMP_ISP_Tuning_GetAfWeight", K_GET, 225, 0, NULL },
    { "IMP_ISP_Tuning_SetAwbWeight", K_SET, 1024, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbWeight", K_GET, 1024, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbStatistics", K_GET, 12288, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbGlobalStatistics", K_GET, 16, 0, NULL },
    { "IMP_ISP_Tuning_GetAeStatistics", K_GET, 1934, 0, NULL },
    { "IMP_ISP_Tuning_SetAeWeight", K_SET, 232, 0, NULL },
    { "IMP_ISP_Tuning_GetAeWeight", K_GET, 232, 0, NULL },
    { "IMP_ISP_Tuning_GetAFMetricesInfo", K_GET, 12, 0, NULL },
    { "IMP_ISP_Tuning_SetAeExpList", K_SET, 324, 0, NULL },
    { "IMP_ISP_Tuning_GetAeExpList", K_GET, 324, 0, NULL },
    { "IMP_ISP_Tuning_GetAEFlickerFlag", K_GET, 4, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbOnlyReadAttr", K_GET, 4, 0, NULL },
    { "IMP_ISP_Tuning_SetAutoZoom", K_SET, 60, 0, NULL },
    { "IMP_ISP_Tuning_GetAutoZoom", K_GET, 60, 0, NULL },
    { "IMP_ISP_Tuning_SetOSDAttr", K_SET, 32, 0, NULL },
    { "IMP_ISP_Tuning_GetOSDAttr", K_GET, 32, 0, NULL },
    { "IMP_ISP_Tuning_SetOSDBlock", K_SET, 20, 0, NULL },
    { "IMP_ISP_Tuning_GetOSDBlock", K_GET, 20, 0, NULL },
    { "IMP_ISP_Tuning_SetWdrOutputMode", K_SET, 4, 0, NULL },
    { "IMP_ISP_Tuning_GetWdrOutputMode", K_GET, 4, 0, NULL },
    { "IMP_ISP_Tuning_SetScalerLv", K_SET, 12, 0, NULL },
    { "IMP_ISP_Tuning_SetAwbCtTrendOffset", K_SET, 28, 0, NULL },
    { "IMP_ISP_Tuning_GetAwbCtTrendOffset", K_GET, 28, 0, NULL },
    { "IMP_ISP_Tuning_SetFaceAe", K_SET, 24, 0, NULL },
    { "IMP_ISP_Tuning_GetFaceAe", K_GET, 24, 0, NULL },
    { "IMP_ISP_Tuning_SetFaceAeWeiget", K_SET, 12, 0, NULL },
    { "IMP_ISP_Tuning_GetFaceAeWeiget", K_GET, 12, 0, NULL },
    { "IMP_ISP_Tuning_GetFaceAeLuma", K_GET, 8, 0, NULL },
    { "IMP_ISP_Tuning_SetFaceAwb", K_SET, 24, 0, NULL },
    { "IMP_ISP_Tuning_GetFaceAwb", K_GET, 24, 0, NULL },
    { "IMP_ISP_Tuning_SetTmoFaceae", K_SET, 56, 0, NULL },
    { "IMP_ISP_Tuning_SetMultiSensorAeEffect", K_SET1, 72, 0, NULL },
    { "IMP_ISP_Tuning_GetMultiSensorAeEffect", K_GET, 72, 0, NULL },
    { "IMP_ISP_Tuning_SetMultiSensorAWBEffect", K_SET1, 72, 0, NULL },
    { "IMP_ISP_Tuning_GetMultiSensorAWBEffect", K_GET, 72, 0, NULL },
    { "IMP_ISP_Tuning_SetMultiSensorEffect", K_SET1, 72, 0, NULL },
    { "IMP_ISP_Tuning_GetMultiSensorEffect", K_GET, 72, 0, NULL },
    { "IMP_ISP_SetCsccrMode", K_SET, 12, 0, NULL },
    { "IMP_ISP_GetCsccrMode", K_GET, 12, 0, NULL },
    { "IMP_ISP_HB_SetAttr", K_SET, 2, 0, NULL },
    { "IMP_ISP_HB_GetAttr", K_GET, 2, 0, NULL },
    { "IMP_ISP_Tuning_SetAeStartAttr", K_SET, 8, 0, "w:1,1024" },
    { "IMP_ISP_Tuning_GetAeStartAttr", K_GET, 8, 0, NULL },
    { "IMP_ISP_Tuning_SetMaskBlock", K_SET, 24, 0, "w:0,0,0,0,0,0" },
    { "IMP_ISP_Tuning_GetMaskBlock", K_GET, 24, 0, NULL },
    { "IMP_ISP_Tuning_SetDrawBlock", K_SET, 36, 0, "w:0,0,0,0,0,0,0,0,0" },
    { "IMP_ISP_Tuning_GetDrawBlock", K_GET, 36, 0, NULL },
    { "IMP_ISP_Tuning_GetSensorAttr", K_GET, 20, 0, NULL },
    { "IMP_ISP_Tuning_GetAfStatistics", K_GET, 5400, 0, NULL },
    { "IMP_ISP_Tuning_WaitFrameDone", K_GET, 24, 0, "w:0,0,0,0,0,0" },
};

static void (*mark)(const char *);
static void (*tsize)(unsigned);
static unsigned char buf[16384];

static void fill(const struct tcase *c)
{
    unsigned i, n = 0;
    const char *p;
    char *e;

    for (i = 0; i < c->size; i++)          /* pattern: never all-zero, varies per byte */
        buf[i] = (unsigned char)(i * 37u + 11u);
    if (c->kind == K_GET && !c->input) {   /* Get: the driver's bytes over a marker */
        memset(buf, 0xa5, c->size);
        return;
    }
    if (!c->input)
        return;
    p = c->input + 2;
    if (c->input[0] == 'b') {
        buf[0] = (unsigned char)strtoul(p, NULL, 0);
    } else if (c->input[0] == 'w') {
        memset(buf, 0, c->size);
        while (*p && n * 4 + 4 <= c->size) {
            uint32_t v = (uint32_t)strtoul(p, &e, 0);
            memcpy(buf + 4 * n++, &v, 4);
            p = *e ? e + 1 : e;
        }
    } else if (c->input[0] == 'f') {        /* CCM: mode words, then 9 floats */
        memset(buf, 0, c->size);
        while (*p && n * 4 + 4 <= c->size) {
            float f = strtof(p, &e);
            if (n < 2) {
                uint32_t v = (uint32_t)f;
                memcpy(buf + 4 * n, &v, 4);
            } else {
                memcpy(buf + 4 * n, &f, 4);
            }
            n++;
            p = *e ? e + 1 : e;
        }
    } else if (c->input[0] == 'c') {        /* CSC: mode 6 (user), 2 x 9 floats, bytes */
        uint32_t mode = 6;
        memset(buf, 0, c->size);
        memcpy(buf, &mode, 4);
        for (i = 0; i < 9; i++) {
            float a = 0.25f + 0.125f * (float)i, b = -0.5f + 0.0625f * (float)i;
            memcpy(buf + 4 + 4 * i, &a, 4);
            memcpy(buf + 48 + 4 * i, &b, 4);
        }
        for (i = 0; i < 6; i++) {
            buf[40 + i] = (unsigned char)(16 + i);
            buf[84 + i] = (unsigned char)(128 + i);
        }
    }
}

static void drop_cb(void)
{
}

static void run_case(const struct tcase *c, int num)
{
    void *fn = dlsym(RTLD_DEFAULT, c->name);
    char text[96];
    unsigned i;
    int rc;

    if (!fn) {
        printf("T32T %s missing\n", c->name);
        return;
    }
    fill(c);
    snprintf(text, sizeof text, "%s(%d, &a)", c->name, num);
    if (mark)
        mark(text);
    if (tsize)
        tsize(c->pay ? c->pay : c->size);
    if (c->kind == K_SET1)
        rc = ((int (*)(void *))fn)(buf);
    else
        rc = ((int (*)(int, void *))fn)(num, buf);
    printf("T32T %s(%d) rc=%d", c->name, num, rc);
    if (c->kind == K_GET && rc == 0) {
        printf(" out=");
        for (i = 0; i < c->size && i < 512; i++)
            printf("%02x", buf[i]);
    }
    printf("\n");
    fflush(stdout);
}

static int step(const char *name, int rc)
{
    printf("T32S %-40s rc=%d\n", name, rc);
    fflush(stdout);
    return rc;
}

#define STEP(call) (mark ? mark(#call) : (void)0, step(#call, (call)))
#define MUST(call) do { if (STEP(call)) goto fail; } while (0)

int main(int argc, char **argv)
{
    const char *only = getenv("T32_TUNING_ONLY");   /* substring filter */
    IMPSensorInfo si;
    unsigned i;
    int (*vdrop)(void (*)(void));
    static const struct tcase early = { "IMP_ISP_Tuning_GetBrightness", K_GET, 1, 0, NULL };
    static const struct tcase badvi = { "IMP_ISP_Tuning_SetBrightness", K_SET, 1, 0, "b:0x80" };

    (void)argc;
    (void)argv;
    mark = (void (*)(const char *))dlsym(RTLD_DEFAULT, "oi_trace_mark");
    tsize = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "oi_trace_tuning_size");
    memset(&si, 0, sizeof si);
    snprintf(si.name, sizeof si.name, "cv8001");
    si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof si.i2c.type, "cv8001");
    si.i2c.addr = 0x35;
    si.rst_gpio = si.pwdn_gpio = si.power_gpio = si.switch_gpio = -1;

    MUST(IMP_ISP_Open());
    MUST(IMP_ISP_AddSensor(IMPVI_MAIN, &si));
    MUST(IMP_ISP_EnableSensor(IMPVI_MAIN, &si));
    MUST(IMP_System_Init());
    run_case(&early, 0);                    /* before EnableTuning */
    MUST(IMP_ISP_EnableTuning());
    run_case(&badvi, 4);                    /* VI number out of range */
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!only || strstr(cases[i].name, only))
            run_case(&cases[i], 0);
    vdrop = (int (*)(void (*)(void)))dlsym(RTLD_DEFAULT, "IMP_ISP_Tuning_SetVideoDrop");
    if (vdrop) {
        if (mark)
            mark("IMP_ISP_Tuning_SetVideoDrop(cb)");
        printf("T32T IMP_ISP_Tuning_SetVideoDrop rc=%d\n", vdrop(drop_cb));
    }
    STEP(IMP_ISP_DisableTuning());
    run_case(&early, 0);                    /* after DisableTuning */
    STEP(IMP_System_Exit());
    STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
    STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
    STEP(IMP_ISP_Close());
    printf("T32T done\n");
    return 0;
fail:
    printf("T32T failed\n");
    return 1;
}
