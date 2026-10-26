/* apitest area "cases": targeted device cases for vendor functions that had no case in apitest/imgfx/devtest
 * ("implemented, not device-tested" cells) and the unclear ones of the first evidence runs.
 *
 * Every case is SAFE by construction: the original value is read first and written back at the end (and the
 * picture is checked to have recovered), nothing writes flash, reboots, reloads modules, plays sound or leaves
 * the ISP in bypass/frozen state.  Frame statistics are taken in memory from FS ch1 (mean Y/U/V, chroma
 * magnitude, detail energy); no picture is written or leaves the camera.
 *
 * Verdicts: PASS = call ok, readback ok and (where a picture effect is expected) the effect was measured;
 *           "rt: ..." in the detail (PASS line) = call + readback ok but no effect expected or measurable here
 *           (results2json turns it into evidence level rt); FAIL = call failed, readback wrong, frames stopped,
 *           or the picture did not recover.
 */
#define _GNU_SOURCE 1
#include <math.h>
#include <unistd.h>
#include "apitest.h"

typedef struct { double y, u, v, c, h; int ok; } Fst;

/* mean Y/U/V, chroma magnitude and detail energy of 2 frames of FS ch1 (after a settle time and 3 dropped frames) */
static Fst fst(int settle_ms)
{
    Fst s = { 0, 0, 0, 0, 0, 0 };
    int k, f;

    if (!IMP_FrameSource_GetFrame) return s;
    usleep(settle_ms * 1000);
    for (k = 0; k < 3; k++) {
        IMPFrameInfo *fr = NULL;
        if (IMP_FrameSource_GetFrame(1, &fr) == 0 && fr) IMP_FrameSource_ReleaseFrame(1, fr);
    }
    for (f = 0; f < 2; f++) {
        IMPFrameInfo *fr = NULL;
        const unsigned char *y, *uv;
        int w, h, i, j, n = 0;
        double sy = 0, su = 0, sv = 0, sc = 0, lap = 0;

        if (IMP_FrameSource_GetFrame(1, &fr) < 0 || !fr) continue;
        y = (const unsigned char *)(uintptr_t)fr->virAddr; w = fr->width; h = fr->height;
        if (!y || w < 16 || h < 16) { IMP_FrameSource_ReleaseFrame(1, fr); continue; }
        uv = y + (size_t)w * h;
        for (j = 1; j < h - 1; j += 2)
            for (i = 1; i < w - 1; i += 2) {
                int l = 4 * y[(size_t)j * w + i] - y[(size_t)j * w + i - 1] - y[(size_t)j * w + i + 1] - y[(size_t)(j - 1) * w + i] - y[(size_t)(j + 1) * w + i];
                sy += y[(size_t)j * w + i]; lap += l < 0 ? -l : l; n++;
            }
        for (j = 0; j < h / 2; j++)
            for (i = 0; i < w / 2; i++) {
                int u = uv[(size_t)j * w + 2 * i], v = uv[(size_t)j * w + 2 * i + 1];
                su += u; sv += v; sc += (abs(u - 128) + abs(v - 128)) / 2.0;
            }
        s.y += sy / n; s.h += lap / n; s.u += su / ((double)w * h / 4); s.v += sv / ((double)w * h / 4); s.c += sc / ((double)w * h / 4);
        s.ok++;
        IMP_FrameSource_ReleaseFrame(1, fr);
    }
    if (s.ok) { s.y /= s.ok; s.u /= s.ok; s.v /= s.ok; s.c /= s.ok; s.h /= s.ok; }
    return s;
}

/* frames still flowing (two frames with increasing time stamps) */
static int flow(void)
{
    int w, h;
    int64_t t = 0, t2 = 0;

    return fs_peek(1, &w, &h, &t) == 0 && fs_peek(1, &w, &h, &t2) == 0 && t2 > t;
}

static double dabs(double a) { return a < 0 ? -a : a; }

/* wait (up to 8 s) until the mean luma is back near 'y0'; returns the last measurement, *ms = time it took (-1 = never) */
static Fst recover(double y0, int *ms)
{
    Fst s;
    int t;

    for (t = 0; t < 8; t++) {
        s = fst(t ? 1000 : 600);
        if (s.ok && dabs(s.y - y0) < 0.06 * y0 + 2) { *ms = t ? 600 + t * 1000 : 600; return s; }
    }
    *ms = -1;
    return s;
}

/* verdict line: PASS (effect measured) or PASS "rt:" (call + readback only) */
#define RTMSG(eff) ((eff) ? "" : "rt: ")

static void *dsym(const char *n) { return dlsym(RTLD_DEFAULT, n); }

#if !defined(PLATFORM_T41)
# define NOT_T41(...) __VA_ARGS__
#else
# define NOT_T41(...)
#endif

/* --------------------------------------------------------------------------------------------------- Movestate */
#if HAS_IMP_ISP_Tuning_EnableMovestate && HAS_IMP_ISP_Tuning_DisableMovestate
static void cs_movestate(void)
{
    int r1, r2, f1, f2, f3;

    NEED(IMP_ISP_Tuning_EnableMovestate) NEED(IMP_ISP_Tuning_DisableMovestate) {
        f1 = flow();
        r1 = IMP_ISP_Tuning_EnableMovestate(); usleep(700000); f2 = flow();
        r2 = IMP_ISP_Tuning_DisableMovestate(); usleep(700000); f3 = flow();
        rep(FN(IMP_ISP_Tuning_EnableMovestate), r1, (r1 == 0 && f1 && f2) ? V_PASS : V_FAIL, "rt: enabled, frames flow before/after (%d/%d); ISP state is internal", f1, f2);
        rep(FN(IMP_ISP_Tuning_DisableMovestate), r2, (r2 == 0 && f3) ? V_PASS : V_FAIL, "rt: disabled again, frames flow (%d)", f3);
    }
}
#else
static void cs_movestate(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- noise */
#if HAS_IMP_ISP_Tuning_GetSinterDnsAttr && HAS_IMP_ISP_Tuning_SetSinterDnsAttr
static void cs_sinter(void)
{
    NEED(IMP_ISP_Tuning_GetSinterDnsAttr) NEED(IMP_ISP_Tuning_SetSinterDnsAttr) {
        G(IMPISPSinterDenoiseAttr, o); G(IMPISPSinterDenoiseAttr, a); G(IMPISPSinterDenoiseAttr, b);
        int r1 = IMP_ISP_Tuning_GetSinterDnsAttr(o), r2, r3, r4;
        Fst lo, hi;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetSinterDnsAttr), r1, r1 == 0 ? V_PASS : V_FAIL, "enable %d type %d strength %u", (int)o->enable, (int)o->type, o->sinter_strength);
        *a = *o;
        r2 = IMP_ISP_Tuning_SetSinterDnsAttr(a); r3 = IMP_ISP_Tuning_GetSinterDnsAttr(b); gchk(b);
        rep(FN(IMP_ISP_Tuning_SetSinterDnsAttr), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o))) ? V_PASS : V_FAIL, "same value written (set ret %d), read back %s", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        /* effect: manual strength 0 vs 255 (restored to the original attr afterwards) */
        a->enable = IMPISP_TUNING_OPS_MODE_ENABLE; a->type = IMPISP_TUNING_OPS_TYPE_MANUAL; a->sinter_strength = 255;
        IMP_ISP_Tuning_SetSinterDnsAttr(a); hi = fst(1200);
        a->sinter_strength = 0;
        r4 = IMP_ISP_Tuning_SetSinterDnsAttr(a); lo = fst(1200);
        IMP_ISP_Tuning_SetSinterDnsAttr(o);
        if (hi.ok && lo.ok) {
            int eff = lo.h > hi.h * 1.03 + 0.05;
            rep(LBL("IMP_ISP_Tuning_SetSinterDnsAttr(manual 0/255)"), r4, (r4 == 0 && lo.y > 5 && hi.y > 5) ? V_PASS : V_FAIL, "%sdetail H %.3f at 0 vs %.3f at 255, Y %.1f/%.1f", RTMSG(eff), lo.h, hi.h, lo.y, hi.y);
        }
        gfree(o); gfree(a); gfree(b);
    }
}
#else
static void cs_sinter(void) {}
#endif

#if HAS_IMP_ISP_Tuning_GetTemperDnsAttr && HAS_IMP_ISP_Tuning_SetTemperDnsAttr
static void cs_temper(void)
{
    NEED(IMP_ISP_Tuning_GetTemperDnsAttr) NEED(IMP_ISP_Tuning_SetTemperDnsAttr) {
        G(IMPISPTemperDenoiseAttr, o); G(IMPISPTemperDenoiseAttr, a); G(IMPISPTemperDenoiseAttr, b);
        int r1 = IMP_ISP_Tuning_GetTemperDnsAttr(o), r2, r3, r4;
        int (*ctl)(IMPISPTemperDenoiseAttr *) = (int (*)(IMPISPTemperDenoiseAttr *))dsym("IMP_ISP_Tuning_SetTemperDnsCtl");
        Fst lo, hi;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetTemperDnsAttr), r1, r1 == 0 ? V_PASS : V_FAIL, "type %d strength %u", (int)o->type, o->temper_strength);
        *a = *o;
        r2 = IMP_ISP_Tuning_SetTemperDnsAttr(a); r3 = IMP_ISP_Tuning_GetTemperDnsAttr(b); gchk(b);
        rep(FN(IMP_ISP_Tuning_SetTemperDnsAttr), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o))) ? V_PASS : V_FAIL, "same value written (set ret %d), read back %s", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        if (ctl) {
            int rc = ctl(a);    /* same mode as now */
            usleep(500000);
            rep(LBL("IMP_ISP_Tuning_SetTemperDnsCtl"), rc, (rc == 0 && flow()) ? V_PASS : V_FAIL, "rt: current type %d written, frames flow", (int)a->type);
        } else
            rep_na(LBL("IMP_ISP_Tuning_SetTemperDnsCtl"));
        /* effect: manual strength 0 vs 255.  The module is never disabled (the memory is released for good). */
        a->type = IMPISP_TEMPER_MANUAL; a->temper_strength = 255;
        IMP_ISP_Tuning_SetTemperDnsAttr(a); hi = fst(1200);
        a->temper_strength = 0;
        r4 = IMP_ISP_Tuning_SetTemperDnsAttr(a); lo = fst(1200);
        IMP_ISP_Tuning_SetTemperDnsAttr(o);
        if (hi.ok && lo.ok) {
            int eff = lo.h > hi.h * 1.03 + 0.05;
            rep(LBL("IMP_ISP_Tuning_SetTemperDnsAttr(manual 0/255)"), r4, (r4 == 0 && lo.y > 5 && hi.y > 5) ? V_PASS : V_FAIL, "%sdetail H %.3f at 0 vs %.3f at 255, Y %.1f/%.1f", RTMSG(eff), lo.h, hi.h, lo.y, hi.y);
        }
        gfree(o); gfree(a); gfree(b);
    }
}
#else
static void cs_temper(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- FW freeze / process */
#if HAS_IMP_ISP_Tuning_SetFWFreeze
static void cs_fwfreeze(void)
{
    NEED(IMP_ISP_Tuning_SetFWFreeze) {
        Fst a, b;
        int r1, r2, f1, f2;

        a = fst(500);
        r1 = IMP_ISP_Tuning_SetFWFreeze(IMPISP_TUNING_OPS_MODE_ENABLE); usleep(800000); f1 = flow(); b = fst(500);
        r2 = IMP_ISP_Tuning_SetFWFreeze(IMPISP_TUNING_OPS_MODE_DISABLE); usleep(500000); f2 = flow();
        rep(FN(IMP_ISP_Tuning_SetFWFreeze), r1 | r2, (r1 == 0 && r2 == 0 && f1 && f2 && b.ok && dabs(b.y - a.y) < 15) ? V_PASS : V_FAIL,
            "rt: freeze on/off, frames flow (%d/%d), Y %.1f -> %.1f frozen", f1, f2, a.y, b.y);
    }
}
#else
static void cs_fwfreeze(void) {}
#endif

static void cs_process(void)
{
    int (*proc)(int) = (int (*)(int))dsym("IMP_ISP_Tuning_SetISPProcess");

    if (!proc) { rep_na(LBL("IMP_ISP_Tuning_SetISPProcess")); return; }
    {   /* The driver treats value ENABLE (1) as normal ISP processing and DISABLE (0) as ISP bypass (tx-isp-core-tuning.c
         * apical_isp_bypass_s_control; the header text says the opposite).  DISABLE wedged the T10 pipeline until reboot
         * (agg47 bisect), so only ENABLE = the normal state = same value as now is written. */
        int bypass = getenv("APITEST_PROCESS_BYPASS") != NULL;      /* QEMU only: the value that wedged the T10 camera */
        int r = proc(bypass ? IMPISP_TUNING_OPS_MODE_DISABLE : IMPISP_TUNING_OPS_MODE_ENABLE), f;
        usleep(500000); f = flow();
        rep(LBL("IMP_ISP_Tuning_SetISPProcess"), r, (r == 0 && f) ? V_PASS : V_FAIL, "rt: ENABLE (= normal processing, the driver's state) written, frames flow (%d); DISABLE (= ISP bypass) not run: wedges the pipeline", f);
    }
}

/* --------------------------------------------------------------------------------------------------- WaitFrame / VideoDrop */
#if HAS_IMP_ISP_Tuning_WaitFrameDone
static void cs_waitframe(void)
{
    NEED(IMP_ISP_Tuning_WaitFrameDone) {
        G(IMPISPWaitFrameAttr, a); G(IMPISPWaitFrameAttr, b);
        int r1, r2;

        a->timeout = 1000; b->timeout = 1000;
        r1 = IMP_ISP_Tuning_WaitFrameDone(V0 a); r2 = IMP_ISP_Tuning_WaitFrameDone(V0 b); gchk(a); gchk(b);
        rep(FN(IMP_ISP_Tuning_WaitFrameDone), r1 | r2, (r1 == 0 && r2 == 0 && b->cnt > a->cnt) ? V_PASS : V_FAIL, "two waits ret %d/%d, frame count %llu -> %llu", r1, r2, (unsigned long long)a->cnt, (unsigned long long)b->cnt);
        gfree(a); gfree(b);
    }
}
#elif HAS_IMP_ISP_Tuning_WaitFrame
static void cs_waitframe(void)
{
    NEED(IMP_ISP_Tuning_WaitFrame) {
        G(IMPISPWaitFrameAttr, a); G(IMPISPWaitFrameAttr, b);
        int r1, r2;

        a->timeout = 1000; b->timeout = 1000;
        r1 = IMP_ISP_Tuning_WaitFrame(V0 a); r2 = IMP_ISP_Tuning_WaitFrame(V0 b); gchk(a); gchk(b);
        rep(FN(IMP_ISP_Tuning_WaitFrame), r1 | r2, (r1 == 0 && r2 == 0 && b->cnt > a->cnt) ? V_PASS : V_FAIL, "two waits ret %d/%d, frame count %llu -> %llu", r1, r2, (unsigned long long)a->cnt, (unsigned long long)b->cnt);
        gfree(a); gfree(b);
    }
}
#else
static void cs_waitframe(void) {}
#endif

#if HAS_IMP_ISP_Tuning_SetVideoDrop
static int g_vd_hits;
static void vd_cb(void) { g_vd_hits++; }
static void cs_videodrop(void)
{
    NEED(IMP_ISP_Tuning_SetVideoDrop) {
        int r = IMP_ISP_Tuning_SetVideoDrop(vd_cb), f;

        usleep(1500000); f = flow();
        rep(FN(IMP_ISP_Tuning_SetVideoDrop), r, (r == 0 && f && g_vd_hits == 0) ? V_PASS : V_FAIL, "rt: callback registered, frames flow (%d), callback fired %d times while frames flow (must be 0); a real sensor-link loss is not simulated", f, g_vd_hits);
        IMP_ISP_Tuning_SetVideoDrop(NULL);
    }
}
#else
static void cs_videodrop(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- Log option */
#if HAS_IMP_Log_Get_Option && HAS_IMP_Log_Set_Option
extern int IMP_Log_Get_Option(void) __attribute__((weak));
extern void IMP_Log_Set_Option(int) __attribute__((weak));
static void cs_log(void)
{
    NEED(IMP_Log_Get_Option) NEED(IMP_Log_Set_Option) {
        int o = IMP_Log_Get_Option(), o2;

        IMP_Log_Set_Option(o); o2 = IMP_Log_Get_Option();
        rep(FN(IMP_Log_Get_Option), 0, V_PASS, "option 0x%x", o);
        rep(FN(IMP_Log_Set_Option), 0, o2 == o ? V_PASS : V_FAIL, "same value written, read back 0x%x", o2);
    }
}
#else
static void cs_log(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- Cwf shift */
#if HAS_IMP_ISP_Tuning_Awb_GetCwfShift && HAS_IMP_ISP_Tuning_Awb_SetCwfShift
static void cs_cwf(void)
{
    NEED(IMP_ISP_Tuning_Awb_GetCwfShift) NEED(IMP_ISP_Tuning_Awb_SetCwfShift) {
        G(IMPISPWB, o); G(IMPISPWB, b);
        int r1 = IMP_ISP_Tuning_Awb_GetCwfShift(o), r2, r3;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_Awb_GetCwfShift), r1, r1 == 0 ? V_PASS : V_FAIL, "mode %d r %u b %u", (int)o->mode, o->rgain, o->bgain);
        r2 = IMP_ISP_Tuning_Awb_SetCwfShift(o); r3 = IMP_ISP_Tuning_Awb_GetCwfShift(b); gchk(b);
        rep(FN(IMP_ISP_Tuning_Awb_SetCwfShift), r2, (r2 == 0 && r3 == 0 && b->rgain == o->rgain && b->bgain == o->bgain && b->mode == o->mode) ? V_PASS : V_FAIL,
            "rt: same value written (set ret %d), read back mode %d r %u b %u", r2, (int)b->mode, b->rgain, b->bgain);
        gfree(o); gfree(b);
    }
}
#else
static void cs_cwf(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- SensorRegister */
static int sreg_get(uint32_t reg, uint32_t *v)
{
#if defined(APITEST_T40)
    uint32_t r = reg; return IMP_ISP_GetSensorRegister ? IMP_ISP_GetSensorRegister(IMPVI_MAIN, &r, v) : -99;
#elif defined(PLATFORM_T41)
    IMPISPSensorRegister s; int r; if (!IMP_ISP_GetSensorRegister) return -99;
    s.addr = reg; s.value = 0; r = IMP_ISP_GetSensorRegister(IMPVI_MAIN, &s); *v = s.value; return r;
#else
    return IMP_ISP_GetSensorRegister ? IMP_ISP_GetSensorRegister(reg, v) : -99;
#endif
}
static int sreg_set(uint32_t reg, uint32_t v)
{
#if defined(APITEST_T40)
    uint32_t r = reg, x = v; return IMP_ISP_SetSensorRegister ? IMP_ISP_SetSensorRegister(IMPVI_MAIN, &r, &x) : -99;
#elif defined(PLATFORM_T41)
    IMPISPSensorRegister s; if (!IMP_ISP_SetSensorRegister) return -99;
    s.addr = reg; s.value = v; return IMP_ISP_SetSensorRegister(IMPVI_MAIN, &s);
#else
    return IMP_ISP_SetSensorRegister ? IMP_ISP_SetSensorRegister(reg, v) : -99;
#endif
}

/* chip id registers of the fleet sensors (sensor drivers of the SDK): name, id reg high/low, expected high/low */
static const struct { const char *n; uint32_t rh, rl, vh, vl; } sids[] = {
    { "jxh42", 0x0a, 0x0b, 0xa0, 0x42 }, { "jxf22", 0x0a, 0x0b, 0x0f, 0x22 }, { "jxf23", 0x0a, 0x0b, 0x0f, 0x23 },
    { "sc1a4t", 0x3107, 0x3108, 0xda, 0x4d }, { "sc2336", 0x3107, 0x3108, 0xcb, 0x3a }, { "sc4336p", 0x3107, 0x3108, 0x9c, 0x42 },
    { "sc5336", 0x3107, 0x3108, 0xce, 0x50 }, { "sc830ai", 0x3107, 0x3108, 0xc1, 0x43 }, { "gc5603", 0x03f0, 0x03f1, 0x56, 0x03 },
};

static void cs_sensorreg(void)
{
    unsigned i;
    uint32_t h = 0, l = 0, h2 = 0;
    int r1, r2, r3, r4;

    for (i = 0; i < sizeof(sids) / sizeof(sids[0]); i++)
        if (!strcmp(sids[i].n, g_sensor.name)) break;
    if (i == sizeof(sids) / sizeof(sids[0])) { rep(LBL("IMP_ISP_GetSensorRegister"), 0, V_SKIP, "no chip id known for sensor %s", g_sensor.name); return; }
    NEED(IMP_ISP_GetSensorRegister) {
        r1 = sreg_get(sids[i].rh, &h); r2 = sreg_get(sids[i].rl, &l);
        rep(FN(IMP_ISP_GetSensorRegister), r1 | r2, (r1 == 0 && r2 == 0 && h == sids[i].vh && l == sids[i].vl) ? V_PASS : V_FAIL,
            "chip id regs 0x%x/0x%x = 0x%02x%02x (%s expects 0x%02x%02x)", sids[i].rh, sids[i].rl, h, l, sids[i].n, sids[i].vh, sids[i].vl);
        NEED(IMP_ISP_SetSensorRegister) {
            /* the chip id register is read-only: writing back the value just read changes nothing, and a read after it must agree */
            r3 = sreg_set(sids[i].rh, h); r4 = sreg_get(sids[i].rh, &h2);
            rep(FN(IMP_ISP_SetSensorRegister), r3, (r3 == 0 && r4 == 0 && h2 == h && flow()) ? V_PASS : V_FAIL,
                "rt: same value 0x%02x written to the read-only id reg 0x%x, read back 0x%02x, frames flow", h, sids[i].rh, h2);
        }
    }
}

/* --------------------------------------------------------------------------------------------------- picture-effect cases */
#if HAS_IMP_ISP_Tuning_SetHiLightDepress
static void cs_hilight(void)
{
    NEED(IMP_ISP_Tuning_SetHiLightDepress) {
        static const unsigned vals[] = { 3, 6, 10 };    /* vendor range [0..10], 0 = off (255 blacks the picture: out of range) */
        /* the T10 vendor header has no getter; OpenIMP and later headers export it */
        int (*get)(uint32_t *) = (int (*)(uint32_t *))dsym("IMP_ISP_Tuning_GetHiLightDepress");
        uint32_t orig = 0, cur = 0;
        Fst base, s[3], back;
        int i, rs = 0, rb_ok = 1, dead = 0, eff = 0, ms;
        char buf[160] = "";

        if (get) get(&orig);
        base = fst(800);
        for (i = 0; i < 3; i++) {
            rs |= IMP_ISP_Tuning_SetHiLightDepress(vals[i]);
            if (get) { cur = 0xffff; get(&cur); if (cur != vals[i]) rb_ok = 0; }
            s[i] = fst(1500);
            if (!s[i].ok || s[i].y < 3) dead = 1;
            if (dabs(s[i].y - base.y) > 2.0 || dabs(s[i].h - base.h) > 0.25) eff = 1;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%u:Y%.1f/H%.2f", i ? " " : "", vals[i], s[i].y, s[i].h);
        }
        rs |= IMP_ISP_Tuning_SetHiLightDepress(orig); back = recover(base.y, &ms);
        {
            int rec = ms >= 0;
            rep(FN(IMP_ISP_Tuning_SetHiLightDepress), rs, (rs == 0 && rb_ok && !dead && rec) ? V_PASS : V_FAIL,
                "%sstrength %s vs base Y%.1f/H%.2f (orig %u): %s; picture %s restored (Y %.1f)", RTMSG(eff), buf, base.y, base.h, orig, dead ? "PICTURE DEAD" : "alive", rec ? "" : "NOT", back.y);
            if (get) rep(LBL("IMP_ISP_Tuning_GetHiLightDepress"), 0, rb_ok ? V_PASS : V_FAIL, "readback equals the value set (3, 6, 10)");
        }
    }
}
#else
static void cs_hilight(void) {}
#endif

#if HAS_IMP_ISP_Tuning_SetIntegrationTime && HAS_IMP_ISP_Tuning_GetIntegrationTime
static void cs_inttime(void)
{
    NEED(IMP_ISP_Tuning_SetIntegrationTime) NEED(IMP_ISP_Tuning_GetIntegrationTime) {
        G(IMPISPITAttr, o); G(IMPISPITAttr, a); G(IMPISPITAttr, c);
        int r0 = IMP_ISP_Tuning_GetIntegrationTime(o), rl, rh, rr, rbk = 0, via_expr = 0, eff = 0;
        Fst base, lo, hi, back;
        uint32_t oag = 0, odg = 0;
        int caps = 0;
        char how[80];

        gchk(o);
        base = fst(800);
        /* with a manual time the AE still moves the gain to hold the brightness: pin the gain caps to 1 first (as the OpenIMP test-pattern
         * hold does), restored at the end */
#if HAS_IMP_ISP_Tuning_GetMaxAgain && HAS_IMP_ISP_Tuning_SetMaxAgain && HAS_IMP_ISP_Tuning_GetMaxDgain && HAS_IMP_ISP_Tuning_SetMaxDgain
        if (IMP_ISP_Tuning_GetMaxAgain(&oag) == 0 && IMP_ISP_Tuning_GetMaxDgain(&odg) == 0) {
            IMP_ISP_Tuning_SetMaxAgain(1); IMP_ISP_Tuning_SetMaxDgain(1); caps = 1;
        }
#endif
        /* 1: IntegrationTime manual, short (30 lines) vs long (240 lines; T20/T21 reject >= 256) */
        a->mode = IMPISP_TUNING_MODE_MANUAL; a->integration_time = 30; a->max_integration_time = o->max_integration_time;
        rl = IMP_ISP_Tuning_SetIntegrationTime(a); IMP_ISP_Tuning_GetIntegrationTime(c); gchk(c); rbk = (c->mode == IMPISP_TUNING_MODE_MANUAL && c->integration_time == 30);
        lo = fst(1500);
        a->integration_time = 240;
        rh = IMP_ISP_Tuning_SetIntegrationTime(a); hi = fst(1500);
        eff = lo.ok && hi.ok && hi.y > lo.y + 8;
        snprintf(how, sizeof(how), "IntegrationTime alone");
#if HAS_IMP_ISP_Tuning_SetExpr
        if (!eff && IMP_ISP_Tuning_SetExpr) {
            /* the AE may override it: take the AE out of the loop with the manual exposure (SetExpr) first, then set the time;
             * third try: additionally freeze the ISP firmware (SetFWFreeze) so no auto algorithm runs */
            G(IMPISPExpr, e); G(IMPISPExpr, g);
            int pass, rbt = 0;
            for (pass = 0; pass < 2 && !eff; pass++) {
                if (pass == 1 && !IMP_ISP_Tuning_SetFWFreeze) break;
                if (pass == 1) IMP_ISP_Tuning_SetFWFreeze(IMPISP_TUNING_OPS_MODE_ENABLE);
                e->s_attr.mode = ISP_CORE_EXPR_MODE_MANUAL; e->s_attr.unit = ISP_CORE_EXPR_UNIT_LINE; e->s_attr.time = 30;
                IMP_ISP_Tuning_SetExpr(e); lo = fst(1500);
                if (IMP_ISP_Tuning_GetExpr) { IMP_ISP_Tuning_GetExpr(g); rbt = g->g_attr.integration_time; }
                e->s_attr.time = 240; IMP_ISP_Tuning_SetExpr(e); hi = fst(1500);
                eff = lo.ok && hi.ok && hi.y > lo.y + 8; via_expr = 1;
                snprintf(how, sizeof(how), "SetExpr manual%s (AE out of the loop; GetExpr after 30 lines: %d)", pass ? " + FWFreeze" : "", rbt);
                e->s_attr.mode = ISP_CORE_EXPR_MODE_AUTO; IMP_ISP_Tuning_SetExpr(e);
                if (pass == 1) IMP_ISP_Tuning_SetFWFreeze(IMPISP_TUNING_OPS_MODE_DISABLE);
            }
            gfree(e); gfree(g);
        }
#endif
#if HAS_IMP_ISP_Tuning_SetMaxAgain && HAS_IMP_ISP_Tuning_SetMaxDgain
        if (caps) { IMP_ISP_Tuning_SetMaxAgain(oag); IMP_ISP_Tuning_SetMaxDgain(odg); }
#endif
        (void)caps; (void)oag; (void)odg;
        rr = IMP_ISP_Tuning_SetIntegrationTime(o);          /* back to what it was (AUTO) */
        if (o->mode != IMPISP_TUNING_MODE_MANUAL) { a->mode = IMPISP_TUNING_MODE_AUTO; rr |= IMP_ISP_Tuning_SetIntegrationTime(a); }
        back = fst(2500);
        {
            int rec = back.ok && dabs(back.y - base.y) < 0.25 * base.y + 4;
            rep(FN(IMP_ISP_Tuning_SetIntegrationTime), rl | rh | rr, (rl == 0 && rh == 0 && rr == 0 && rec && eff) ? V_PASS : V_FAIL,
                "%s(gain caps %s) 30 lines Y %.1f -> 240 lines Y %.1f via %s%s; AE %s; picture %s restored (Y %.1f, base %.1f)", "", caps ? "pinned to 1" : "not pinned", lo.y, hi.y, how,
                eff ? "" : " (NO EXPOSURE EFFECT, expected 8x brighter)", via_expr ? (eff ? "overrides IntegrationTime alone" : "overrides IntegrationTime alone, SetExpr manual does not help either") : "follows the set time", rec ? "" : "NOT", back.y, base.y);
            rep(FN(IMP_ISP_Tuning_GetIntegrationTime), r0, (r0 == 0 && rbk) ? V_PASS : V_FAIL, "mode %d it %u max %u; read back manual 30 lines: %s", (int)o->mode, o->integration_time, o->max_integration_time, rbk ? "yes" : "NO");
        }
        gfree(o); gfree(a); gfree(c);
    }
}
#else
static void cs_inttime(void) {}
#endif

#if HAS_IMP_ISP_Tuning_SetSceneMode && HAS_IMP_ISP_Tuning_GetSceneMode
static void cs_scene(void)
{
    NEED(IMP_ISP_Tuning_SetSceneMode) NEED(IMP_ISP_Tuning_GetSceneMode) {
        static const IMPISPSceneMode modes[] = { IMPISP_SCENE_MODE_NIGHT, IMPISP_SCENE_MODE_TEXT, IMPISP_SCENE_MODE_SPORTS };
        IMPISPSceneMode orig = IMPISP_SCENE_MODE_AUTO, cur = IMPISP_SCENE_MODE_AUTO;
        Fst base, s, back;
        int i, rs = 0, rb_ok = 1, eff = 0, rg, rec_ok = 1, ms;
        char buf[260] = "";

        rg = IMP_ISP_Tuning_GetSceneMode(&orig);
        base = fst(800);
        for (i = 0; i < 3; i++) {
            rs |= IMP_ISP_Tuning_SetSceneMode(modes[i]); cur = (IMPISPSceneMode)-1; IMP_ISP_Tuning_GetSceneMode(&cur);
            if (cur != modes[i]) rb_ok = 0;
            s = fst(2000);
            if (s.ok && (dabs(s.y - base.y) > 3 || dabs(s.h - base.h) > 0.3 || dabs(s.c - base.c) > 0.5)) eff = 1;
            rs |= IMP_ISP_Tuning_SetSceneMode(orig); back = recover(base.y, &ms);      /* back to the original mode between the modes */
            if (ms < 0) rec_ok = 0;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%smode %d: Y%.1f H%.2f C%.1f, back to mode %d %s (Y%.1f%s)", i ? "; " : "", (int)modes[i], s.y, s.h, s.c, (int)orig,
                     ms >= 0 ? "ok" : "NOT RECOVERED", back.y, ms >= 0 ? "" : " after 8 s");
        }
        rep(FN(IMP_ISP_Tuning_SetSceneMode), rs, (rs == 0 && rb_ok && rec_ok) ? V_PASS : V_FAIL, "%sreadback %s; base Y%.1f H%.2f C%.1f; %s", RTMSG(eff), rb_ok ? "ok" : "WRONG", base.y, base.h, base.c, buf);
        rep(FN(IMP_ISP_Tuning_GetSceneMode), rg, (rg == 0 && rb_ok) ? V_PASS : V_FAIL, "mode %d, readback of the 3 modes set %s", (int)orig, rb_ok ? "ok" : "WRONG");
    }
}
#else
static void cs_scene(void) {}
#endif

#if HAS_IMP_ISP_Tuning_SetColorfxMode && HAS_IMP_ISP_Tuning_GetColorfxMode
static void cs_colorfx(void)
{
    NEED(IMP_ISP_Tuning_SetColorfxMode) NEED(IMP_ISP_Tuning_GetColorfxMode) {
        static const IMPISPColorfxMode modes[] = { IMPISP_COLORFX_MODE_BW, IMPISP_COLORFX_MODE_NEGATIVE, IMPISP_COLORFX_MODE_VIVID, IMPISP_COLORFX_MODE_SEPIA };
        IMPISPColorfxMode orig = IMPISP_COLORFX_MODE_AUTO, cur;
        Fst base, s[4], back;
        int i, rs = 0, rb_ok = 1, rg;
        int ok_bw, ok_neg, ok_viv, ok_sep, fail_y = 0;

        rg = IMP_ISP_Tuning_GetColorfxMode(&orig);
        base = fst(800);
        for (i = 0; i < 4; i++) {
            rs |= IMP_ISP_Tuning_SetColorfxMode(modes[i]); cur = (IMPISPColorfxMode)-1; IMP_ISP_Tuning_GetColorfxMode(&cur);
            if (cur != modes[i]) rb_ok = 0;
            s[i] = fst(2000);
            rs |= IMP_ISP_Tuning_SetColorfxMode(orig); fst(800);        /* back to the original mode between the modes */
        }
        rs |= IMP_ISP_Tuning_SetColorfxMode(orig); back = fst(2000);
        ok_bw = s[0].ok && s[0].c < 1.0 + base.c * 0.5 && dabs(s[0].u - s[0].v) < 3 && s[0].y > base.y * 0.5;
        if (s[0].ok && s[0].y < base.y * 0.5) fail_y = 1;      /* BW must keep the luma */
        ok_neg = s[1].ok && dabs(s[1].y - (255 - base.y)) < 25 && dabs(s[1].y - base.y) > 5;
        ok_viv = s[2].ok && s[2].c > base.c * 1.25 + 0.3;
        ok_sep = s[3].ok && s[3].v - s[3].u > 15;
        {
            int rec = back.ok && dabs(back.y - base.y) < 0.2 * base.y + 3;
            int eff = ok_bw && ok_neg;      /* the two strong effects prove that the mode reaches the pipeline */
            rep(FN(IMP_ISP_Tuning_SetColorfxMode), rs, (rs == 0 && rb_ok && rec && !fail_y) ? V_PASS : V_FAIL,
                "%sbw %s (C%.1f U%.1f V%.1f Y%.1f%s) negative %s (Y%.1f vs inv. base %.1f) vivid %s (C%.1f vs %.1f) sepia %s (V-U %.1f); base Y%.1f C%.1f; restored %s",
                RTMSG(eff), ok_bw ? "ok" : "no", s[0].c, s[0].u, s[0].v, s[0].y, fail_y ? " LUMA LOST" : "", ok_neg ? "ok" : "no", s[1].y, 255 - base.y,
                ok_viv ? "ok" : "no", s[2].c, base.c, ok_sep ? "ok" : "no", s[3].v - s[3].u, base.y, base.c, rec ? "yes" : "NO");
            rep(FN(IMP_ISP_Tuning_GetColorfxMode), rg, (rg == 0 && rb_ok) ? V_PASS : V_FAIL, "mode %d, readback of the 4 modes set %s", (int)orig, rb_ok ? "ok" : "WRONG");
        }
    }
}
#else
static void cs_colorfx(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- driver */
/* --------------------------------------------------------------------------------------------------- T23/T31/T40/T41 extras */
#if HAS_IMP_ISP_Tuning_SetAeFreeze
static void cs_aefreeze(void)
{
    NEED(IMP_ISP_Tuning_SetAeFreeze) {
        Fst a, b;
        int r1, r2, f1, f2;

        a = fst(500);
        r1 = IMP_ISP_Tuning_SetAeFreeze(IMPISP_TUNING_OPS_MODE_ENABLE); usleep(800000); f1 = flow(); b = fst(500);
        r2 = IMP_ISP_Tuning_SetAeFreeze(IMPISP_TUNING_OPS_MODE_DISABLE); usleep(500000); f2 = flow();
        rep(FN(IMP_ISP_Tuning_SetAeFreeze), r1 | r2, (r1 == 0 && r2 == 0 && f1 && f2 && b.ok && dabs(b.y - a.y) < 15) ? V_PASS : V_FAIL,
            "rt: AE freeze on/off, frames flow (%d/%d), Y %.1f -> %.1f", f1, f2, a.y, b.y);
    }
}
#else
static void cs_aefreeze(void) {}
#endif

#if HAS_IMP_ISP_Tuning_GetAeHist_Origin
static void cs_aehistorigin(void)
{
    NEED(IMP_ISP_Tuning_GetAeHist_Origin) {
        G(IMPISPAEHistOrigin, p);
        int r = IMP_ISP_Tuning_GetAeHist_Origin(p);

        gchk(p);
        rep(FN(IMP_ISP_Tuning_GetAeHist_Origin), r, (r == 0 && gtouched(p)) ? V_PASS : V_FAIL, "histogram filled: %s", gtouched(p) ? "yes" : "NO (all zero)");
        gfree(p);
    }
}
#else
static void cs_aehistorigin(void) {}
#endif

#if HAS_IMP_ISP_Tuning_GetAutoZoom && HAS_IMP_ISP_Tuning_SetAutoZoom
static void cs_autozoom(void)
{
    NEED(IMP_ISP_Tuning_GetAutoZoom) NEED(IMP_ISP_Tuning_SetAutoZoom) {
        G(IMPISPAutoZoom, o); G(IMPISPAutoZoom, b);
        int r1 = IMP_ISP_Tuning_GetAutoZoom(V0 o), r2, r3;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetAutoZoom), r1, r1 == 0 ? V_PASS : V_FAIL, "read");
        r2 = IMP_ISP_Tuning_SetAutoZoom(V0 o); r3 = IMP_ISP_Tuning_GetAutoZoom(V0 b); gchk(b);
        rep(FN(IMP_ISP_Tuning_SetAutoZoom), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && flow()) ? V_PASS : V_FAIL, "rt: same value written (set ret %d), read back %s", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        gfree(o); gfree(b);
    }
}
#else
static void cs_autozoom(void) {}
#endif

#if defined(PLATFORM_T41) && HAS_IMP_ISP_GetFrameDrop && HAS_IMP_ISP_SetFrameDrop
static void cs_framedrop(void)
{
    NEED(IMP_ISP_GetFrameDrop) NEED(IMP_ISP_SetFrameDrop) {
        G(IMPISPFrameDropAttr, o); G(IMPISPFrameDropAttr, b);
        int r1 = IMP_ISP_GetFrameDrop(IMPVI_MAIN, o), r2, r3;

        gchk(o);
        rep(FN(IMP_ISP_GetFrameDrop), r1, r1 == 0 ? V_PASS : V_FAIL, "read");
        r2 = IMP_ISP_SetFrameDrop(IMPVI_MAIN, o); r3 = IMP_ISP_GetFrameDrop(IMPVI_MAIN, b); gchk(b);
        rep(FN(IMP_ISP_SetFrameDrop), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && flow()) ? V_PASS : V_FAIL, "rt: same value written (set ret %d), read back %s, frames flow", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        gfree(o); gfree(b);
    }
}
#else
static void cs_framedrop(void) {}
#endif

/* APITEST_CASES=a,b,c runs only these cases (names: sensorreg log waitframe videodrop movestate aefreeze aehistorigin autozoom framedrop cwf sinter temper process fwfreeze hilight inttime scene colorfx); default all */
static int cs_on(const char *n)
{
    const char *e = getenv("APITEST_CASES"), *p;
    size_t l = strlen(n);

    if (!e) return 1;
    for (p = e; (p = strstr(p, n)); p += l) if ((p == e || p[-1] == ',') && (p[l] == ',' || !p[l])) return 1;
    return 0;
}

void t_cases(void)
{
    rep_area("cases");
    wdog("cases", 60);
    if (cs_on("sensorreg")) cs_sensorreg();  wdog("cases", 60);
    if (cs_on("log")) cs_log();        wdog("cases", 60);
    if (cs_on("waitframe")) cs_waitframe();  wdog("cases", 60);
    if (cs_on("videodrop")) cs_videodrop();  wdog("cases", 60);
    if (cs_on("movestate")) cs_movestate();  wdog("cases", 60);
    if (cs_on("aefreeze")) cs_aefreeze();   wdog("cases", 60);
    if (cs_on("aehistorigin")) cs_aehistorigin(); wdog("cases", 60);
    if (cs_on("autozoom")) cs_autozoom();   wdog("cases", 60);
    if (cs_on("framedrop")) cs_framedrop(); wdog("cases", 60);
    if (cs_on("cwf")) cs_cwf();        wdog("cases", 60);
    if (cs_on("sinter")) cs_sinter();     wdog("cases", 90);
    if (cs_on("temper")) cs_temper();     wdog("cases", 90);
    if (cs_on("process")) cs_process();    wdog("cases", 60);
    if (cs_on("fwfreeze")) cs_fwfreeze();   wdog("cases", 60);
    if (cs_on("hilight")) cs_hilight();    wdog("cases", 90);
    if (cs_on("inttime")) cs_inttime();    wdog("cases", 90);
    if (cs_on("scene")) cs_scene();      wdog("cases", 90);
    if (cs_on("colorfx")) cs_colorfx();    wdog("cases", 90);
    wdog("cases", 60);
}
