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
#include <pthread.h>
#include "apitest.h"

typedef struct { double y, u, v, c, h, sd; int ok; } Fst;

/* mean Y/U/V, chroma magnitude and detail energy of 2 frames of FS ch1 (after a settle time and 3 dropped frames) */
static Fst fst(int settle_ms)
{
    Fst s = { 0, 0, 0, 0, 0, 0, 0 };
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
        double sy = 0, su = 0, sv = 0, sc = 0, lap = 0, sy2 = 0;

        if (IMP_FrameSource_GetFrame(1, &fr) < 0 || !fr) continue;
        y = (const unsigned char *)(uintptr_t)fr->virAddr; w = fr->width; h = fr->height;
        if (!y || w < 16 || h < 16) { IMP_FrameSource_ReleaseFrame(1, fr); continue; }
        uv = y + (size_t)w * h;
        for (j = 1; j < h - 1; j += 2)
            for (i = 1; i < w - 1; i += 2) {
                int l = 4 * y[(size_t)j * w + i] - y[(size_t)j * w + i - 1] - y[(size_t)j * w + i + 1] - y[(size_t)(j - 1) * w + i] - y[(size_t)(j + 1) * w + i];
                sy += y[(size_t)j * w + i]; sy2 += (double)y[(size_t)j * w + i] * y[(size_t)j * w + i]; lap += l < 0 ? -l : l; n++;
            }
        for (j = 0; j < h / 2; j++)
            for (i = 0; i < w / 2; i++) {
                int u = uv[(size_t)j * w + 2 * i], v = uv[(size_t)j * w + 2 * i + 1];
                su += u; sv += v; sc += (abs(u - 128) + abs(v - 128)) / 2.0;
            }
        s.y += sy / n; s.sd += sqrt(sy2 / n - (sy / n) * (sy / n)); s.h += lap / n; s.u += su / ((double)w * h / 4); s.v += sv / ((double)w * h / 4); s.c += sc / ((double)w * h / 4);
        s.ok++;
        IMP_FrameSource_ReleaseFrame(1, fr);
    }
    if (s.ok) { s.sd /= s.ok; s.y /= s.ok; s.u /= s.ok; s.v /= s.ok; s.c /= s.ok; s.h /= s.ok; }
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

/* AE drift of the scene without any change (dark scenes drift several luma levels in seconds): measured once at the start of the
 * cases (t_cases), in luma levels per second; recover() widens its tolerance by it */
static double g_drift = 0;

static void measure_drift(void)
{
    Fst a = fst(600), b;
    int64_t t0 = now_us();

    usleep(4000000);
    b = fst(0);
    if (a.ok && b.ok) {
        double dt = (now_us() - t0) / 1e6;
        g_drift = dabs(b.y - a.y) / (dt > 1 ? dt : 1);
    }
}

/* wait (up to 20 s) until the mean luma is back near 'y0' (tolerance 6 % + 2 + the measured drift over the time waited, and a
 * stable reading counts when it is inside twice that); returns the last measurement, *ms = time it took (-1 = never) */
static Fst recover(double y0, int *ms)
{
    Fst s, prev;
    int t;
    int64_t t0 = now_us();

    memset(&prev, 0, sizeof(prev));
    for (t = 0; t < 20; t++) {
        double el = (now_us() - t0) / 1e6 + 0.6, tol = 0.06 * y0 + 2 + g_drift * (el + 2);

        s = fst(t ? 1000 : 600);
        if (s.ok && dabs(s.y - y0) < tol) { *ms = t ? 600 + t * 1000 : 600; return s; }
        if (s.ok && prev.ok && dabs(s.y - prev.y) < 1.0 && dabs(s.y - y0) < 2 * tol) { *ms = 600 + t * 1000; return s; }
        prev = s;
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

/* ------------------------------------------------------------------------------------------- SetExpr manual time */
#if HAS_IMP_ISP_Tuning_SetExpr && HAS_IMP_ISP_Tuning_GetExpr && (defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21))
/* SetExpr(MANUAL) in lines and in microseconds must reach the exposure (GetExpr reads the integration time back);
 * T10/T20: the driver took no time in lines and refused microseconds without the sensor's line time (issue 130) */
static void cs_expr(void)
{
    NEED(IMP_ISP_Tuning_SetExpr) NEED(IMP_ISP_Tuning_GetExpr) {
        G(IMPISPExpr, e); G(IMPISPExpr, g);
        int r0 = IMP_ISP_Tuning_GetExpr(g), rl, ru, ra, gl, gu, it_l = -1, it_u = -1, lmax, line_us, want_u, ms;
        Fst base = fst(800), lo, hi, back;

        lmax = g->g_attr.integration_time_max; line_us = g->g_attr.one_line_expr_in_us;
        memset(e, 0, sizeof(*e));
        e->s_attr.mode = ISP_CORE_EXPR_MODE_MANUAL; e->s_attr.unit = ISP_CORE_EXPR_UNIT_LINE; e->s_attr.time = 30;
        rl = IMP_ISP_Tuning_SetExpr(e); lo = fst(1500);
        e->s_attr.time = 240; rl |= IMP_ISP_Tuning_SetExpr(e); hi = fst(1500);
        gl = IMP_ISP_Tuning_GetExpr(g); if (gl == 0) it_l = g->g_attr.integration_time;
        e->s_attr.unit = ISP_CORE_EXPR_UNIT_US; e->s_attr.time = 10000;
        ru = IMP_ISP_Tuning_SetExpr(e); usleep(600000);
        gu = IMP_ISP_Tuning_GetExpr(g); if (gu == 0) it_u = g->g_attr.integration_time;
        e->s_attr.mode = ISP_CORE_EXPR_MODE_AUTO; ra = IMP_ISP_Tuning_SetExpr(e);
        back = recover(base.y, &ms);
        want_u = line_us > 0 ? 10000 / line_us : -1;
        {
            int ok_l = rl == 0 && gl == 0 && (it_l == 240 || (lmax > 0 && lmax < 240 && it_l == lmax));
            /* the reported line time is whole microseconds: allow its rounding (up to 1/line_us relative) */
            int ok_u = ru == 0 && gu == 0 && want_u > 0 && it_u >= want_u * line_us / (line_us + 1) && it_u <= want_u * (line_us + 1) / line_us + 1;
            int eff = lo.ok && hi.ok && hi.y > lo.y * 1.5;

            rep(LBL("IMP_ISP_Tuning_SetExpr(manual lines)"), rl | gl, ok_l && r0 == 0 ? V_PASS : V_FAIL, "%s240 lines -> GetExpr %d (max %d); Y %.1f at 30 lines, %.1f at 240", RTMSG(eff), it_l, lmax, lo.y, hi.y);
            rep(LBL("IMP_ISP_Tuning_SetExpr(manual us)"), ru | gu, ok_u ? V_PASS : V_FAIL, "10000 us -> GetExpr %d lines (line %d us, expected ~%d); AUTO ret %d, picture %s restored (Y %.1f, base %.1f)", it_u, line_us, want_u, ra, ms >= 0 ? "" : "NOT", back.y, base.y);
        }
        gfree(e); gfree(g);
    }
}
#else
static void cs_expr(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- Movestate */
#if HAS_IMP_ISP_Tuning_EnableMovestate && HAS_IMP_ISP_Tuning_DisableMovestate
/* the exposure ceiling the motion block changes (T10/T20/T21/T30: legacy GetIntegrationTime), -1 where not readable */
static int ms_it_max(void)
{
#if HAS_IMP_ISP_Tuning_GetIntegrationTime && !defined(PLATFORM_T31)
    IMPISPITAttr a;

    memset(&a, 0, sizeof(a));
    return IMP_ISP_Tuning_GetIntegrationTime(&a) == 0 ? (int)a.max_integration_time : -1;
#else
    return -1;
#endif
}

static void cs_movestate(void)
{
    int r1, r2, f1, f2, f3, m0, m1, m2;

    NEED(IMP_ISP_Tuning_EnableMovestate) NEED(IMP_ISP_Tuning_DisableMovestate) {
        f1 = flow(); m0 = ms_it_max();
        r1 = IMP_ISP_Tuning_EnableMovestate(); usleep(700000); f2 = flow(); m1 = ms_it_max();
        r2 = IMP_ISP_Tuning_DisableMovestate(); usleep(700000); f3 = flow(); m2 = ms_it_max();
        rep(FN(IMP_ISP_Tuning_EnableMovestate), r1, (r1 == 0 && f1 && f2) ? V_PASS : V_FAIL, "rt: enabled, frames flow before/after (%d/%d); IT max %d -> %d", f1, f2, m0, m1);
        rep(FN(IMP_ISP_Tuning_DisableMovestate), r2, (r2 == 0 && f3) ? V_PASS : V_FAIL, "rt: disabled again, frames flow (%d); IT max back %d", f3, m2);
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
/* T41: MSCA output 0 only delivers frames while a consumer takes them (vendor too; timps stops ch0 without a client), and the frame-done
 * wait counts those frames: a thread takes and releases the frames of ch0 for the duration of the case. */
static volatile int g_wf_stop;
static void *wf_drain(void *arg)
{
    (void)arg;
    while (!g_wf_stop) {
        IMPFrameInfo *fr = NULL;
        if (IMP_FrameSource_GetFrame(0, &fr) == 0 && fr) IMP_FrameSource_ReleaseFrame(0, fr);
        else usleep(20000);
    }
    return NULL;
}
static void cs_waitframe(void)
{
    NEED(IMP_ISP_Tuning_WaitFrameDone) {
        G(IMPISPWaitFrameAttr, a); G(IMPISPWaitFrameAttr, b);
        int r1, r2, drain = 0;
        pthread_t th;

#ifdef PLATFORM_T41
        g_wf_stop = 0;
        if (pthread_create(&th, NULL, wf_drain, NULL) == 0) drain = 1;
#endif
        a->timeout = 1000; b->timeout = 1000;
        r1 = IMP_ISP_Tuning_WaitFrameDone(V0 a); r2 = IMP_ISP_Tuning_WaitFrameDone(V0 b); gchk(a); gchk(b);
        if (drain) { g_wf_stop = 1; pthread_join(th, NULL); }
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
        G(IMPISPAutoZoom, o); G(IMPISPAutoZoom, b); G(IMPISPAutoZoom, c);
        int r1 = IMP_ISP_Tuning_GetAutoZoom(V0 o), r2, r3, r4, r5;
        const int32_t *x, *y;
        size_t w, bad = 0;
        char dd[160] = ""; size_t n = 0;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetAutoZoom), r1, r1 == 0 ? V_PASS : V_FAIL, "read");
        r2 = IMP_ISP_Tuning_SetAutoZoom(V0 o); r3 = IMP_ISP_Tuning_GetAutoZoom(V0 b); gchk(b);
        r4 = IMP_ISP_Tuning_SetAutoZoom(V0 b); r5 = IMP_ISP_Tuning_GetAutoZoom(V0 c); gchk(c);
        /* a disabled output (en != 1) reads back as the full input window after a Set (vendor tisp_s_autozoom_control stores
         * 0,0,in_w,in_h for it); so: the first read-back may normalise disabled outputs only, a second Set/Get must be stable */
        x = (const int32_t *)o; y = (const int32_t *)b;
        for (w = 0; w < sizeof(*o) / 4; w++) if (x[w] != y[w]) {
            size_t field = w / 3, chx = w % 3;       /* en[3] left[3] top[3] width[3] height[3] */
            if (field == 0 || (field >= 1 && x[chx] == 1)) bad++;      /* enable flag changed, or an enabled output's window changed */
            if (n < sizeof(dd) - 30) n += snprintf(dd + n, sizeof(dd) - n, " [%zu] %d->%d", w, x[w], y[w]);
        }
        rep(FN(IMP_ISP_Tuning_SetAutoZoom), r2 | r4, (r2 == 0 && r3 == 0 && r4 == 0 && r5 == 0 && !bad && !memcmp(b, c, sizeof(*b)) && flow()) ? V_PASS : V_FAIL,
            "rt: same value written (set ret %d), read back %s%s; second Set/Get %s", r2, bad ? "DIFFERENT" : "equal (disabled outputs normalised to the full window)", dd, !memcmp(b, c, sizeof(*b)) ? "stable" : "UNSTABLE");
        gfree(o); gfree(b); gfree(c);
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


/* =================================================================================================== RC3 cases (claude/rc3-cases)
 * Streamer-used functions of the RC3 gate that had no case or only a Get: same-value round trips (+ effect where one is expected). */
#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetGammaAttr && HAS_IMP_ISP_Tuning_SetGammaAttr
static void cs_gamma(void)
{
    NEED(IMP_ISP_Tuning_GetGammaAttr) NEED(IMP_ISP_Tuning_SetGammaAttr) {
        G(IMPISPGammaAttr, o); G(IMPISPGammaAttr, b); G(IMPISPGammaAttr, u);
        int r1 = IMP_ISP_Tuning_GetGammaAttr(IMPVI_MAIN, o), r2, r3, r4, k, ms, eff;
        Fst base, steep, lin, back;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetGammaAttr), r1, r1 == 0 ? V_PASS : V_FAIL, "type %d g[0]=%u g[64]=%u g[128]=%u", (int)o->Curve_type, o->gamma[0], o->gamma[64], o->gamma[128]);
        r2 = IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, o); r3 = IMP_ISP_Tuning_GetGammaAttr(IMPVI_MAIN, b); gchk(b);
        base = fst(800);
        /* effect: user curve x^0.3 (bright) vs x^1.0 (linear = dark); the module is not bypassed */
        u->Curve_type = IMP_ISP_GAMMA_CURVE_USER;
        for (k = 0; k < 129; k++) u->gamma[k] = (uint16_t)(pow(k / 128.0, 0.3) * 4095 + 0.5);
        r4 = IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, u); steep = fst(1500);
        for (k = 0; k < 129; k++) u->gamma[k] = (uint16_t)(k * 4095 / 128);
        IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, u); lin = fst(1500);
        IMP_ISP_Tuning_SetGammaAttr(IMPVI_MAIN, o); back = recover(base.y, &ms);
        eff = steep.ok && lin.ok && steep.y > lin.y * 1.15 + 3;
        rep(FN(IMP_ISP_Tuning_SetGammaAttr), r2 | r4, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && ms >= 0 && eff) ? V_PASS : V_FAIL,
            "same value written (set ret %d), read back %s; user curve x^0.3 Y %.1f vs linear Y %.1f (base %.1f): %s; picture %s restored (Y %.1f)", r2,
            !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT", steep.y, lin.y, base.y, eff ? "effect" : "NO PICTURE EFFECT", ms >= 0 ? "" : "NOT", back.y);
        gfree(o); gfree(b); gfree(u);
    }
}
#else
static void cs_gamma(void) {}
#endif

#if HAS_IMP_ISP_Tuning_GetModuleControl && HAS_IMP_ISP_Tuning_SetModuleControl
static void cs_modctl(void)
{
    NEED(IMP_ISP_Tuning_GetModuleControl) NEED(IMP_ISP_Tuning_SetModuleControl) {
        G(IMPISPModuleCtl, o); G(IMPISPModuleCtl, b); G(IMPISPModuleCtl, m); G(IMPISPModuleCtl, c);
        int r1 = IMP_ISP_Tuning_GetModuleControl(V0 o), r2, r3, r4, r5, ms;
        Fst base, off, back;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetModuleControl), r1, r1 == 0 ? V_PASS : V_FAIL, "key 0x%08x", o->key);
        r2 = IMP_ISP_Tuning_SetModuleControl(V0 o); r3 = IMP_ISP_Tuning_GetModuleControl(V0 b); gchk(b);
        base = fst(800);
        /* effect: bypass the gamma module (the picture goes dark: the linear sensor data without the gamma curve), restored afterwards */
        *m = *o; m->bitBypassGAMMA = 1;
        r4 = IMP_ISP_Tuning_SetModuleControl(V0 m); r5 = IMP_ISP_Tuning_GetModuleControl(V0 c); off = fst(1500);
        IMP_ISP_Tuning_SetModuleControl(V0 o); back = recover(base.y, &ms);
        rep(FN(IMP_ISP_Tuning_SetModuleControl), r2 | r4, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && r5 == 0 && c->bitBypassGAMMA && off.ok && off.y < base.y * 0.85 && ms >= 0) ? V_PASS : V_FAIL,
            "same value written (set ret %d), read back %s; gamma bypass bit read back %d, Y %.1f -> %.1f; picture %s restored (Y %.1f)", r2,
            !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT", (int)c->bitBypassGAMMA, base.y, off.y, ms >= 0 ? "" : "NOT", back.y);
        gfree(o); gfree(b); gfree(m); gfree(c);
    }
}
#else
static void cs_modctl(void) {}
#endif

#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetCCMAttr && HAS_IMP_ISP_Tuning_SetCCMAttr
static void cs_ccm(void)
{
    NEED(IMP_ISP_Tuning_GetCCMAttr) NEED(IMP_ISP_Tuning_SetCCMAttr) {
        G(IMPISPCCMAttr, o); G(IMPISPCCMAttr, b);
        int r1 = IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, o), r2, r3;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetCCMAttr), r1, r1 == 0 ? V_PASS : V_FAIL, "manual %d sat %d m0 %.3f m4 %.3f m8 %.3f", (int)o->ManualEn, (int)o->SatEn, o->ColorMatrix[0], o->ColorMatrix[4], o->ColorMatrix[8]);
        r2 = IMP_ISP_Tuning_SetCCMAttr(IMPVI_MAIN, o); usleep(700000); r3 = IMP_ISP_Tuning_GetCCMAttr(IMPVI_MAIN, b); gchk(b);
        rep(FN(IMP_ISP_Tuning_SetCCMAttr), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && flow()) ? V_PASS : V_FAIL,
            "rt: same value written (set ret %d), read back %s, frames flow (colour effect of a manual matrix: see imgfx ccm-*, issue 105)", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        gfree(o); gfree(b);
    }
}
#else
static void cs_ccm(void) {}
#endif

#ifdef PLATFORM_T41
# define RGBC IMPISPCoefftWb
#else
# define RGBC IMPISPCOEFFTWB
#endif
#if HAS_IMP_ISP_Tuning_Awb_GetRgbCoefft && HAS_IMP_ISP_Tuning_Awb_SetRgbCoefft
static void cs_rgbcoef(void)
{
    NEED(IMP_ISP_Tuning_Awb_GetRgbCoefft) NEED(IMP_ISP_Tuning_Awb_SetRgbCoefft) {
        G(RGBC, o); G(RGBC, b); G(RGBC, a);
        int r1 = IMP_ISP_Tuning_Awb_GetRgbCoefft(V0 o), r2, r3, r4, ms;
        Fst base, x, back;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_Awb_GetRgbCoefft), r1, r1 == 0 ? V_PASS : V_FAIL, "r %u g %u b %u", o->rgb_coefft_wb_r, o->rgb_coefft_wb_g, o->rgb_coefft_wb_b);
        r2 = IMP_ISP_Tuning_Awb_SetRgbCoefft(V0 o); r3 = IMP_ISP_Tuning_Awb_GetRgbCoefft(V0 b); gchk(b);
        base = fst(800);
        *a = *o; a->rgb_coefft_wb_r = (unsigned short)(o->rgb_coefft_wb_r * 3 / 2);
        r4 = IMP_ISP_Tuning_Awb_SetRgbCoefft(V0 a); x = fst(1000);
        IMP_ISP_Tuning_Awb_SetRgbCoefft(V0 o); back = recover(base.y, &ms);
        {
            int eff = x.ok && base.ok && dabs(x.v - base.v) > 1.5;
            rep(FN(IMP_ISP_Tuning_Awb_SetRgbCoefft), r2 | r4, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%ssame value written (set ret %d), read back %s; R x1.5: V %.2f -> %.2f (AWB may pull it back); picture %s restored (Y %.1f)", RTMSG(eff), r2,
                !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT", base.v, x.v, ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(o); gfree(b); gfree(a);
    }
}
#else
static void cs_rgbcoef(void) {}
#endif

#if defined(PLATFORM_T41) && HAS_IMP_ISP_GetISPBypass && HAS_IMP_ISP_SetISPBypass
static void cs_ispbypass(void)
{
    NEED(IMP_ISP_GetISPBypass) NEED(IMP_ISP_SetISPBypass) {
        IMPISPTuningOpsMode o = IMPISP_TUNING_OPS_MODE_DISABLE, b = IMPISP_TUNING_OPS_MODE_ENABLE;
        int r1 = IMP_ISP_GetISPBypass(IMPVI_MAIN, &o), r2, r3;

        rep(FN(IMP_ISP_GetISPBypass), r1, r1 == 0 ? V_PASS : V_FAIL, "bypass %d", (int)o);
        r2 = IMP_ISP_SetISPBypass(IMPVI_MAIN, &o); usleep(700000); r3 = IMP_ISP_GetISPBypass(IMPVI_MAIN, &b);
        rep(FN(IMP_ISP_SetISPBypass), r2, (r2 == 0 && r3 == 0 && b == o && flow()) ? V_PASS : V_FAIL, "rt: same value (%d) written (set ret %d), read back %d, frames flow", (int)o, r2, (int)b);
    }
}
#else
static void cs_ispbypass(void) {}
#endif

/* IMP_ISP_Set/GetDefaultBinPath: the vendor requires the call BEFORE IMP_ISP_AddSensor (attention note of the header); called from main()
 * between ISP_Open and AddSensor.  Writes the bin file the sensor loads by default (only if it exists), reads it back. */
#if defined(PLATFORM_T41) && HAS_IMP_ISP_GetDefaultBinPath && HAS_IMP_ISP_SetDefaultBinPath
void t_binpath_pre(void)
{
    const char *want = getenv("APITEST_CASES");

    if (!want || !strstr(want, "binpath")) return;      /* opt-in: with the current driver AddSensor fails afterwards (issue 116) */
    NEED(IMP_ISP_GetDefaultBinPath) NEED(IMP_ISP_SetDefaultBinPath) {
        char *o = (char *)gnew(128), *b = (char *)gnew(128), *p = (char *)gnew(128);
        struct stat st;
        int r1 = IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, o), r2 = -99, r3 = -99;

#ifdef APITEST_T40
        snprintf(p, 128, "/etc/sensor/%s-t40.bin", g_sensor.name);
#else
        snprintf(p, 128, "/etc/sensor/%s-t41.bin", g_sensor.name);
#endif
        gchk(o);
        if (stat(p, &st) != 0) {
            rep(FN(IMP_ISP_GetDefaultBinPath), r1, r1 == 0 ? V_PASS : V_NA, "before AddSensor: \"%.100s\" (empty = none stored)", o);
            rep(FN(IMP_ISP_SetDefaultBinPath), 0, V_SKIP, "%s does not exist", p);
        } else {
            r2 = IMP_ISP_SetDefaultBinPath(IMPVI_MAIN, p); r3 = IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, b); gchk(b);
            rep(FN(IMP_ISP_GetDefaultBinPath), r3, (r3 == 0 && !strcmp(p, b)) ? V_PASS : V_FAIL, "before AddSensor and the first Set: ret %d \"%.60s\" (nothing stored); after Set: \"%.100s\"", r1, o, b);
            rep(FN(IMP_ISP_SetDefaultBinPath), r2, (r2 == 0 && r3 == 0 && !strcmp(p, b)) ? V_PASS : V_FAIL, "rt: the sensor's own bin file %s written before AddSensor (ret %d), read back %s", p, r2, !strcmp(p, b) ? "equal" : "DIFFERENT");
        }
        gfree(o); gfree(b); gfree(p);
    }
}
#else
void t_binpath_pre(void) {}
#endif

#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SwitchBin && HAS_IMP_ISP_GetDefaultBinPath
static void cs_switchbin(void)
{
    NEED(IMP_ISP_Tuning_SwitchBin) NEED(IMP_ISP_GetDefaultBinPath) {
        char *p = (char *)gnew(128);
        G(IMPISPBinAttr, a);
        int r1 = IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, p), r2, ms;
        Fst base, back;
        struct stat st;

        if (r1 != 0 || !p[0] || stat(p, &st) != 0) {
            rep(FN(IMP_ISP_Tuning_SwitchBin), r1, V_SKIP, "no usable current bin file (\"%.80s\")", p);
        } else {
            base = fst(800);
            a->enable = IMPISP_TUNING_OPS_MODE_ENABLE; snprintf(a->bname, sizeof(a->bname), "%s", p);
            r2 = IMP_ISP_Tuning_SwitchBin(IMPVI_MAIN, a); back = recover(base.y, &ms);
            rep(FN(IMP_ISP_Tuning_SwitchBin), r2, (r2 == 0 && ms >= 0 && flow()) ? V_PASS : V_FAIL, "rt: the bin file in use (%.60s) switched to itself (ret %d), picture %s same (Y %.1f -> %.1f), frames flow", p, r2, ms >= 0 ? "" : "NOT", base.y, back.y);
        }
        gfree(p); gfree(a);
    }
}
#else
static void cs_switchbin(void) {}
#endif

#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SetScalerLv
static void cs_scalerlv(void)
{
    NEED(IMP_ISP_Tuning_SetScalerLv) {
        G(IMPISPScalerLvAttr, a);
        int r1, r2, r3, ms;
        Fst lo, hi, back, base;

        base = fst(800);
        a->chx = 1; a->mode = IMPISP_SCALER_FITTING_CRUVE; a->level = 0;
        r1 = IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, a); lo = fst(1200);
        a->level = 128;
        r2 = IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, a); hi = fst(1200);
        a->level = 64;      /* no getter: the middle of the 0..128 range */
        r3 = IMP_ISP_Tuning_SetScalerLv(IMPVI_MAIN, a); back = recover(base.y, &ms);
        {
            int eff = lo.ok && hi.ok && dabs(lo.h - hi.h) > 0.05;
            rep(FN(IMP_ISP_Tuning_SetScalerLv), r1 | r2 | r3, (r1 == 0 && r2 == 0 && r3 == 0 && lo.ok && hi.ok && lo.y > 5 && hi.y > 5 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%sch1 level 0/128/64 accepted, detail H %.3f at 0 vs %.3f at 128, Y %.1f/%.1f, picture %s alive (Y %.1f)", RTMSG(eff), lo.h, hi.h, lo.y, hi.y, ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(a);
    }
}
#else
static void cs_scalerlv(void) {}
#endif


/* ---- shape "Set(value) / Get(&value)" pairs: same-value round trip, a changed value read back, restore; optional effect (mode 1 luma std-dev, 2 mean luma, 3 chroma V-U shift) */
#ifdef PLATFORM_T41
# define SETU(f, T, v) ({ T t_ = (v); f(IMPVI_MAIN, &t_); })
#else
# define SETU(f, T, v) f(v)
#endif
static double eff_metric(int mode, const Fst *a, const Fst *b)
{
    switch (mode) {
    case 1: return dabs(a->sd - b->sd) / (a->sd + b->sd + 1) * 2;
    case 2: return dabs(a->y - b->y) / (a->y + b->y + 1) * 2;
    case 3: return dabs((a->v - a->u) - (b->v - b->u)) / 16.0;
    }
    return 0;
}
#define PAIR_CASE(CS, GETF, SETF, T, GETC, SETC, ALT, MODE, LOV, HIV) \
static void CS(void) \
{ \
    NEED(GETF) NEED(SETF) { \
        T o = (T)0, b = (T)0, c = (T)0, alt; \
        int r1 = GETC(o), r2, r3, r4, r5, f, ms, r6 = 0, eff = 0; \
        Fst base, lo, hi, back; \
        rep(FN(GETF), r1, r1 == 0 ? V_PASS : V_FAIL, "value %u", (unsigned)o); \
        base = fst(600); \
        r2 = SETC(o); usleep(500000); r3 = GETC(b); \
        alt = (T)(ALT); r4 = SETC(alt); usleep(800000); r5 = GETC(c); f = flow(); \
        if (MODE) { r6 = SETC((T)(LOV)); lo = fst(1200); r6 |= SETC((T)(HIV)); hi = fst(1200); eff = lo.ok && hi.ok && eff_metric(MODE, &lo, &hi) > 0.03; } \
        SETC(o); back = recover(base.y, &ms); \
        rep(FN(SETF), r2 | r4 | r6, (r2 == 0 && r3 == 0 && b == o && r4 == 0 && r5 == 0 && c == alt && f && flow() && ms >= 0) ? V_PASS : V_FAIL, \
            "%ssame value %u written, read back %u; %u written, read back %u, frames flow %d; restored%s", (MODE && !eff) ? "rt: " : "", (unsigned)o, (unsigned)b, (unsigned)alt, (unsigned)c, f, \
            MODE ? (eff ? ", effect measured" : ", no measurable picture effect in this scene") : ""); \
    } \
}

#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetContrast && HAS_IMP_ISP_Tuning_SetContrast
# define GC_CT(v) IMP_ISP_Tuning_GetContrast(&v)
# define SC_CT(v) IMP_ISP_Tuning_SetContrast(v)
PAIR_CASE(cs_contrast, IMP_ISP_Tuning_GetContrast, IMP_ISP_Tuning_SetContrast, unsigned char, GC_CT, SC_CT, (o >= 128 ? o - 40 : o + 40), 1, 30, 230)
#else
static void cs_contrast(void) {}
#endif
#if HAS_IMP_ISP_Tuning_GetBcshHue && HAS_IMP_ISP_Tuning_SetBcshHue
# ifdef PLATFORM_T41
#  define GC_HUE(v) IMP_ISP_Tuning_GetBcshHue(IMPVI_MAIN, &v)
#  define SC_HUE(v) ({ unsigned char t_ = (v); IMP_ISP_Tuning_SetBcshHue(IMPVI_MAIN, &t_); })
# else
#  define GC_HUE(v) IMP_ISP_Tuning_GetBcshHue(&v)
#  define SC_HUE(v) IMP_ISP_Tuning_SetBcshHue(v)
# endif
PAIR_CASE(cs_hue, IMP_ISP_Tuning_GetBcshHue, IMP_ISP_Tuning_SetBcshHue, unsigned char, GC_HUE, SC_HUE, (o >= 128 ? o - 40 : o + 40), 3, 40, 215)
#else
static void cs_hue(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetAeComp && HAS_IMP_ISP_Tuning_SetAeComp
# define GC_AEC(v) IMP_ISP_Tuning_GetAeComp(&v)
# define SC_AEC(v) IMP_ISP_Tuning_SetAeComp(v)
PAIR_CASE(cs_aecomp, IMP_ISP_Tuning_GetAeComp, IMP_ISP_Tuning_SetAeComp, int, GC_AEC, SC_AEC, (o >= 128 ? o - 20 : o + 20), 2, 80, 180)
#else
static void cs_aecomp(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetBacklightComp && HAS_IMP_ISP_Tuning_SetBacklightComp
# define GC_BLC(v) IMP_ISP_Tuning_GetBacklightComp(&v)
# define SC_BLC(v) IMP_ISP_Tuning_SetBacklightComp(v)
PAIR_CASE(cs_backlight, IMP_ISP_Tuning_GetBacklightComp, IMP_ISP_Tuning_SetBacklightComp, uint32_t, GC_BLC, SC_BLC, (o == 5 ? 4 : 5), 2, 1, 10)
#else
static void cs_backlight(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetDPC_Strength && HAS_IMP_ISP_Tuning_SetDPC_Strength
# define GC_DPC(v) IMP_ISP_Tuning_GetDPC_Strength(&v)
# define SC_DPC(v) IMP_ISP_Tuning_SetDPC_Strength(v)
PAIR_CASE(cs_dpc, IMP_ISP_Tuning_GetDPC_Strength, IMP_ISP_Tuning_SetDPC_Strength, unsigned int, GC_DPC, SC_DPC, (o >= 128 ? o - 30 : o + 30), 0, 0, 0)
#else
static void cs_dpc(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetISPCustomMode && HAS_IMP_ISP_Tuning_SetISPCustomMode
# define GC_CUS(v) IMP_ISP_Tuning_GetISPCustomMode(&v)
# define SC_CUS(v) IMP_ISP_Tuning_SetISPCustomMode(v)
PAIR_CASE(cs_custom, IMP_ISP_Tuning_GetISPCustomMode, IMP_ISP_Tuning_SetISPCustomMode, IMPISPTuningOpsMode, GC_CUS, SC_CUS, o, 0, 0, 0)
#else
static void cs_custom(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_WDR_ENABLE && HAS_IMP_ISP_WDR_ENABLE_Get
# define GC_WDR(v) IMP_ISP_WDR_ENABLE_Get(&v)
# define SC_WDR(v) IMP_ISP_WDR_ENABLE(v)
PAIR_CASE(cs_wdr, IMP_ISP_WDR_ENABLE_Get, IMP_ISP_WDR_ENABLE, IMPISPTuningOpsMode, GC_WDR, SC_WDR, o, 0, 0, 0)
#else
static void cs_wdr(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetWdr_OutputMode && HAS_IMP_ISP_Tuning_SetWdr_OutputMode
static void cs_wdrout(void)
{
    NEED(IMP_ISP_Tuning_GetWdr_OutputMode) NEED(IMP_ISP_Tuning_SetWdr_OutputMode) {
        IMPISPWdrOutputMode o = 0, b = 0;
        int r1 = IMP_ISP_Tuning_GetWdr_OutputMode(&o), r2, r3;

        rep(FN(IMP_ISP_Tuning_GetWdr_OutputMode), r1, r1 == 0 ? V_PASS : V_FAIL, "mode %d", (int)o);
        r2 = IMP_ISP_Tuning_SetWdr_OutputMode(&o); usleep(500000); r3 = IMP_ISP_Tuning_GetWdr_OutputMode(&b);
        rep(FN(IMP_ISP_Tuning_SetWdr_OutputMode), r2, (r2 == 0 && r3 == 0 && b == o && flow()) ? V_PASS : V_FAIL, "rt: same mode %d written (ret %d), read back %d, frames flow", (int)o, r2, (int)b);
    }
}
#else
static void cs_wdrout(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetISPHflip && HAS_IMP_ISP_Tuning_SetISPHflip && HAS_IMP_ISP_Tuning_GetISPVflip && HAS_IMP_ISP_Tuning_SetISPVflip
static void cs_ispflip(void)
{
    NEED(IMP_ISP_Tuning_GetISPHflip) NEED(IMP_ISP_Tuning_SetISPHflip) NEED(IMP_ISP_Tuning_GetISPVflip) NEED(IMP_ISP_Tuning_SetISPVflip) {
        IMPISPTuningOpsMode h = 0, v = 0, h2 = 0, v2 = 0, h3 = 0, v3 = 0;
        int r1 = IMP_ISP_Tuning_GetISPHflip(&h), r2 = IMP_ISP_Tuning_GetISPVflip(&v), r3, r4, r5, r6, r7, r8, f;

        rep(FN(IMP_ISP_Tuning_GetISPHflip), r1, r1 == 0 ? V_PASS : V_FAIL, "hflip %d", (int)h);
        rep(FN(IMP_ISP_Tuning_GetISPVflip), r2, r2 == 0 ? V_PASS : V_FAIL, "vflip %d", (int)v);
        r3 = IMP_ISP_Tuning_SetISPHflip(h); r4 = IMP_ISP_Tuning_SetISPVflip(v); usleep(700000);
        IMP_ISP_Tuning_GetISPHflip(&h2); IMP_ISP_Tuning_GetISPVflip(&v2);
        rep(FN(IMP_ISP_Tuning_SetISPHflip), r3, (r3 == 0 && h2 == h && flow()) ? V_PASS : V_FAIL, "rt: same value %d written (ret %d), read back %d, frames flow", (int)h, r3, (int)h2);
        rep(FN(IMP_ISP_Tuning_SetISPVflip), r4, (r4 == 0 && v2 == v && flow()) ? V_PASS : V_FAIL, "rt: same value %d written (ret %d), read back %d, frames flow", (int)v, r4, (int)v2);
        /* toggled and back (the picture mirrors twice: measured by imgfx isp-hflip/isp-vflip) */
        r5 = IMP_ISP_Tuning_SetISPHflip(!h); r6 = IMP_ISP_Tuning_SetISPVflip(!v); usleep(900000);
        IMP_ISP_Tuning_GetISPHflip(&h3); IMP_ISP_Tuning_GetISPVflip(&v3); f = flow();
        r7 = IMP_ISP_Tuning_SetISPHflip(h); r8 = IMP_ISP_Tuning_SetISPVflip(v); usleep(900000);
        rep(LBL("IMP_ISP_Tuning_Set/GetISPHflip+Vflip(toggle)"), r5 | r6 | r7 | r8, (!(r5 | r6 | r7 | r8) && h3 == !h && v3 == !v && f && flow()) ? V_PASS : V_FAIL,
            "toggled h %d->%d v %d->%d (read back), frames flow, restored", (int)h, (int)h3, (int)v, (int)v3);
    }
}
#else
static void cs_ispflip(void) {}
#endif
#if defined(PLATFORM_T23) && HAS_IMP_ISP_Tuning_GetSensorHflip && HAS_IMP_ISP_Tuning_SetSensorHflip && HAS_IMP_ISP_Tuning_GetSensorVflip && HAS_IMP_ISP_Tuning_SetSensorVflip
/* T23 sensor flip: the vendor API has Get/Set pairs (the picture effect is proven by the picture cases sensor-hflip / sensor-vflip:
 * mirrored frame, left/right and top/bottom halves compared) */
static void cs_sensorflip(void)
{
    NEED(IMP_ISP_Tuning_GetSensorHflip) NEED(IMP_ISP_Tuning_SetSensorHflip) NEED(IMP_ISP_Tuning_GetSensorVflip) NEED(IMP_ISP_Tuning_SetSensorVflip) {
        IMPISPTuningOpsMode h = 0, v = 0, h2 = 0, v2 = 0, h3 = 0, v3 = 0;
        int r1 = IMP_ISP_Tuning_GetSensorHflip(&h), r2 = IMP_ISP_Tuning_GetSensorVflip(&v), r3, r4, r5, r6, r7, r8, f;

        r3 = IMP_ISP_Tuning_SetSensorHflip(h); r4 = IMP_ISP_Tuning_SetSensorVflip(v); usleep(900000);
        IMP_ISP_Tuning_GetSensorHflip(&h2); IMP_ISP_Tuning_GetSensorVflip(&v2);
        r5 = IMP_ISP_Tuning_SetSensorHflip(!h); r6 = IMP_ISP_Tuning_SetSensorVflip(!v); usleep(1200000);
        IMP_ISP_Tuning_GetSensorHflip(&h3); IMP_ISP_Tuning_GetSensorVflip(&v3); f = flow();
        r7 = IMP_ISP_Tuning_SetSensorHflip(h); r8 = IMP_ISP_Tuning_SetSensorVflip(v); usleep(1200000);
        rep(FN(IMP_ISP_Tuning_GetSensorHflip), r1, (r1 == 0 && r3 == 0 && h2 == h && r5 == 0 && h3 == !h && r7 == 0) ? V_PASS : V_FAIL,
            "hflip %d; same value written (ret %d) reads back %d; toggled reads back %d; restored (ret %d)", (int)h, r3, (int)h2, (int)h3, r7);
        rep(FN(IMP_ISP_Tuning_GetSensorVflip), r2, (r2 == 0 && r4 == 0 && v2 == v && r6 == 0 && v3 == !v && r8 == 0) ? V_PASS : V_FAIL,
            "vflip %d; same value written (ret %d) reads back %d; toggled reads back %d; restored (ret %d)", (int)v, r4, (int)v2, (int)v3, r8);
        rep(LBL("IMP_ISP_Tuning_SetSensorHflip+Vflip(toggle)"), r5 | r6 | r7 | r8, (!(r5 | r6 | r7 | r8) && f && flow()) ? V_PASS : V_FAIL, "frames flow while toggled (%d) and after the restore", f);
    }
}
#else
static void cs_sensorflip(void) {}
#endif

#if (defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T10) || defined(PLATFORM_T30)) && HAS_IMP_ISP_Tuning_GetAeStrategy && HAS_IMP_ISP_Tuning_SetAeStrategy
/* AE strategy: split balanced (0) vs integration priority (1).  Round trip + effect: with the priority on integration time the AE keeps the
 * gain lower / the exposure time longer at the same scene brightness; the effect is only visible when the scene is not at the exposure limits */
static void cs_aestrategy(void)
{
    NEED(IMP_ISP_Tuning_GetAeStrategy) NEED(IMP_ISP_Tuning_SetAeStrategy) {
        IMPISPAeStrategy o = 0, a = 0, b = 0, c = 0;
        int r1 = IMP_ISP_Tuning_GetAeStrategy(&o), r2, r3, r4, r5, r6, it0 = -1, it1 = -1;
        Fst s0, s1;

        r2 = IMP_ISP_Tuning_SetAeStrategy(IMPISP_AE_STRATEGY_SPLIT_BALANCED); usleep(2500000);
        r3 = IMP_ISP_Tuning_GetAeStrategy(&a); s0 = fst(300);
#if HAS_IMP_ISP_Tuning_GetIntegrationTime
        { IMPISPITAttr t; memset(&t, 0, sizeof(t)); if (IMP_ISP_Tuning_GetIntegrationTime && IMP_ISP_Tuning_GetIntegrationTime(&t) == 0) it0 = (int)t.integration_time; }
#endif
        r4 = IMP_ISP_Tuning_SetAeStrategy(IMPISP_AE_STRATEGY_SPLIT_INTEGRATION_PRIORITY); usleep(2500000);
        r5 = IMP_ISP_Tuning_GetAeStrategy(&b); s1 = fst(300);
#if HAS_IMP_ISP_Tuning_GetIntegrationTime
        { IMPISPITAttr t; memset(&t, 0, sizeof(t)); if (IMP_ISP_Tuning_GetIntegrationTime && IMP_ISP_Tuning_GetIntegrationTime(&t) == 0) it1 = (int)t.integration_time; }
#endif
        r6 = IMP_ISP_Tuning_SetAeStrategy(o); usleep(1500000); IMP_ISP_Tuning_GetAeStrategy(&c);
        rep(FN(IMP_ISP_Tuning_GetAeStrategy), r1, (r1 == 0 && r3 == 0 && a == IMPISP_AE_STRATEGY_SPLIT_BALANCED && r5 == 0 && b == IMPISP_AE_STRATEGY_SPLIT_INTEGRATION_PRIORITY && c == o) ? V_PASS : V_FAIL,
            "initial %d; set 0 reads %d, set 1 reads %d, restored %d", (int)o, (int)a, (int)b, (int)c);
        rep(FN(IMP_ISP_Tuning_SetAeStrategy), r2 | r4 | r6, (r2 == 0 && r4 == 0 && r6 == 0 && s0.ok && s1.ok && s0.y > 5 && s1.y > 5 && flow()) ? V_PASS : V_FAIL,
            "%sintegration time %d (balanced) -> %d (integration priority), Y %.1f -> %.1f", RTMSG(it1 > it0 && it0 > 0), it0, it1, s0.y, s1.y);
    }
}
#else
static void cs_aestrategy(void) {}
#endif

#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetHVFlip && HAS_IMP_ISP_Tuning_SetHVFLIP
static void cs_hvflip(void)
{
    NEED(IMP_ISP_Tuning_GetHVFlip) NEED(IMP_ISP_Tuning_SetHVFLIP) {
        IMPISPHVFLIP o = 0, b = 0, c = 0;
        int r1 = IMP_ISP_Tuning_GetHVFlip(&o), r2, r3, r4, r5, f;

        rep(FN(IMP_ISP_Tuning_GetHVFlip), r1, r1 == 0 ? V_PASS : V_FAIL, "hvflip %d", (int)o);
        r2 = IMP_ISP_Tuning_SetHVFLIP(o); usleep(700000); r3 = IMP_ISP_Tuning_GetHVFlip(&b);
        r4 = IMP_ISP_Tuning_SetHVFLIP(o == IMPISP_FLIP_NORMAL_MODE ? IMPISP_FLIP_HV_MODE : IMPISP_FLIP_NORMAL_MODE); usleep(1200000); r5 = IMP_ISP_Tuning_GetHVFlip(&c); f = flow();
        IMP_ISP_Tuning_SetHVFLIP(o); usleep(1200000);
        rep(FN(IMP_ISP_Tuning_SetHVFLIP), r2 | r4, (r2 == 0 && r3 == 0 && b == o && r4 == 0 && r5 == 0 && c != o && f && flow()) ? V_PASS : V_FAIL,
            "rt: same mode %d written, read back %d; changed, read back %d, frames flow %d; restored", (int)o, (int)b, (int)c, f);
    }
}
#else
static void cs_hvflip(void) {}
#endif
#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SetISPBypass
static void cs_ispbypass_old(void)
{
    NEED(IMP_ISP_Tuning_SetISPBypass) {
        /* the old API has no getter.  OEM semantics (T31 ISP bypass control 0x8000164):
         * ENABLE (1) = full ISP pipeline, the state the camera is in and what prudynt
         * writes at startup; DISABLE = the real bypass.  The round trip writes ENABLE.
         * DISABLE: the vendor T21 library returns -1 and OpenIMP refuses it the same
         * way (issue 119: the bypassed open pipeline delivers no frames), so -1 with
         * frames still flowing is the expected answer.  That part is opt-in
         * (APITEST_BYPASS_DISABLE=1): an older libimp still tears the link graph
         * down for DISABLE and wedges a T10/T20/T31 camera until reboot. */
        int r = IMP_ISP_Tuning_SetISPBypass(IMPISP_TUNING_OPS_MODE_ENABLE), r2 = -1, f1, f2 = 1;
        int dis = getenv("APITEST_BYPASS_DISABLE") && atoi(getenv("APITEST_BYPASS_DISABLE"));

        usleep(700000);
        f1 = flow();
        if (dis) {
            r2 = IMP_ISP_Tuning_SetISPBypass(IMPISP_TUNING_OPS_MODE_DISABLE);
            usleep(700000);
            f2 = flow();
        }
        rep(FN(IMP_ISP_Tuning_SetISPBypass), r, (r == 0 && f1 && r2 == -1 && f2) ? V_PASS : V_FAIL,
            "rt: ENABLE (= full ISP pipeline, the running state) ret %d, frames %s%s%s%s",
            r, f1 ? "flow" : "STOPPED", dis ? "; DISABLE (real bypass, refused like the vendor T21) ret " : "",
            dis ? (r2 == -1 ? "-1" : "NOT -1") : "", dis ? (f2 ? ", frames flow" : ", frames STOPPED") : "");
    }
}
#else
static void cs_ispbypass_old(void) {}
#endif
#if !defined(PLATFORM_T41) && (HAS_IMP_ISP_Tuning_GetGamma && HAS_IMP_ISP_Tuning_SetGamma)
static void cs_gamma_old(void)
{
    NEED(IMP_ISP_Tuning_GetGamma) NEED(IMP_ISP_Tuning_SetGamma) {
        G(IMPISPGamma, o); G(IMPISPGamma, b); G(IMPISPGamma, u);
        int r1 = IMP_ISP_Tuning_GetGamma(o), r2, r3, r4, r5, k, ms, eff, nz = 0;
        Fst base, steep, lin, back;

        gchk(o);
        for (k = 0; k < 129; k++) if (o->gamma[k]) nz++;
        rep(FN(IMP_ISP_Tuning_GetGamma), r1, (r1 == 0 && nz > 100) ? V_PASS : V_FAIL, "g[0]=%u g[64]=%u g[128]=%u (%d of 129 points non-zero)", o->gamma[0], o->gamma[64], o->gamma[128], nz);
        if (nz <= 100) { gfree(o); gfree(b); gfree(u); return; }       /* nothing to restore from: do not touch the curve */
        r2 = IMP_ISP_Tuning_SetGamma(o); r3 = IMP_ISP_Tuning_GetGamma(b); gchk(b);
        base = fst(800);
        for (k = 0; k < 129; k++) u->gamma[k] = (uint16_t)(pow(k / 128.0, 0.3) * 4095 + 0.5);
        r4 = IMP_ISP_Tuning_SetGamma(u); steep = fst(1500); r5 = IMP_ISP_Tuning_GetGamma(b);
        {
            int same = !memcmp(u, b, sizeof(*u));
            for (k = 0; k < 129; k++) u->gamma[k] = (uint16_t)(k * 4095 / 128);
            IMP_ISP_Tuning_SetGamma(u); lin = fst(1500);
            IMP_ISP_Tuning_SetGamma(o); back = recover(base.y, &ms);
            eff = steep.ok && lin.ok && steep.y > lin.y * 1.15 + 3;
            rep(FN(IMP_ISP_Tuning_SetGamma), r2 | r4, (r2 == 0 && r3 == 0 && r4 == 0 && r5 == 0 && same && ms >= 0 && eff) ? V_PASS : V_FAIL,
                "same value written (ret %d); curve x^0.3 written, read back %s; Y %.1f vs linear Y %.1f (base %.1f): %s; picture %s restored (Y %.1f)", r2, same ? "equal" : "DIFFERENT",
                steep.y, lin.y, base.y, eff ? "effect" : "NO PICTURE EFFECT", ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(o); gfree(b); gfree(u);
    }
}
#else
static void cs_gamma_old(void) {}
#endif


#if HAS_IMP_ISP_Tuning_GetCsc_Attr && HAS_IMP_ISP_Tuning_SetCsc_Attr
static void cs_csc(void)
{
    NEED(IMP_ISP_Tuning_GetCsc_Attr) NEED(IMP_ISP_Tuning_SetCsc_Attr) {
        G(IMPISPCscAttr, o); G(IMPISPCscAttr, b); G(IMPISPCscAttr, a);
        int r1 = IMP_ISP_Tuning_GetCsc_Attr(V0 o), r2, r3, r4, r5, ms;
        Fst base, full, clip, back;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetCsc_Attr), r1, r1 == 0 ? V_PASS : V_FAIL, "mode %d coef[0] %d", (int)o->mode, o->csc_par.csc_coef[0]);
        r2 = IMP_ISP_Tuning_SetCsc_Attr(V0 o); usleep(600000); r3 = IMP_ISP_Tuning_GetCsc_Attr(V0 b); gchk(b);
        base = fst(600);
        *a = *o; a->mode = ISP_CSC_CG_BT601_FULL;
        r4 = IMP_ISP_Tuning_SetCsc_Attr(V0 a); full = fst(1200);
        a->mode = ISP_CSC_CG_BT601_CLIP;
        r5 = IMP_ISP_Tuning_SetCsc_Attr(V0 a); clip = fst(1200);
        IMP_ISP_Tuning_SetCsc_Attr(V0 o); back = recover(base.y, &ms);
        {
            int eff = full.ok && clip.ok && eff_metric(1, &full, &clip) > 0.03;
            rep(FN(IMP_ISP_Tuning_SetCsc_Attr), r2 | r4 | r5, (r2 == 0 && r3 == 0 && o->mode == b->mode && r4 == 0 && r5 == 0 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%ssame value written (ret %d), mode read back %d; BT601 full sd %.1f vs clip sd %.1f; picture %s restored (Y %.1f)", RTMSG(eff), r2, (int)b->mode, full.sd, clip.sd, ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(o); gfree(b); gfree(a);
    }
}
#else
static void cs_csc(void) {}
#endif

#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SetAwbCt && HAS_IMP_ISP_Tuning_GetAWBCt
static void cs_awbct(void)
{
    NEED(IMP_ISP_Tuning_SetAwbCt) NEED(IMP_ISP_Tuning_GetAWBCt) {
        unsigned int o = 0, b = 0;
        int r1 = IMP_ISP_Tuning_GetAWBCt(&o), r2, r3, ms;
        Fst base, back;

        base = fst(600);
        r2 = IMP_ISP_Tuning_SetAwbCt(&o); usleep(800000); r3 = IMP_ISP_Tuning_GetAWBCt(&b); back = recover(base.y, &ms);
        rep(FN(IMP_ISP_Tuning_SetAwbCt), r2, (r1 == 0 && r2 == 0 && r3 == 0 && ms >= 0 && flow()) ? V_PASS : V_FAIL, "rt: current colour temperature %u K written (ret %d), now %u K, picture %s same (Y %.1f -> %.1f), frames flow", o, r2, b, ms >= 0 ? "" : "NOT", base.y, back.y);
    }
}
#else
static void cs_awbct(void) {}
#endif

#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SetScalerLv
static void cs_scalerlv_old(void)
{
    NEED(IMP_ISP_Tuning_SetScalerLv) {
        G(IMPISPScalerLv, a);
        int r1, r2, r3, ms;
        Fst lo, hi, back, base;

        base = fst(800);
        a->channel = 1; a->method = 0; a->level = 0;
        r1 = IMP_ISP_Tuning_SetScalerLv(a); lo = fst(1200);
        a->level = 128;
        r2 = IMP_ISP_Tuning_SetScalerLv(a); hi = fst(1200);
        a->level = 64;      /* no getter: the middle of the 0..128 range */
        r3 = IMP_ISP_Tuning_SetScalerLv(a); back = recover(base.y, &ms);
        {
            int eff = lo.ok && hi.ok && dabs(lo.h - hi.h) > 0.05;
            rep(FN(IMP_ISP_Tuning_SetScalerLv), r1 | r2 | r3, (r1 == 0 && r2 == 0 && r3 == 0 && lo.ok && hi.ok && lo.y > 5 && hi.y > 5 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%sch1 level 0/128/64 accepted, detail H %.3f at 0 vs %.3f at 128, Y %.1f/%.1f, picture %s alive (Y %.1f)", RTMSG(eff), lo.h, hi.h, lo.y, hi.y, ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(a);
    }
}
#else
static void cs_scalerlv_old(void) {}
#endif


#if defined(PLATFORM_T41) && HAS_IMP_ISP_WDR_ENABLE && HAS_IMP_ISP_WDR_ENABLE_GET
static void cs_wdr_t41(void)
{
    NEED(IMP_ISP_WDR_ENABLE_GET) NEED(IMP_ISP_WDR_ENABLE) {
        IMPISPTuningOpsMode o = IMPISP_TUNING_OPS_MODE_DISABLE, b = IMPISP_TUNING_OPS_MODE_ENABLE;
        int r1 = IMP_ISP_WDR_ENABLE_GET(IMPVI_MAIN, &o), r2, r3;

        rep(FN(IMP_ISP_WDR_ENABLE_GET), r1, r1 == 0 ? V_PASS : V_FAIL, "wdr %d", (int)o);
        r2 = IMP_ISP_WDR_ENABLE(IMPVI_MAIN, &o); usleep(700000); r3 = IMP_ISP_WDR_ENABLE_GET(IMPVI_MAIN, &b);
        rep(FN(IMP_ISP_WDR_ENABLE), r2, (r2 == 0 && r3 == 0 && b == o && flow()) ? V_PASS : V_FAIL, "rt: same value (%d) written (ret %d), read back %d, frames flow", (int)o, r2, (int)b);
    }
}
#else
static void cs_wdr_t41(void) {}
#endif

#if defined(APITEST_T40) && HAS_IMP_ISP_Tuning_SetISPBypass && HAS_IMP_ISP_Tuning_GetISPBypass
static void cs_bypass_t40(void)
{
    NEED(IMP_ISP_Tuning_GetISPBypass) NEED(IMP_ISP_Tuning_SetISPBypass) {
        IMPISPTuningOpsMode o = IMPISP_TUNING_OPS_MODE_DISABLE, b = IMPISP_TUNING_OPS_MODE_ENABLE;
        int r1 = IMP_ISP_Tuning_GetISPBypass(IMPVI_MAIN, &o), r2, r3;

        rep(FN(IMP_ISP_Tuning_GetISPBypass), r1, r1 == 0 ? V_PASS : V_FAIL, "bypass %d", (int)o);
        r2 = IMP_ISP_Tuning_SetISPBypass(IMPVI_MAIN, &o); usleep(700000); r3 = IMP_ISP_Tuning_GetISPBypass(IMPVI_MAIN, &b);
        rep(FN(IMP_ISP_Tuning_SetISPBypass), r2, (r2 == 0 && r3 == 0 && b == o && flow()) ? V_PASS : V_FAIL, "rt: same value (%d) written (ret %d), read back %d, frames flow", (int)o, r2, (int)b);
    }
}
#else
static void cs_bypass_t40(void) {}
#endif

#if defined(APITEST_T40) && HAS_IMP_ISP_SetCameraInputSelect
static void cs_camsel(void)
{
    NEED(IMP_ISP_SetCameraInputSelect) {
        int r = IMP_ISP_SetCameraInputSelect(IMPVI_MAIN);

        usleep(700000);
        /* vendor (libimp disassembly, src/t40/openimp_p1.c): -10 unless SetCameraInputMode selected the dual-sensor "select" mode; a single-sensor camera is never in it */
        rep(FN(IMP_ISP_SetCameraInputSelect), r, ((r == 0 || r == -10) && flow()) ? V_PASS : V_FAIL, "rt: input IMPVI_MAIN (ret %d%s), frames flow", r, r == -10 ? " = the vendor's error when the camera input mode is not 'select' (single sensor)" : "");
    }
}
#else
static void cs_camsel(void) {}
#endif


#if (defined(PLATFORM_T10) || defined(PLATFORM_T20)) && HAS_IMP_ISP_Tuning_AE_GetROI && HAS_IMP_ISP_Tuning_AE_SetROI
static void cs_aeroi(void)
{
    NEED(IMP_ISP_Tuning_AE_GetROI) NEED(IMP_ISP_Tuning_AE_SetROI) {
        IMPISPAERoi o, b, c, a;
        int r1, r2, r3, r4, r5, ms;
        Fst base, back;

        memset(&o, 0, sizeof(o)); memset(&b, 0, sizeof(b)); memset(&c, 0, sizeof(c));
        r1 = IMP_ISP_Tuning_AE_GetROI(&o);
        rep(FN(IMP_ISP_Tuning_AE_GetROI), r1, r1 == 0 ? V_PASS : V_FAIL, "roi 0x%08x (%u,%u)-(%u,%u)", o.value, o.startx, o.starty, o.endx, o.endy);
        base = fst(600);
        r2 = IMP_ISP_Tuning_AE_SetROI(&o); usleep(500000); r3 = IMP_ISP_Tuning_AE_GetROI(&b);
        a = o; a.startx = 64; a.starty = 64; a.endx = 192; a.endy = 192;
        r4 = IMP_ISP_Tuning_AE_SetROI(&a); usleep(800000); r5 = IMP_ISP_Tuning_AE_GetROI(&c);
        IMP_ISP_Tuning_AE_SetROI(&o); back = recover(base.y, &ms);
        rep(FN(IMP_ISP_Tuning_AE_SetROI), r2 | r4, (r2 == 0 && r3 == 0 && b.value == o.value && r4 == 0 && r5 == 0 && c.value == a.value && ms >= 0 && flow()) ? V_PASS : V_FAIL,
            "rt: same roi 0x%08x written, read back 0x%08x; centre window 0x%08x written, read back 0x%08x; restored, picture %s same (Y %.1f -> %.1f)", o.value, b.value, a.value, c.value, ms >= 0 ? "" : "NOT", base.y, back.y);
    }
}
#else
static void cs_aeroi(void) {}
#endif

#if !defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetRawDRC && HAS_IMP_ISP_Tuning_SetRawDRC
static void cs_rawdrc(void)
{
    NEED(IMP_ISP_Tuning_GetRawDRC) NEED(IMP_ISP_Tuning_SetRawDRC) {
        G(IMPISPDrcAttr, o); G(IMPISPDrcAttr, b);
        int r1 = IMP_ISP_Tuning_GetRawDRC(o), r2, r3;

        gchk(o);
        rep(FN(IMP_ISP_Tuning_GetRawDRC), r1, r1 == 0 ? V_PASS : V_FAIL, "mode %d strength %u slope %u/%u black %u white %u", (int)o->mode, o->drc_strength, o->slop_max, o->slop_min, o->black_level, o->white_level);
        r2 = IMP_ISP_Tuning_SetRawDRC(o); usleep(700000); r3 = IMP_ISP_Tuning_GetRawDRC(b); gchk(b);
        rep(FN(IMP_ISP_Tuning_SetRawDRC), r2, (r2 == 0 && r3 == 0 && !memcmp(o, b, sizeof(*o)) && flow()) ? V_PASS : V_FAIL,
            "rt: same value written (set ret %d), read back %s, frames flow (the off/manual modes can black the picture on some SoCs: not written, see imgfx rawdrc-*)", r2, !memcmp(o, b, sizeof(*o)) ? "equal" : "DIFFERENT");
        gfree(o); gfree(b);
    }
}
#else
static void cs_rawdrc(void) {}
#endif


/* ---- ISP-drawn OSD (raptor-hal sequence): pool, create, set attr (ARGB picture), show/hide, destroy; the picture must appear in the frame */
#if HAS_IMP_ISP_Tuning_CreateOsdRgn && HAS_IMP_ISP_Tuning_SetOsdRgnAttr && HAS_IMP_ISP_Tuning_ShowOsdRgn && HAS_IMP_ISP_Tuning_DestroyOsdRgn && HAS_IMP_ISP_Tuning_SetOsdPoolSize
static double region_y(int x0, int y0, int x1, int y1)      /* mean luma of a rectangle of FS ch0 (sensor size coordinates), 2 frames */
{
    double acc = 0; int n = 0, f, i, j;

    usleep(getenv("APITEST_OSD_WAIT") ? (useconds_t)atoi(getenv("APITEST_OSD_WAIT")) * 1000 : 700000);
    for (f = 0; f < 4; f++) {
        IMPFrameInfo *fr = NULL;
        const unsigned char *y;
        if (IMP_FrameSource_GetFrame(0, &fr) < 0 || !fr) continue;
        y = (const unsigned char *)(uintptr_t)fr->virAddr;
        if (f >= 2 && y && x1 <= fr->width && y1 <= fr->height) {
            double s = 0; int c = 0;
            for (j = y0; j < y1; j++) for (i = x0; i < x1; i++) { s += y[(size_t)j * fr->width + i]; c++; }
            acc += s / c; n++;
        }
        IMP_FrameSource_ReleaseFrame(0, fr);
    }
    return n ? acc / n : -1;
}
#endif

#if !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_CreateOsdRgn && HAS_IMP_ISP_Tuning_SetOsdRgnAttr && HAS_IMP_ISP_Tuning_ShowOsdRgn && HAS_IMP_ISP_Tuning_DestroyOsdRgn && HAS_IMP_ISP_Tuning_SetOsdPoolSize
static void cs_isposd(void)
{
    NEED(IMP_ISP_Tuning_CreateOsdRgn) NEED(IMP_ISP_Tuning_SetOsdRgnAttr) NEED(IMP_ISP_Tuning_ShowOsdRgn) NEED(IMP_ISP_Tuning_DestroyOsdRgn) NEED(IMP_ISP_Tuning_SetOsdPoolSize) {
        enum { PWD = 128, PHT = 64 };
        uint8_t *pic = (uint8_t *)malloc(PWD * PHT * 4);
        IMPIspOsdAttrAsm *a = (IMPIspOsdAttrAsm *)calloc(1, sizeof(*a));
        int rp, h, rs, rsh, rhide, rd = -99, k, x0 = getenv("APITEST_OSD_X") ? atoi(getenv("APITEST_OSD_X")) : 64, y0 = getenv("APITEST_OSD_Y") ? atoi(getenv("APITEST_OSD_Y")) : 64;
        int cx0 = x0 + 4, cy0 = y0 + 4, cx1 = x0 + PWD - 4, cy1 = y0 + PHT - 4;
        double base, shown, hidden;
        int dark, jenc = 0;

        if (!pic || !a || cx1 <= cx0 + 2 || cy1 <= cy0 + 2) { rep(FN(IMP_ISP_Tuning_CreateOsdRgn), 0, V_SKIP, "no memory or sensor too small for the test picture"); free(pic); free(a); return; }
        base = region_y(cx0, cy0, cx1, cy1);
        dark = base < 140;
        for (k = 0; k < PWD * PHT; k++) { uint8_t v = getenv("APITEST_OSD_FILL") ? (uint8_t)atoi(getenv("APITEST_OSD_FILL")) : (dark ? 255 : 0); pic[k * 4] = v; pic[k * 4 + 1] = v; pic[k * 4 + 2] = v; pic[k * 4 + 3] = 255; }
#ifdef PLATFORM_T41
        /* a consumer on the drawn output: a JPEG encoder bound to FS ch0 (the ISP OSD is committed per frame of MSCA output 0;
         * isp_osd_devtest, the proof of the driver, runs with it) */
        {
            IMPCell fs0 = { DEV_ID_FS, 0, 0 }, en0 = { DEV_ID_ENC, 6, 0 };
            IMPEncoderChnAttr *ea = (IMPEncoderChnAttr *)calloc(1, sizeof(*ea));

            if (ea && IMP_Encoder_SetDefaultParam && IMP_Encoder_CreateGroup(6) == 0) {
                IMP_Encoder_SetDefaultParam(ea, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP, g_sw, g_sh, 15, 1, 0, 0, 30, 0);
                if (IMP_Encoder_CreateChn(6, ea) == 0 && IMP_Encoder_RegisterChn(6, 6) == 0 && IMP_System_Bind(&fs0, &en0) == 0) {
                    jenc = 1;
                    IMP_Encoder_StartRecvPic(6);
                }
            }
            free(ea);
        }
#endif
        { int (*oinit)(void) = (int (*)(void))dsym("IMP_OSD_Init_ISP"); if (oinit) oinit(); }      /* T23: the ISP OSD module needs its init first */
        rp = IMP_ISP_Tuning_SetOsdPoolSize(512 * 1024);
        rep(FN(IMP_ISP_Tuning_SetOsdPoolSize), rp, rp == 0 ? V_PASS : V_FAIL, "512 KB");
        h = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
        rep(FN(IMP_ISP_Tuning_CreateOsdRgn), h, h >= 0 ? V_PASS : V_FAIL, "handle %d", h);
        if (h < 0) { free(pic); free(a); return; }
        a->type = ISP_OSD_REG_PIC;
        a->stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888;
#ifdef PLATFORM_T41
        /* T41 hardware (audit T41_ISP_OSD_1026): alpha is read from the MSB of the little-endian word; a B,G,R,A byte bitmap needs
         * IMP_ISP_ARGB_TYPE_ARGB.  With BGRA byte 0 is the alpha: a black (0,0,0,255) pixel is fully transparent. */
        a->stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_ARGB;
#else
        a->stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_BGRA;
#endif
        a->stsinglepicAttr.chnOSDAttr.osd_pixel_alpha_disable = IMPISP_TUNING_OPS_MODE_DISABLE;
        
#ifdef PLATFORM_T23
        a->stsinglepicAttr.chx = 0;
#endif
        a->stsinglepicAttr.pic.pinum = (uint8_t)h; a->stsinglepicAttr.pic.osd_enable = 1;
        a->stsinglepicAttr.pic.osd_left = x0; a->stsinglepicAttr.pic.osd_top = y0;
        a->stsinglepicAttr.pic.osd_width = PWD; a->stsinglepicAttr.pic.osd_height = PHT;
        a->stsinglepicAttr.pic.osd_image = (char *)pic; a->stsinglepicAttr.pic.osd_stride = PWD * 4;
        if (getenv("APITEST_OSD_DEBUG")) {
            int cxm, cym; char dbg[200]; int n = 0;
            for (cym = 0; cym < 2; cym++) for (cxm = 0; cxm < 4; cxm++)
                n += snprintf(dbg + n, sizeof(dbg) - n, "%.0f ", region_y(x0 + cxm * PWD / 4 + 2, y0 + cym * PHT / 2 + 2, x0 + (cxm + 1) * PWD / 4 - 2, y0 + (cym + 1) * PHT / 2 - 2));
            fprintf(stderr, "[T] osd cells BEFORE luma: %s\n", dbg);
        }
        rs = IMP_ISP_Tuning_SetOsdRgnAttr(0, h, a);
        rsh = IMP_ISP_Tuning_ShowOsdRgn(0, h, 1);
        shown = region_y(cx0, cy0, cx1, cy1);
        if (getenv("APITEST_OSD_DEBUG")) {          /* where in the picture area is the OSD? 8x6 cells of 32x32 luma from (x0-32, y0-32) */
            int cxm, cym; char dbg[400]; int n = 0;
            for (cym = 0; cym < 6; cym++) { for (cxm = 0; cxm < 8; cxm++)
                n += snprintf(dbg + n, sizeof(dbg) - n, "%.0f ", region_y(x0 - 32 + cxm * 32 + 2, y0 - 32 + cym * 32 + 2, x0 - 32 + (cxm + 1) * 32 - 2, y0 - 32 + (cym + 1) * 32 - 2)); n += snprintf(dbg + n, sizeof(dbg) - n, "| "); }
            fprintf(stderr, "[T] osd map 8x6 from (x0-32,y0-32): %s\n", dbg);
        }
        rhide = IMP_ISP_Tuning_ShowOsdRgn(0, h, 0);
        hidden = region_y(cx0, cy0, cx1, cy1);
        rd = IMP_ISP_Tuning_DestroyOsdRgn(0, h);
        {
            int appears = dark ? shown > base + 40 : shown < base - 40, gone = dabs(hidden - base) < 20;
            rep(FN(IMP_ISP_Tuning_SetOsdRgnAttr), rs, (rs == 0 && appears) ? V_PASS : V_FAIL, "%s %dx%d ARGB picture at (64,64): region luma %.1f -> %.1f (%s)", dark ? "white" : "black", (int)PWD, (int)PHT, base, shown, appears ? "visible" : "NOT VISIBLE");
            rep(FN(IMP_ISP_Tuning_ShowOsdRgn), rsh | rhide, (rsh == 0 && rhide == 0 && gone) ? V_PASS : V_FAIL, "show ret %d, hide ret %d, hidden: region luma %.1f (base %.1f)", rsh, rhide, hidden, base);
            rep(FN(IMP_ISP_Tuning_DestroyOsdRgn), rd, (rd == 0 && flow()) ? V_PASS : V_FAIL, "handle %d destroyed, frames flow", h);
        }
#ifdef PLATFORM_T41
        if (jenc) {
            IMPCell fs0 = { DEV_ID_FS, 0, 0 }, en0 = { DEV_ID_ENC, 6, 0 };
            IMP_Encoder_StopRecvPic(6);
            IMP_System_UnBind(&fs0, &en0);
            IMP_Encoder_UnRegisterChn(6);
            IMP_Encoder_DestroyChn(6);
            IMP_Encoder_DestroyGroup(6);
        }
#endif
        { void (*oexit)(void) = (void (*)(void))dsym("IMP_OSD_Exit_ISP"); if (oexit) oexit(); }
        free(pic); free(a);
    }
}
#else
static void cs_isposd(void) {}
#endif


#if defined(PLATFORM_T23)
/* nx x ny grid of mean luma of one ch1 frame, normalised to its own mean (where the picture content sits, independent of the exposure) */
static int grid_grab(double *g, int nx, int ny, int settle_ms)
{
    IMPFrameInfo *fr = NULL;
    int k, i, j, w, h, ok = 0;
    const unsigned char *y;
    double mean = 0;

    usleep(settle_ms * 1000);
    for (k = 0; k < 4; k++)
        if (IMP_FrameSource_GetFrame(1, &fr) == 0 && fr) { if (k < 3) { IMP_FrameSource_ReleaseFrame(1, fr); fr = NULL; } }
    if (!fr) return 0;
    y = (const unsigned char *)(uintptr_t)fr->virAddr; w = fr->width; h = fr->height;
    if (y && w >= 64 && h >= 48) {
        for (j = 0; j < ny; j++) for (i = 0; i < nx; i++) {
            int x0 = i * w / nx, x1 = (i + 1) * w / nx, y0 = j * h / ny, y1 = (j + 1) * h / ny, xx, yy, n = 0; double sum = 0;
            for (yy = y0; yy < y1; yy += 2) for (xx = x0; xx < x1; xx += 2) { sum += y[(size_t)yy * w + xx]; n++; }
            g[j * nx + i] = n ? sum / n : 0; mean += g[j * nx + i];
        }
        mean /= nx * ny;
        if (mean > 1) { for (i = 0; i < nx * ny; i++) g[i] /= mean; ok = 1; }
    }
    IMP_FrameSource_ReleaseFrame(1, fr);
    return ok;
}

static double grid_dist(const double *a, const double *b, int n)
{
    double d = 0; int i;
    for (i = 0; i < n; i++) d += fabs(a[i] - b[i]);
    return d / n;
}
#endif

/* SetAutoZoom without a getter (T31): ch1 scaler at its own size + a crop to the upper-left quarter (zoom 2x), then back to the full picture */
#if HAS_IMP_ISP_Tuning_SetAutoZoom && (!HAS_IMP_ISP_Tuning_GetAutoZoom || defined(PLATFORM_T23)) && !defined(PLATFORM_T41)
static void cs_autozoom_set(void)
{
    NEED(IMP_ISP_Tuning_SetAutoZoom) {
        G(IMPISPAutoZoom, a);
        int r1, r2, ms;
        Fst base, zoom, back;
#if defined(PLATFORM_T23)
        double b16[192], zg[48], kg[48], e_full[48], e_ctr[48], e_tl[48];
        int gb, gz, gk, i, j;
#endif

        base = fst(800);
#if defined(PLATFORM_T23)
        gb = grid_grab(b16, 16, 12, 100);
#endif
        a->chan = 1; a->scaler_enable = 1; a->scaler_outwidth = SUB_W; a->scaler_outheight = SUB_H;
        /* ePTZ: the crop window is cut out of the SCALED picture and its size is the channel's output size (the driver refuses a size change):
         * scale ch1 to 2x its size, crop the centre in the channel's own size = 2x zoom */
        a->scaler_outwidth = SUB_W * 2; a->scaler_outheight = SUB_H * 2;
        a->crop_enable = 1; a->crop_left = SUB_W / 2; a->crop_top = SUB_H / 2; a->crop_width = SUB_W; a->crop_height = SUB_H;
#if defined(PLATFORM_T23)
        /* T23 (stock autozoom control): the window is in SENSOR coordinates (inside the picture, >= 64x64, even) and the scaler output must not
         * exceed it: centre half of the sensor picture, scaled to the channel size = zoom on the centre */
        a->scaler_outwidth = SUB_W; a->scaler_outheight = SUB_H;
        a->crop_left = (g_sw / 4) & ~1; a->crop_top = (g_sh / 4) & ~1; a->crop_width = (g_sw / 2) & ~1; a->crop_height = (g_sh / 2) & ~1;
#endif
        r1 = IMP_ISP_Tuning_SetAutoZoom(a); zoom = fst(1500);
#if defined(PLATFORM_T23)
        gz = grid_grab(zg, 8, 6, 100);
#endif
        a->crop_enable = 0; a->scaler_outwidth = SUB_W; a->scaler_outheight = SUB_H;
        r2 = IMP_ISP_Tuning_SetAutoZoom(a); back = recover(base.y, &ms);
#if defined(PLATFORM_T23)
        gk = grid_grab(kg, 8, 6, 300);
        for (j = 0; j < 6; j++) for (i = 0; i < 8; i++) {
            const double *c = &b16[0];
            e_full[j * 8 + i] = (c[(2 * j) * 16 + 2 * i] + c[(2 * j) * 16 + 2 * i + 1] + c[(2 * j + 1) * 16 + 2 * i] + c[(2 * j + 1) * 16 + 2 * i + 1]) / 4;
            e_ctr[j * 8 + i] = c[(j + 3) * 16 + i + 4];
            e_tl[j * 8 + i] = c[j * 16 + i];
        }
#endif
        {
            int eff = zoom.ok && base.ok && dabs(zoom.h - base.h) > 0.1 * base.h;
#if defined(PLATFORM_T23)
            /* where the content sits (grids normalised to their mean): the zoomed picture is the centre of the base, the picture after the
             * back-Set is the whole base again (not a corner) */
            double dz_c = gb && gz ? grid_dist(zg, e_ctr, 48) : 99, dz_f = gb && gz ? grid_dist(zg, e_full, 48) : 99;
            double dk_f = gb && gk ? grid_dist(kg, e_full, 48) : 99, dk_t = gb && gk ? grid_dist(kg, e_tl, 48) : 99, dk_c = gb && gk ? grid_dist(kg, e_ctr, 48) : 99;
            int pos = dz_c < dz_f && dk_f < dk_t && dk_f < dk_c;
            rep(FN(IMP_ISP_Tuning_SetAutoZoom), r1 | r2, (r1 == 0 && r2 == 0 && ms >= 0 && flow() && pos) ? V_PASS : V_FAIL,
                "%sch1 zoom (centre half of the sensor picture, scaler = channel size, ret %d) and back (ret %d): detail H %.3f -> %.3f -> %.3f; layout distance zoom-picture to base centre %.3f / whole %.3f, back-picture to whole %.3f / top-left corner %.3f / centre %.3f (%s), frames flow",
                RTMSG(eff), r1, r2, base.h, zoom.h, back.h, dz_c, dz_f, dk_f, dk_t, dk_c, pos ? "zoom shows the centre, back shows the whole picture" : "WRONG picture area");
#else
            rep(FN(IMP_ISP_Tuning_SetAutoZoom), r1 | r2, (r1 == 0 && r2 == 0 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%sch1 2x zoom (scaler 2x, centre crop in the channel size, ret %d) and back (ret %d): detail H %.3f -> %.3f -> %.3f, frames flow", RTMSG(eff), r1, r2, base.h, zoom.h, back.h);
#endif
        }
        gfree(a);
    }
}
#else
static void cs_autozoom_set(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- T41 AE API (issue 90) */
#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_GetAeStatistics   /* T40: own AE API cases (t40dev); never GetAeExpList on the Eufy */
/* optional Y snapshot of FS ch1 (PGM, 320x180 = every 2nd pixel) into APITEST_SNAPDIR, for the labeled collage; nothing without it */
static void snap_y(const char *tag)
{
    const char *d = getenv("APITEST_SNAPDIR");
    IMPFrameInfo *fr = NULL;
    char path[256];
    FILE *f;
    int k;

    if (!d || !IMP_FrameSource_GetFrame) return;
    for (k = 0; k < 3; k++) if (IMP_FrameSource_GetFrame(1, &fr) == 0 && fr) { if (k < 2) { IMP_FrameSource_ReleaseFrame(1, fr); fr = NULL; } }
    if (!fr) return;
    snprintf(path, sizeof(path), "%s/%s.pgm", d, tag);
    if ((f = fopen(path, "wb"))) {
        const unsigned char *y = (const unsigned char *)(uintptr_t)fr->virAddr;
        int w = fr->width, h = fr->height, i, j;

        fprintf(f, "P5\n%d %d\n255\n", w / 2, h / 2);
        for (j = 0; j < h / 2; j++) for (i = 0; i < w / 2; i++) fputc(y[(size_t)(2 * j) * w + 2 * i], f);
        fclose(f);
    }
    IMP_FrameSource_ReleaseFrame(1, fr);
}

/* expected 5-bin histogram (65535 scale) from the 256 bins and the limits, as the vendor AE's api_ae_long_process */
static void hist5_expect(const IMPISPAEStatisInfo *st, const unsigned char *thr, unsigned *e)
{
    double b[5] = { 0, 0, 0, 0, 0 }, t = 0;
    int i, k;

    for (i = 0; i < 256; i++) {
        for (k = 0; k < 4 && i >= thr[k]; k++) ;
        b[k] += st->ae_hist_256bin[i];
        t += st->ae_hist_256bin[i];
    }
    for (k = 0; k < 5; k++) e[k] = t > 0 ? (unsigned)(b[k] * 65535.0 / t) : 0;
}

/* The vendor AE counts the 5 bins on the raw hardware histogram; the reported 256 bins are those x4 with bin 0 replaced by the
 * remainder of width x height, so only the split of bins 1..4 among themselves can be recomputed exactly: compare that (1 %). */
static int hist5_close(const IMPISPAEStatisInfo *st, const unsigned *e)
{
    double s = 0, x = 0;
    int k;

    for (k = 1; k < 5; k++) { s += st->ae_hist_5bin[k]; x += e[k]; }
    if (s == 0 || x == 0) return s == x;
    for (k = 1; k < 5; k++) if (dabs(st->ae_hist_5bin[k] / s - e[k] / x) > 0.01) return 0;
    return 1;
}

static void cs_aestats(void)
{
    NEED(IMP_ISP_Tuning_GetAeStatistics) NEED(IMP_ISP_Tuning_GetStatisConfig) NEED(IMP_ISP_Tuning_SetStatisConfig) {
        G(IMPISPAEStatisInfo, a); G(IMPISPAEStatisInfo, b);
        IMPISPStatisConfig o, c, back;
        static const unsigned char thr2[4] = { 32, 64, 96, 128 };
        unsigned long long sum = 0;
        unsigned e0[5], e1[5], zmin = ~0U, zmax = 0, s5 = 0;
        int r1, r2, r3, r4, r5, i, j, nz = 0, ok0, ok1;

        memset(&o, 0, sizeof(o)); memset(&back, 0, sizeof(back));
        r1 = IMP_ISP_Tuning_GetStatisConfig(IMPVI_MAIN, &o);
        r2 = IMP_ISP_Tuning_GetAeStatistics(IMPVI_MAIN, a); gchk(a);
        for (i = 0; i < 256; i++) { sum += a->ae_hist_256bin[i]; nz += a->ae_hist_256bin[i] != 0; }
        for (i = 0; i < 5; i++) s5 += a->ae_hist_5bin[i];
        for (j = 0; j < 15; j++) for (i = 0; i < 15; i++) { unsigned z = a->ae_statis.statis[j][i]; if (z < zmin) zmin = z; if (z > zmax) zmax = z; }
        hist5_expect(a, o.ae.histThresh, e0);
        ok0 = hist5_close(a, e0);
        rep(FN(IMP_ISP_Tuning_GetAeStatistics), r2, (r1 == 0 && r2 == 0 && sum == (unsigned long long)g_sw * g_sh && nz > 5 && s5 == 65535 && ok0 && zmax > zmin) ? V_PASS : V_FAIL,
            "256 bins sum %llu (= %d x %d: %s), %d non-zero bins; 5-bin %u/%u/%u/%u/%u (sum %u) vs from the 256 bins over histThresh (bins 1..4 split) %u/%u/%u/%u %u/%u/%u/%u/%u (%s); zones %u..%u (centre %u)",
            sum, g_sw, g_sh, sum == (unsigned long long)g_sw * g_sh ? "yes" : "NO", nz, a->ae_hist_5bin[0], a->ae_hist_5bin[1], a->ae_hist_5bin[2], a->ae_hist_5bin[3], a->ae_hist_5bin[4], s5,
            o.ae.histThresh[0], o.ae.histThresh[1], o.ae.histThresh[2], o.ae.histThresh[3], e0[0], e0[1], e0[2], e0[3], e0[4], ok0 ? "match" : "MISMATCH", zmin, zmax, a->ae_statis.statis[7][7]);
        /* histThresh 32/64/96/128 (AE part enabled, the rest as read), then the original back */
        c = o; c.ae.ae_sta_en = IMPISP_TUNING_OPS_MODE_ENABLE; memcpy(c.ae.histThresh, thr2, 4);
        r3 = IMP_ISP_Tuning_SetStatisConfig(IMPVI_MAIN, &c); usleep(600000);
        r4 = IMP_ISP_Tuning_GetAeStatistics(IMPVI_MAIN, b); gchk(b);
        hist5_expect(b, thr2, e1);
        ok1 = hist5_close(b, e1);
        /* restore: the original limits with the AE part enabled (a disabled part only clears its enable, the limits would stay),
         * then the original record as read */
        c = o; c.ae.ae_sta_en = IMPISP_TUNING_OPS_MODE_ENABLE;
        r5 = IMP_ISP_Tuning_SetStatisConfig(IMPVI_MAIN, &c);
        if (o.ae.ae_sta_en != IMPISP_TUNING_OPS_MODE_ENABLE) r5 |= IMP_ISP_Tuning_SetStatisConfig(IMPVI_MAIN, &o);
        usleep(300000);
        IMP_ISP_Tuning_GetStatisConfig(IMPVI_MAIN, &back);
        rep(FN(IMP_ISP_Tuning_SetStatisConfig), r3 | r5, (r3 == 0 && r4 == 0 && r5 == 0 && ok1 && !memcmp(back.ae.histThresh, o.ae.histThresh, 4) && flow()) ? V_PASS : V_FAIL,
            "histThresh 32/64/96/128: 5-bin %u/%u/%u/%u/%u vs from the 256 bins %u/%u/%u/%u/%u (%s; before %u/%u/%u/%u/%u); original %u/%u/%u/%u restored (%s), frames flow",
            b->ae_hist_5bin[0], b->ae_hist_5bin[1], b->ae_hist_5bin[2], b->ae_hist_5bin[3], b->ae_hist_5bin[4], e1[0], e1[1], e1[2], e1[3], e1[4], ok1 ? "match" : "MISMATCH",
            a->ae_hist_5bin[0], a->ae_hist_5bin[1], a->ae_hist_5bin[2], a->ae_hist_5bin[3], a->ae_hist_5bin[4],
            o.ae.histThresh[0], o.ae.histThresh[1], o.ae.histThresh[2], o.ae.histThresh[3], !memcmp(back.ae.histThresh, o.ae.histThresh, 4) ? "read back" : "NOT read back");
        gfree(a); gfree(b);
    }
}
#else
static void cs_aestats(void) {}
#endif

#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_SetFaceAe && HAS_IMP_ISP_Tuning_GetFaceAe
/* face AE as streamers use it: the window + target set every frame (vendor remark), here every 40 ms for 'ms' */
static Fst face_hold(IMPISPFaceAttr *f, int ms, int *ret)
{
    int64_t t0 = now_us();

    *ret = 0;
    while (now_us() - t0 < (int64_t)ms * 1000) { *ret |= IMP_ISP_Tuning_SetFaceAe(IMPVI_MAIN, f); usleep(40000); }
    return fst(0);
}

static void cs_faceae(void)
{
    NEED(IMP_ISP_Tuning_SetFaceAe) NEED(IMP_ISP_Tuning_GetFaceAe) {
        IMPISPFaceAttr o, f, g, bad, after;
        Fst base, lo, hi, back;
        int r0, r1, r2, r3, r4, r5, ms, hold;

        memset(&o, 0, sizeof(o)); memset(&g, 0, sizeof(g)); memset(&after, 0, sizeof(after));
        r0 = IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &o);
        base = fst(600); snap_y("faceae-0-base");
        memset(&f, 0, sizeof(f));
        f.enable = IMPISP_TUNING_OPS_MODE_ENABLE;
        f.left = g_sw / 4; f.top = g_sh / 4; f.right = g_sw * 3 / 4; f.bottom = g_sh * 3 / 4;
        hold = getenv("APITEST_FACE_MS") ? atoi(getenv("APITEST_FACE_MS")) : 8000;   /* the face AE converges slowly (vendor AE steps): 9 s moves the centre ~17 levels */
        f.target = 40; lo = face_hold(&f, hold, &r1); snap_y("faceae-1-target40");
        r2 = IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &g);
        f.target = 180; hi = face_hold(&f, hold, &r3); snap_y("faceae-2-target180");
        bad = f; bad.left = f.right + 8;                                  /* empty window: refused, nothing changed */
        r4 = IMP_ISP_Tuning_SetFaceAe(IMPVI_MAIN, &bad);
        IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &after);
        memset(&f, 0, sizeof(f)); f.enable = IMPISP_TUNING_OPS_MODE_DISABLE;
        r5 = IMP_ISP_Tuning_SetFaceAe(IMPVI_MAIN, &f);
        back = recover(base.y, &ms); snap_y("faceae-3-off");
        rep(FN(IMP_ISP_Tuning_GetFaceAe), r0 | r2, (r0 == 0 && r2 == 0 && g.enable == IMPISP_TUNING_OPS_MODE_ENABLE && g.left == (unsigned)g_sw / 4 && g.bottom == (unsigned)g_sh * 3 / 4 && g.target == 40) ? V_PASS : V_FAIL,
            "initial enable %d; while set: enable %d window (%u,%u)-(%u,%u) target %u (set (%d,%d)-(%d,%d) 40)", (int)o.enable, (int)g.enable, g.left, g.top, g.right, g.bottom, g.target,
            g_sw / 4, g_sh / 4, g_sw * 3 / 4, g_sh * 3 / 4);
        rep(FN(IMP_ISP_Tuning_SetFaceAe), r1 | r3 | r5, (r1 == 0 && r3 == 0 && r5 == 0 && r4 != 0 && after.target == 180 && lo.ok && hi.ok && hi.y > lo.y + 15 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
            "centre window metered: target 40 -> Y %.1f, target 180 -> Y %.1f (base %.1f); empty window refused (ret %d, target still %u); off: back to Y %.1f in %d ms, frames flow",
            lo.y, hi.y, base.y, r4, after.target, back.y, ms);
    }
}
#else
static void cs_faceae(void) {}
#endif

#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_SetAeExpList && HAS_IMP_ISP_Tuning_GetAeExpList
static void cs_aeexplist(void)
{
    NEED(IMP_ISP_Tuning_SetAeExpList) NEED(IMP_ISP_Tuning_GetAeExpList) {
        G(IMPISPAeExpListAttr, o); G(IMPISPAeExpListAttr, l); G(IMPISPAeExpListAttr, g); G(IMPISPAeExpListAttr, z);
        IMPISPAEExprInfo e0, e1;
        Fst base, on, back;
        int r0, r1, r2, r3, r4, i, same = 1, ms, have_ei = IMP_ISP_Tuning_GetAeExprInfo != NULL;

        memset(&e0, 0, sizeof(e0)); memset(&e1, 0, sizeof(e1));
        r0 = IMP_ISP_Tuning_GetAeExpList(IMPVI_MAIN, o); gchk(o);
        base = fst(600); if (have_ei) IMP_ISP_Tuning_GetAeExprInfo(IMPVI_MAIN, &e0);
        /* list mode on with the list as read (the calibration's own entries), so the exposure may only move inside the tuned range */
        *l = *o; l->mode = IMPISP_TUNING_OPS_MODE_ENABLE;
        r1 = IMP_ISP_Tuning_SetAeExpList(IMPVI_MAIN, l); on = fst(2000);
        if (have_ei) IMP_ISP_Tuning_GetAeExprInfo(IMPVI_MAIN, &e1);
        r2 = IMP_ISP_Tuning_GetAeExpList(IMPVI_MAIN, g); gchk(g);
        for (i = 0; i < 16; i++) if (g->elist[i][1] != l->elist[i][1] || g->elist[i][2] != l->elist[i][2] || g->elist[i][3] != l->elist[i][3]) same = 0;
        *z = *o; z->mode = IMPISP_TUNING_OPS_MODE_DISABLE;
        r3 = IMP_ISP_Tuning_SetAeExpList(IMPVI_MAIN, z);
        z->mode = (IMPISPTuningOpsMode)7; r4 = IMP_ISP_Tuning_SetAeExpList(IMPVI_MAIN, z);   /* invalid mode: refused */
        back = recover(base.y, &ms);
        rep(FN(IMP_ISP_Tuning_GetAeExpList), r0 | r2, (r0 == 0 && r2 == 0 && g->mode == IMPISP_TUNING_OPS_MODE_ENABLE && same) ? V_PASS : V_FAIL,
            "mode %d, row 0 it %u ag %u dg %u; after Set: mode %d, list %s", (int)o->mode, o->elist[0][1], o->elist[0][2], o->elist[0][3], (int)g->mode, same ? "read back" : "DIFFERENT");
        rep(FN(IMP_ISP_Tuning_SetAeExpList), r1 | r3, (r1 == 0 && r3 == 0 && r4 != 0 && on.ok && ms >= 0 && flow()) ? V_PASS : V_FAIL,
            "rt: list mode on (own list): Y %.1f -> %.1f, IT %u -> %u, AG %u -> %u; off: Y back to %.1f in %d ms; mode 7 refused (ret %d), frames flow",
            base.y, on.y, e0.AeIntegrationTime, e1.AeIntegrationTime, e0.AeAGain, e1.AeAGain, back.y, ms, r4);
        gfree(o); gfree(l); gfree(g); gfree(z);
    }
}
#else
static void cs_aeexplist(void) {}
#endif

/* ---------------------------------------------------------------------------------------------- T41 FrameSource pool (issue #72)
 * IMP_System_MemPoolRequest + IMP_FrameSource_SetPool: ch1 is destroyed, bound to a pool and re-created; its capture buffers must
 * come from the pool (frame physical addresses inside the pool range, OpenIMP_P2_PoolRange), frames flow, and after Disable/Destroy
 * the pool is idle again (IMP_System_MemPoolFree succeeds only without live buffers; rmem in use back to the start value, 3 cycles).
 * Vendor return codes: chn 33 -> 0x80010001, unknown pool -> 0x80010040, second SetPool -> 0x80010020.  A pool too small for the
 * queue: EnableChn fails, nothing leaks.  ch1 is restored without a pool at the end.  OpenIMP-only helpers are weak (vendor: N/A). */
#if defined(PLATFORM_T41) && HAS_IMP_FrameSource_SetPool && HAS_IMP_FrameSource_GetPool && HAS_IMP_System_MemPoolRequest
/* OpenIMP-only entry points, looked up at run time (vendor libimp: not there) */
static int (*pool_free_fn)(int);
static int (*pool_range_fn)(int, uint32_t *, uint32_t *);
static int (*dma_usage_fn)(uint32_t *, uint32_t *, uint32_t *, const char *, uint32_t *, uint32_t *);

static uint32_t rmem_used(void)
{
    uint32_t used = 0, total = 0, largest = 0, tb = 0, tc = 0;

    if (dma_usage_fn) dma_usage_fn(&used, &total, &largest, "x", &tb, &tc);
    return used;
}

static int ch1_up(const IMPFSChnAttr *a, int depth)
{
    if (IMP_FrameSource_CreateChn(1, (IMPFSChnAttr *)a)) return -1;
    if (depth > 0) IMP_FrameSource_SetFrameDepth(1, depth);
    return IMP_FrameSource_EnableChn(1);
}

static void ch1_down(void)
{
    IMP_FrameSource_DisableChn(1);
    IMP_FrameSource_DestroyChn(1);
}

static void cs_fspool(void)
{
    IMPFSChnAttr a;
    int depth = 0, cycle, e33, enone, ebound = 0, g = -1, fl_end = 0, ok_all = 1, inside_all = 1, flow_all = 1, free_all = 1, rs, rsmall, rback;
    uint32_t used0, used_end, pphys = 0, psize = 0, fmin = 0xffffffffu, fmax = 0;
    char note[160] = "";

    pool_free_fn = (int (*)(int))dlsym(RTLD_DEFAULT, "IMP_System_MemPoolFree");
    pool_range_fn = (int (*)(int, uint32_t *, uint32_t *))dlsym(RTLD_DEFAULT, "OpenIMP_P2_PoolRange");
    dma_usage_fn = (int (*)(uint32_t *, uint32_t *, uint32_t *, const char *, uint32_t *, uint32_t *))dlsym(RTLD_DEFAULT, "OpenIMP_P2_DMAUsage");
    if (!pool_free_fn || !pool_range_fn) { rep(FN(IMP_FrameSource_SetPool), 0, V_NA, "vendor libimp: no pool release / range (OpenIMP extension), case needs it"); return; }
    memset(&a, 0, sizeof(a));
    if (IMP_FrameSource_GetChnAttr(1, &a)) { rep(FN(IMP_FrameSource_SetPool), -1, V_SKIP, "ch1 not created"); return; }
    IMP_FrameSource_GetFrameDepth(1, &depth);
    ch1_down();
    used0 = rmem_used();
    e33 = IMP_FrameSource_SetPool(33, 9);
    enone = IMP_FrameSource_SetPool(1, 31);
    for (cycle = 0; cycle < 3; cycle++) {
        int r = IMP_System_MemPoolRequest(9, 2u << 20, "apitest-fs1"), k, n = 0;

        if (r || IMP_FrameSource_SetPool(1, 9)) { ok_all = 0; break; }
        if (!cycle) ebound = IMP_FrameSource_SetPool(1, 9);
        g = IMP_FrameSource_GetPool(1);
        if (g != 9) ok_all = 0;
        pool_range_fn(9, &pphys, &psize);
        if (ch1_up(&a, depth > 2 ? depth : 2)) { ok_all = 0; snprintf(note, sizeof(note), "cycle %d: ch1 enable in the pool failed", cycle); break; }
        usleep(500000);
        for (k = 0; k < 8; k++) {
            IMPFrameInfo *fr = NULL;
            if (IMP_FrameSource_GetFrame(1, &fr) == 0 && fr) {
                if (fr->phyAddr < pphys || fr->phyAddr + fr->size > pphys + psize) inside_all = 0;
                if (fr->phyAddr < fmin) fmin = fr->phyAddr;
                if (fr->phyAddr > fmax) fmax = fr->phyAddr;
                n++;
                IMP_FrameSource_ReleaseFrame(1, fr);
            }
        }
        if (n < 6 || !flow()) { flow_all = 0; snprintf(note, sizeof(note), "cycle %d: %d of 8 frames", cycle, n); }
        ch1_down();
        if (pool_free_fn(9)) free_all = 0;
    }
    used_end = rmem_used();
    /* a pool too small for the capture queue: EnableChn fails, no leak */
    rs = IMP_System_MemPoolRequest(10, 64u << 10, "apitest-small");
    if (!rs) IMP_FrameSource_SetPool(1, 10);
    rsmall = IMP_FrameSource_CreateChn(1, &a) ? -99 : IMP_FrameSource_EnableChn(1);
    IMP_FrameSource_DestroyChn(1);
    rs |= pool_free_fn(10);
    /* restore ch1 as the run had it (no pool) */
    rback = ch1_up(&a, depth > 2 ? depth : 2);
    usleep(500000);
    fl_end = flow();
    if (!rback) IMP_FrameSource_SetFrameDepth(1, depth);   /* the depth the run had */
    rep(FN(IMP_FrameSource_GetPool), g, (g == 9 && IMP_FrameSource_GetPool(1) == (int)0x80010020 && IMP_FrameSource_GetPool(33) == (int)0x80010001) ? V_PASS : V_FAIL,
        "bound ch1 -> %d; after the pool release: 0x%x (vendor 0x80010020 none), chn 33 -> 0x%x", g, (unsigned)IMP_FrameSource_GetPool(1), (unsigned)IMP_FrameSource_GetPool(33));
    rep(FN(IMP_FrameSource_SetPool), ok_all ? 0 : -1,
        (ok_all && inside_all && flow_all && free_all && e33 == (int)0x80010001 && enone == (int)0x80010040 && ebound == (int)0x80010020 &&
         used_end == used0 && rs == 0 && rsmall != 0 && rmem_used() <= used0 + 4u * 1024 * 1024 && rback == 0 && fl_end) ? V_PASS : V_FAIL,
        "dev: 3 cycles pool 9 (2 MB at 0x%08x): ch1 frames 0x%08x..0x%08x %s the pool, frames flow %s, pool free after Destroy %s; rmem in use %u -> %u KB; "
        "chn 33 0x%x, unknown pool 0x%x, second set 0x%x; 64 KB pool: EnableChn ret %d (refused), released %s; ch1 restored without pool ret %d, frames %s %s",
        pphys, fmin, fmax, inside_all ? "inside" : "OUTSIDE", flow_all ? "yes" : "NO", free_all ? "yes" : "NO", used0 / 1024, used_end / 1024,
        (unsigned)e33, (unsigned)enone, (unsigned)ebound, rsmall, rs ? "NO" : "yes", rback, fl_end ? "flow" : "STOPPED", note);
}
#else
static void cs_fspool(void) {}
#endif

/* APITEST_CASES=a,b,c runs only these cases (names: sensorreg log waitframe videodrop movestate aefreeze aehistorigin autozoom framedrop cwf sinter temper process fwfreeze hilight inttime scene colorfx gamma modctl ccm rgbcoef ispbypass binpath switchbin scalerlv contrast hue aecomp backlight dpc custom wdr wdrout ispflip hvflip ispbypass_old gamma_old csc awbct scalerlv_old aestats faceae aeexplist dmic fspool); default all */
static int cs_on(const char *n)
{
    const char *e = getenv("APITEST_CASES"), *p;
    size_t l = strlen(n);

    if (!e) return 1;
    for (p = e; (p = strstr(p, n)); p += l) if ((p == e || p[-1] == ',') && (p[l] == ',' || !p[l])) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------------------------- stale first snapshot (issue #89)
 * A stream kept alive without a consumer fills every capture buffer once and leaves them "done"; the first frame requested after a
 * pause must be a fresh one, not the oldest buffer.  The T40 frame timestamp is taken at DQBUF (no completion time on the wire),
 * so it cannot show staleness: the picture does.  The ISP brightness is switched while nobody reads (after the buffers are full):
 * a stale buffer still shows the old brightness, a fresh frame the new one (mean luma of ch1 NV12).  Explicit case only
 * (APITEST_CASES=stalesnap), pauses APITEST_STALE_PAUSES (default "2,10,30" seconds), both consumers: SnapFrame and GetFrame. */
#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetBrightness && HAS_IMP_ISP_Tuning_SetBrightness && HAS_IMP_FrameSource_GetFrame
static double luma_nv12(const unsigned char *y, int w, int h)
{
    double s = 0;
    long n = 0, i, tot = (long)w * h;

    if (!y || tot <= 0) return -1;
    for (i = 0; i < tot; i += 53) { s += y[i]; n++; }
    return s / n;
}

static double frame_luma(void)      /* one GetFrame, mean luma, -1 = no frame */
{
    IMPFrameInfo *fr = NULL;
    double y;

    if (IMP_FrameSource_GetFrame(1, &fr) < 0 || !fr) return -1;
    y = luma_nv12((const unsigned char *)(uintptr_t)fr->virAddr, fr->width, fr->height);
    IMP_FrameSource_ReleaseFrame(1, fr);
    return y;
}

static void set_bright(unsigned char v) { unsigned char t = v; IMP_ISP_Tuning_SetBrightness(IMPVI_MAIN, &t); }

static void cs_stalesnap(void)
{
    const char *pe = getenv("APITEST_STALE_PAUSES");
    char list[64], *tok, *sv = NULL;
    size_t nv12 = (size_t)SUB_W * SUB_H * 3 / 2;
    unsigned char ob = 128;
    int round = 0, fails = 0, runs = 0;

    snprintf(list, sizeof list, "%s", pe && *pe ? pe : "2,10,30");
    NEED(IMP_FrameSource_GetFrame) NEED(IMP_ISP_Tuning_SetBrightness) {
        IMP_ISP_Tuning_GetBrightness(IMPVI_MAIN, &ob);
        for (tok = strtok_r(list, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
            int pause_s = atoi(tok), mode;

            if (pause_s <= 0) continue;
            for (mode = 0; mode < 2; mode++) {       /* 0 SnapFrame, 1 GetFrame */
                unsigned char from = (round & 1) ? 220 : 40, to = (round & 1) ? 40 : 220;
                double y_from, y_to, y_first, lat_ms, y_after, diff, d_to, d_from;
                int64_t ts_first = 0, t_req, t_ret, ok_snap = 0;
                unsigned char *buf = NULL;
                int k, verdict;

                round++;
                wdog("cases", pause_s + 60);
                set_bright(from);
                usleep(1200000);
                for (k = 0; k < 6; k++) (void)frame_luma();
                y_from = frame_luma();
                usleep(1500000);                     /* no reader: the capture buffers fill with the "from" picture */
                set_bright(to);
                usleep((unsigned)pause_s * 1000000u);
                t_req = now_us();
                if (mode == 0) {
                    G(IMPFrameInfo, fi);
                    buf = malloc(nv12 + 4096);
                    if (buf && IMP_FrameSource_SnapFrame && IMP_FrameSource_SnapFrame(1, PIX_FMT_NV12, SUB_W, SUB_H, buf, fi) == 0) {
                        y_first = luma_nv12(buf, SUB_W, SUB_H); ts_first = fi->timeStamp; ok_snap = 1;
                    } else y_first = -1;
                    gfree(fi); free(buf);
                } else {
                    y_first = frame_luma();
                    ok_snap = y_first >= 0;
                }
                t_ret = now_us();
                lat_ms = (t_ret - t_req) / 1000.0;
                for (k = 0; k < 6; k++) (void)frame_luma();
                y_to = frame_luma();                 /* settled picture with the new brightness */
                y_after = frame_luma();
                (void)y_after; (void)ts_first;
                diff = dabs(y_to - y_from);
                d_to = dabs(y_first - y_to); d_from = dabs(y_first - y_from);
                runs++;
                if (!ok_snap || y_from < 0 || y_to < 0) verdict = 0;
                else if (diff < 15) verdict = 2;     /* brightness step not visible: no statement */
                else verdict = d_to < diff * 0.25 && d_from > diff * 0.5;
                if (verdict == 0) fails++;
                rep(mode == 0 ? LBL("stalesnap SnapFrame") : LBL("stalesnap GetFrame"), 0, verdict == 0 ? V_FAIL : V_PASS,
                    "%s%ds pause, brightness %u->%u: luma old %.1f new %.1f, first frame after the pause %.1f (%s), %.0f ms for the call",
                    verdict == 2 ? "rt: step too small, " : "", pause_s, from, to, y_from, y_to, y_first,
                    !ok_snap ? "no frame" : verdict == 0 ? "STALE or wrong" : "fresh", lat_ms);
            }
        }
        set_bright(ob);
        (void)fails; (void)runs;
    }
}
#else
static void cs_stalesnap(void) {}
#endif

/* ---- dmic: IMP_DMIC_* at API level (T31/T40/T41).  Round trips and argument validation need no DMIC hardware;
 * the stream functions are exercised for real only when Enable succeeds (a DMIC board), otherwise they must
 * fail cleanly (-1, no hang) like the vendor library without the device.  Nothing is played; capture only. */
#if defined(HAS_IMP_DMIC_SetUserInfo) && defined(HAS_IMP_DMIC_Enable) && defined(HAS_IMP_DMIC_PollingFrame)
#define DM_REP(f, r, ok, ...) do { char dm_[200]; snprintf(dm_, sizeof dm_, __VA_ARGS__); \
    rep(FN(f), (long)(r), (ok) ? V_PASS : V_FAIL, "rt: API round trip, no DMIC hardware on the test board; %s", dm_); } while (0)

void t_dmic(void)
{
    G(IMPDmicAttr, at); G(IMPDmicAttr, ag); G(IMPDmicChnParam, cp); G(IMPDmicChnParam, cg);
    G(IMPDmicChnFrame, fr);
    int *iv = (int *)gnew(sizeof(int));
    int r, ok, hw, v, gain_ok = 1;
    long t0;

    if (!IMP_DMIC_Enable) { rep_na(FN(IMP_DMIC_Enable)); return; }
    wdog("cases", 90);
    /* --- SetUserInfo */
    ok = IMP_DMIC_SetUserInfo(1, 0, 0) == -1 && IMP_DMIC_SetUserInfo(0, 4, 1) == -1 && IMP_DMIC_SetUserInfo(0, -1, 1) == -1;
    r = IMP_DMIC_SetUserInfo(0, 2, 1);
    ok = ok && r == 0 && IMP_DMIC_SetUserInfo(0, 0, 0) == 0;
    DM_REP(IMP_DMIC_SetUserInfo, r, ok, "dev 1 / aec id 4 / aec id -1 rejected, (0,2,1) and (0,0,0) accepted");

    /* --- SetPubAttr / GetPubAttr */
    memset(at, 0, sizeof *at);
    at->samplerate = (IMPDmicSampleRate)16000; at->bitwidth = DMIC_BIT_WIDTH_16; at->soundmode = DMIC_SOUND_MODE_MONO;
    at->frmNum = 4; at->numPerFrm = 160; at->chnCnt = 2;
    ok = IMP_DMIC_SetPubAttr(0, NULL) == -1 && IMP_DMIC_SetPubAttr(1, at) == -1;
    at->chnCnt = 0; ok = ok && IMP_DMIC_SetPubAttr(0, at) == -1;
    at->chnCnt = 5; ok = ok && IMP_DMIC_SetPubAttr(0, at) == -1;
    at->chnCnt = 2; at->numPerFrm = 100; ok = ok && IMP_DMIC_SetPubAttr(0, at) == -1;   /* 6.25 ms: not a multiple of 10 ms */
    at->numPerFrm = 160;
    r = IMP_DMIC_SetPubAttr(0, at);
    ok = ok && r == 0;
    DM_REP(IMP_DMIC_SetPubAttr, r, ok, "NULL / dev 1 / chnCnt 0 and 5 / 6.25 ms frame rejected, 16 kHz 160x2 accepted");
    ok = IMP_DMIC_GetPubAttr(0, NULL) == -1 && IMP_DMIC_GetPubAttr(1, ag) == -1;
    r = IMP_DMIC_GetPubAttr(0, ag);
    ok = ok && r == 0 && ag->samplerate == at->samplerate && ag->bitwidth == at->bitwidth && ag->soundmode == at->soundmode &&
         ag->frmNum == at->frmNum && ag->numPerFrm == at->numPerFrm && ag->chnCnt == at->chnCnt;
    { int rate; for (rate = 8000; rate <= 48000; rate *= 2) { if (rate == 32000) rate = 48000;
        at->samplerate = (IMPDmicSampleRate)rate; at->numPerFrm = rate / 50;       /* 20 ms */
        ok = ok && IMP_DMIC_SetPubAttr(0, at) == 0 && IMP_DMIC_GetPubAttr(0, ag) == 0 && (int)ag->samplerate == rate && ag->numPerFrm == rate / 50; } }
    at->samplerate = (IMPDmicSampleRate)16000; at->numPerFrm = 160; at->chnCnt = 2;
    ok = ok && IMP_DMIC_SetPubAttr(0, at) == 0;
    DM_REP(IMP_DMIC_GetPubAttr, r, ok, "all fields read back; 8000/16000/48000 Hz round trip");

    /* --- SetChnParam / GetChnParam (before Enable) */
    cp->usrFrmDepth = 3; cp->Rev = 0;
    ok = IMP_DMIC_SetChnParam(0, 0, NULL) == -1 && IMP_DMIC_SetChnParam(1, 0, cp) == -1 && IMP_DMIC_SetChnParam(0, 1, cp) == -1;
    cp->usrFrmDepth = 1; ok = ok && IMP_DMIC_SetChnParam(0, 0, cp) == -1;
    cp->usrFrmDepth = at->frmNum + 1; ok = ok && IMP_DMIC_SetChnParam(0, 0, cp) == -1;
    cp->usrFrmDepth = 3;
    r = IMP_DMIC_SetChnParam(0, 0, cp);
    ok = ok && r == 0;
    DM_REP(IMP_DMIC_SetChnParam, r, ok, "NULL / dev 1 / chn 1 / depth 1 and frmNum+1 rejected, depth 3 accepted");
    r = IMP_DMIC_GetChnParam(0, 0, cg);      /* the channel parameters are readable once the device is enabled */
    ok = IMP_DMIC_GetChnParam(0, 0, NULL) == -1 && IMP_DMIC_GetChnParam(1, 0, cg) == -1 && r == -1;
    DM_REP(IMP_DMIC_GetChnParam, r, ok, "NULL / dev 1 rejected, -1 while the device is not enabled (checked again after Enable)");

    /* --- volume (software, no device needed) */
    ok = IMP_DMIC_SetVol(1, 0, 60) == -1 && IMP_DMIC_SetVol(0, 1, 60) == -1;
    r = IMP_DMIC_SetVol(0, 0, -100);
    ok = ok && r == 0 && IMP_DMIC_GetVol(0, 0, iv) == 0 && *iv == -30;
    ok = ok && IMP_DMIC_SetVol(0, 0, 200) == 0 && IMP_DMIC_GetVol(0, 0, iv) == 0 && *iv == 120;
    ok = ok && IMP_DMIC_SetVol(0, 0, 60) == 0 && IMP_DMIC_GetVol(0, 0, iv) == 0 && *iv == 60;
    DM_REP(IMP_DMIC_SetVol, r, ok, "-100 -> -30, 200 -> 120, 60 -> 60 (clamp -30..120), dev 1 / chn 1 rejected");
    ok = IMP_DMIC_GetVol(0, 0, NULL) == -1 && IMP_DMIC_GetVol(1, 0, iv) == -1;
    r = IMP_DMIC_GetVol(0, 0, iv);
    ok = ok && r == 0 && *iv == 60;
    DM_REP(IMP_DMIC_GetVol, r, ok, "NULL / dev 1 rejected, value %d read back", *iv);

    /* --- calls that need the enabled device must fail cleanly before Enable */
    ok = IMP_DMIC_EnableChn(0, 0) == -1 && IMP_DMIC_DisableChn(0, 0) == -1;
    ok = ok && IMP_DMIC_GetFrame(0, 0, fr, NOBLOCK) == -1 && IMP_DMIC_ReleaseFrame(0, 0, fr) == -1;
    ok = ok && IMP_DMIC_EnableAec(0, 0, 0, 0) == -1 && IMP_DMIC_DisableAec(0, 0) == -1;
    ok = ok && IMP_DMIC_Enable(1) == -1 && IMP_DMIC_Disable(1) == -1 && IMP_DMIC_EnableChn(1, 0) == -1;
    ok = ok && IMP_DMIC_SetGain(0, 0, 10) == -1 && IMP_DMIC_GetGain(0, 0, NULL) == -1;
    t0 = (long)(now_us() / 1000);
    r = IMP_DMIC_PollingFrame(0, 0, 300);       /* libimp polls up to 5 s; this one honours the timeout */
    ok = ok && r != 0 && (long)(now_us() / 1000) - t0 < 4000;
    DM_REP(IMP_DMIC_PollingFrame, r, ok, "not enabled: -1 (%ld ms), dev 1 and chn 1 rejected", (long)(now_us() / 1000) - t0);
    DM_REP(IMP_DMIC_ReleaseFrame, -1, ok, "not enabled: -1");
    DM_REP(IMP_DMIC_GetFrame, -1, ok, "not enabled: -1, no hang");
    DM_REP(IMP_DMIC_EnableChn, -1, ok, "not enabled: -1");
    DM_REP(IMP_DMIC_DisableChn, -1, ok, "not enabled: -1");
    DM_REP(IMP_DMIC_EnableAec, -1, ok, "not enabled: -1");
    DM_REP(IMP_DMIC_DisableAec, -1, ok, "not enabled: -1");
#ifdef HAS_IMP_DMIC_GetFrameAndRef
    { G(IMPDmicFrame, rf); r = IMP_DMIC_GetFrameAndRef(0, 0, fr, rf, NOBLOCK); DM_REP(IMP_DMIC_GetFrameAndRef, r, r == -1, "not enabled: -1"); gfree(rf); }
#endif
    r = IMP_DMIC_EnableAecRefFrame(0, 0, 0, 0);
    DM_REP(IMP_DMIC_EnableAecRefFrame, r, r == -1, "not enabled: -1");
#ifdef HAS_IMP_DMIC_DisableAecRefFrame
    NEED(IMP_DMIC_DisableAecRefFrame) { r = IMP_DMIC_DisableAecRefFrame(0, 0, 0, 0); DM_REP(IMP_DMIC_DisableAecRefFrame, r, r == -1, "not enabled: -1"); }
#endif

    /* --- Enable: a DMIC board enables, a board without the device must fail cleanly */
    wdog("cases", 60);
    r = IMP_DMIC_Enable(0);
    hw = r == 0;
    DM_REP(IMP_DMIC_Enable, r, r == 0 || r == -1, "%s", hw ? "enabled (DMIC device present)" : "-1 without the DMIC device, returns immediately");
    ok = 1;
    if (hw) {
        int n, got = 0;
        ok = IMP_DMIC_GetChnParam(0, 0, cg) == 0 && cg->usrFrmDepth == 3;
        ok = ok && IMP_DMIC_SetGain(0, 0, 40) == 0 && IMP_DMIC_GetGain(0, 0, iv) == 0 && *iv == 31;   /* clamped */
        ok = ok && IMP_DMIC_SetGain(0, 0, 5) == 0 && IMP_DMIC_GetGain(0, 0, iv) == 0 && *iv == 5;
        gain_ok = ok;
        ok = ok && IMP_DMIC_EnableChn(0, 0) == 0;
        for (n = 0; ok && n < 5; n++) {
            if (IMP_DMIC_PollingFrame(0, 0, 2000) != 0) { ok = 0; break; }
            if (IMP_DMIC_GetFrame(0, 0, fr, BLOCK) != 0) { ok = 0; break; }
            ok = ok && fr->rawFrame.virAddr && fr->rawFrame.len == 160 * 2 * 2;
            IMP_DMIC_ReleaseFrame(0, 0, fr);
            got++;
        }
        v = IMP_DMIC_DisableChn(0, 0);
        ok = ok && v == 0 && IMP_DMIC_Disable(0) == 0;
        printf("[D] dmic hardware flow: %d frames, %s\n", got, ok ? "ok" : "FAILED");
        if (!ok) rep(LBL("dmic hardware flow"), -1, V_FAIL, "stream flow failed after %d frames", got);
    } else {
        ok = IMP_DMIC_GetFrame(0, 0, fr, NOBLOCK) == -1 && IMP_DMIC_EnableChn(0, 0) == -1 && IMP_DMIC_Disable(0) == 0 &&
             IMP_DMIC_Disable(0) == 0;
        if (!ok) rep(LBL("dmic no-device cleanup"), -1, V_FAIL, "calls after a failed Enable misbehave");
    }
    /* after a (failed) Enable the attributes are intact */
    r = IMP_DMIC_GetPubAttr(0, ag);
    if (r != 0 || ag->samplerate != at->samplerate || ag->chnCnt != at->chnCnt) rep(LBL("dmic attr after enable"), r, V_FAIL, "attributes changed");
    r = IMP_DMIC_SetGain(0, 0, 7);
    DM_REP(IMP_DMIC_SetGain, r, r == -1 && gain_ok, "device not enabled: -1%s", hw ? "; on the DMIC board 40 -> 31 clamped and 5 read back" : "");
    r = IMP_DMIC_GetGain(0, 0, iv);
    DM_REP(IMP_DMIC_GetGain, r, IMP_DMIC_GetGain(0, 0, NULL) == -1 && r == 0, "NULL rejected, stored value %d", *iv);
    DM_REP(IMP_DMIC_Disable, 0, IMP_DMIC_Disable(0) == 0, "Disable on a disabled device: 0 (libimp: success)");
    (void)v;
    gfree(at); gfree(ag); gfree(cp); gfree(cg); gfree(fr); gfree(iv);
}
#else
void t_dmic(void) {}
#endif

/* --------------------------------------------------------------------------------------------------- allocator helpers */
/* libimp's physical-memory allocator exports.  The vendor headers do not declare them (the OEM libimp exports them for the SDK samples), so the
 * case resolves them with dlsym and uses the call forms the OEM binary has (descriptor style: IMP_Alloc(&info, size, tag) returns the virtual
 * address and fills the 0x94 byte descriptor).  A function the libimp does not export is N/A. */
typedef struct { char name[96]; char tag[32]; uint32_t virt, phys, size, flags, pool_id; } at_dmainfo;

#define ALN(f) rep_na(LBL(f))
static void cs_alloc(void)
{
    uintptr_t (*a_alloc)(void *, intptr_t, char *) = (uintptr_t (*)(void *, intptr_t, char *))dsym("IMP_Alloc");
    int (*a_free)(uintptr_t) = (int (*)(uintptr_t))dsym("IMP_Free");
    uintptr_t (*a_sp)(int, char *) = (uintptr_t (*)(int, char *))dsym("IMP_Sp_Alloc");
    void *(*p2v)(uint32_t) = (void *(*)(uint32_t))dsym("IMP_Phys_to_Virt");
    uint32_t (*v2p)(void *) = (uint32_t (*)(void *))dsym("IMP_Virt_to_Phys");
    int (*flush)(void *, uint32_t) = (int (*)(void *, uint32_t))dsym("IMP_FlushCache");
    int (*aset)(uint32_t, uint32_t) = (int (*)(uint32_t, uint32_t))dsym("IMP_Alloc_Set_Attr");
    int (*aget)(uint32_t *, uint32_t *) = (int (*)(uint32_t *, uint32_t *))dsym("IMP_Alloc_Get_Attr");
    int (*adump)(void) = (int (*)(void))dsym("IMP_Alloc_Dump");
    int (*adumpf)(const char *) = (int (*)(const char *))dsym("IMP_Alloc_Dump_To_File");
    uintptr_t (*p_alloc)(int, void *, intptr_t, char *) = (uintptr_t (*)(int, void *, intptr_t, char *))dsym("IMP_PoolAlloc");
    void (*p_free)(void *) = (void (*)(void *))dsym("IMP_PoolFree");
    int (*p_flush)(void *, uint32_t) = (int (*)(void *, uint32_t))dsym("IMP_PoolFlushCache");
    void *(*pp2v)(uint32_t) = (void *(*)(uint32_t))dsym("IMP_PoolPhys_to_Virt");
    uint32_t (*pv2p)(void *) = (uint32_t (*)(void *))dsym("IMP_PoolVirt_to_Phys");
    int (*pset)(uint32_t, uint32_t) = (int (*)(uint32_t, uint32_t))dsym("IMP_PoolAlloc_Set_Attr");
    int (*pget)(uint32_t *, uint32_t *) = (int (*)(uint32_t *, uint32_t *))dsym("IMP_PoolAlloc_Get_Attr");
    int (*pdump)(void) = (int (*)(void))dsym("IMP_PoolAlloc_Dump");
    int (*mp_init)(int, uint32_t, int) = (int (*)(int, uint32_t, int))dsym("IMP_MemPool_InitPool");
    int (*mp_get)(int, void *) = (int (*)(int, void *))dsym("IMP_MemPool_GetById");
    int (*mp_rel)(int) = (int (*)(int))dsym("IMP_MemPool_Release");
    at_dmainfo *d = (at_dmainfo *)gnew(sizeof(at_dmainfo)), *d2 = (at_dmainfo *)gnew(sizeof(at_dmainfo)), *mi = (at_dmainfo *)gnew(sizeof(at_dmainfo));
    uint32_t al = 0, cp = 0, i;
    unsigned char *va = NULL;
    int ok;
    long r;

    /* --- attributes (stored, read back, restored) */
    if (aset && aget) {
        uint32_t a0 = 0, c0 = 0;
        aget(&a0, &c0);
        r = aset(4096, 0); ok = aget(&al, &cp) == 0 && al == 4096 && cp == 0;
        rep(LBL("IMP_Alloc_Set_Attr"), r, (r == 0 && ok) ? V_PASS : V_FAIL, "alignment 4096 / cached written, read back %u/%u", al, cp);
        rep(LBL("IMP_Alloc_Get_Attr"), 0, ok ? V_PASS : V_FAIL, "initial %u/%u, read back after Set %u/%u", a0, c0, al, cp);
        aset(a0, c0);
    } else { ALN("IMP_Alloc_Set_Attr"); ALN("IMP_Alloc_Get_Attr"); }
    /* --- descriptor allocation, address conversion, cache flush, free */
    if (a_alloc && a_free) {
        uintptr_t st = a_alloc(d, 8192, "apitest"), v = (uintptr_t)d->virt;       /* descriptor style: the return value is the status, the addresses are in the descriptor */
        va = (unsigned char *)v;
        ok = st == 0 && v != 0 && d->phys != 0 && d->size >= 8192;
        rep(LBL("IMP_Alloc"), ok ? 0 : -1, ok ? V_PASS : V_FAIL, "8192 bytes: status %ld virt %p phys 0x%x size %u", (long)st, (void *)v, d->phys, d->size);
        if (ok) {
            for (i = 0; i < 8192; i++) va[i] = (unsigned char)(i * 7 + 3);
            if (v2p) { uint32_t ph = v2p(va + 100); r = ph; rep(LBL("IMP_Virt_to_Phys"), 0, ph == d->phys + 100 ? V_PASS : V_FAIL, "virt+100 -> 0x%x (descriptor phys+100 = 0x%x)", ph, d->phys + 100); } else ALN("IMP_Virt_to_Phys");
            if (p2v) { void *pv = p2v(d->phys + 100); rep(LBL("IMP_Phys_to_Virt"), 0, pv == (void *)(va + 100) ? V_PASS : V_FAIL, "phys+100 -> %p (expected %p)", pv, (void *)(va + 100)); } else ALN("IMP_Phys_to_Virt");
            if (flush) {
                r = flush(va, 8192);
                for (i = 0, ok = 1; i < 8192; i++) if (va[i] != (unsigned char)(i * 7 + 3)) { ok = 0; break; }
                rep(LBL("IMP_FlushCache"), r, (r == 0 && ok && flush(NULL, 0) == -1) ? V_PASS : V_FAIL, "8192 bytes flushed, pattern intact (%d), NULL/0 rejected", ok);
            } else ALN("IMP_FlushCache");
            if (adump) { r = adump(); rep(LBL("IMP_Alloc_Dump"), r, r == 0 ? V_PASS : V_FAIL, "registry dump with a live buffer"); } else ALN("IMP_Alloc_Dump");
            if (adumpf) { r = adumpf("/dev/null"); ok = adumpf(NULL) == -1; rep(LBL("IMP_Alloc_Dump_To_File"), r, (r == 0 && ok) ? V_PASS : V_FAIL, "/dev/null accepted, NULL path rejected"); } else ALN("IMP_Alloc_Dump_To_File");
            r = a_free(v);
            rep(LBL("IMP_Free"), r, r == 0 ? V_PASS : V_FAIL, "freed (ret %ld)", r);
        } else if (v) a_free(v);
    } else { ALN("IMP_Alloc"); ALN("IMP_Free"); ALN("IMP_Virt_to_Phys"); ALN("IMP_Phys_to_Virt"); ALN("IMP_FlushCache"); ALN("IMP_Alloc_Dump"); ALN("IMP_Alloc_Dump_To_File"); }
    if (a_sp && a_free) {
        uintptr_t v = a_sp(4096, "apitest");
        if (v) memset((void *)v, 0x5a, 4096);
        r = v ? a_free(v) : -1;
        rep(LBL("IMP_Sp_Alloc"), r, (v != 0 && r == 0) ? V_PASS : V_FAIL, "4096 bytes at %p written and freed", (void *)v);
    } else ALN("IMP_Sp_Alloc");
    /* --- pools */
    if (pset && pget) {
        uint32_t a0 = 0, c0 = 0;
        pget(&a0, &c0);
        r = pset(4096, 0); ok = pget(&al, &cp) == 0 && al == 4096 && cp == 0;
        rep(LBL("IMP_PoolAlloc_Set_Attr"), r, (r == 0 && ok) ? V_PASS : V_FAIL, "alignment 4096 / cached written, read back %u/%u", al, cp);
        rep(LBL("IMP_PoolAlloc_Get_Attr"), 0, ok ? V_PASS : V_FAIL, "initial %u/%u, read back after Set %u/%u", a0, c0, al, cp);
        pset(a0, c0);
    } else { ALN("IMP_PoolAlloc_Set_Attr"); ALN("IMP_PoolAlloc_Get_Attr"); }
    if (mp_init && mp_get) {
        r = mp_init(9, 65536, 0);
        ok = r == 0 && mp_get(9, mi) == 0 && mi->phys != 0 && mi->size == 65536 && mp_get(9 + 20, mi) != 0;
        rep(LBL("IMP_MemPool_InitPool"), r, ok ? V_PASS : V_FAIL, "pool 9, 64 KB: phys 0x%x size %u; a second init of the same id returns %d", mi->phys, mi->size, mp_init(9, 65536, 0));
        rep(LBL("IMP_MemPool_GetById"), 0, ok ? V_PASS : V_FAIL, "pool 9 info read back, unknown id rejected");
        if (p_alloc && p_free) {
            uintptr_t st = p_alloc(9, d2, 4096, "apitest"), v = (uintptr_t)d2->virt;
            ok = st == 0 && v != 0 && d2->phys != 0;
            rep(LBL("IMP_PoolAlloc"), ok ? 0 : -1, ok ? V_PASS : V_FAIL, "pool 9, 4096 bytes: status %ld virt %p phys 0x%x", (long)st, (void *)v, d2->phys);
            if (ok) {
                memset((void *)v, 0xa5, 4096);
                if (pv2p) rep(LBL("IMP_PoolVirt_to_Phys"), 0, pv2p((void *)(v + 64)) == d2->phys + 64 ? V_PASS : V_FAIL, "virt+64 -> 0x%x (expected 0x%x)", pv2p((void *)(v + 64)), d2->phys + 64); else ALN("IMP_PoolVirt_to_Phys");
                if (pp2v) rep(LBL("IMP_PoolPhys_to_Virt"), 0, pp2v(d2->phys + 64) == (void *)(v + 64) ? V_PASS : V_FAIL, "phys+64 -> %p (expected %p)", pp2v(d2->phys + 64), (void *)(v + 64)); else ALN("IMP_PoolPhys_to_Virt");
                if (p_flush) { r = p_flush((void *)v, 4096); rep(LBL("IMP_PoolFlushCache"), r, (r == 0 && ((unsigned char *)v)[100] == 0xa5) ? V_PASS : V_FAIL, "4096 bytes flushed, data intact"); } else ALN("IMP_PoolFlushCache");
                if (pdump) { r = pdump(); rep(LBL("IMP_PoolAlloc_Dump"), r, r == 0 ? V_PASS : V_FAIL, "pool dump with a live pool buffer"); } else ALN("IMP_PoolAlloc_Dump");
                p_free((void *)v);
                rep(LBL("IMP_PoolFree"), 0, V_PASS, "returned (void); the process continues");
            }
        } else { ALN("IMP_PoolAlloc"); ALN("IMP_PoolFree"); ALN("IMP_PoolVirt_to_Phys"); ALN("IMP_PoolPhys_to_Virt"); ALN("IMP_PoolFlushCache"); ALN("IMP_PoolAlloc_Dump"); }
        if (mp_rel) mp_rel(9);
    } else { ALN("IMP_MemPool_InitPool"); ALN("IMP_MemPool_GetById"); }
    gfree(d); gfree(d2); gfree(mi);
}

/* ---------------------------------------------------------------------------------------------- T41/T40 ISP-OSD API family
 * IMP_OSD_*_ISP (isp_osd.h, not part of the generated have/weak lists), IMP_ISP_Tuning_GetOsdRgnAttr, T41 Get/SetOSDAttr + Get/SetOSDBlock,
 * T40 Get/SetOSDAttr + Get/SetSingleOSDAttr.  Pass criterion = the picture: shown -> region luma moves, hidden -> back to the base. */
#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetOsdRgnAttr && HAS_IMP_ISP_Tuning_CreateOsdRgn && HAS_IMP_ISP_Tuning_SetOsdRgnAttr && HAS_IMP_ISP_Tuning_ShowOsdRgn && HAS_IMP_ISP_Tuning_DestroyOsdRgn && HAS_IMP_ISP_Tuning_SetOsdPoolSize
extern int IMP_OSD_SetPoolSize_ISP(int size);
extern int IMP_OSD_CreateRgn_ISP(int chn, IMPIspOsdAttrAsm *a);
extern int IMP_OSD_SetRgnAttr_PicISP(int chn, int handle, IMPIspOsdAttrAsm *a);
extern int IMP_OSD_GetRgnAttr_ISPPic(int chn, int handle, IMPIspOsdAttrAsm *a);
extern int IMP_OSD_ShowRgn_ISP(int chn, int handle, int show);
extern int IMP_OSD_DestroyRgn_ISP(int chn, int handle);
#pragma weak IMP_OSD_SetPoolSize_ISP
#pragma weak IMP_OSD_CreateRgn_ISP
#pragma weak IMP_OSD_SetRgnAttr_PicISP
#pragma weak IMP_OSD_GetRgnAttr_ISPPic
#pragma weak IMP_OSD_ShowRgn_ISP
#pragma weak IMP_OSD_DestroyRgn_ISP

enum { OPW = 128, OPH = 64 };

static void isposd_fill(IMPIspOsdAttrAsm *a, int h, int x, int y, uint8_t *img)
{
    memset(a, 0, sizeof(*a));
    a->type = ISP_OSD_REG_PIC;
# ifdef APITEST_T40
    a->stsinglepicAttr.chx = 0; a->stsinglepicAttr.pic_num = h;
    a->stsinglepicAttr.osd_type = IMP_ISP_PIC_ARGB_8888; a->stsinglepicAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_BGRA;
    a->stsinglepicAttr.osd_pixel_alpha_disable = IMPISP_TUNING_OPS_MODE_DISABLE;
# else
    a->stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888; a->stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_ARGB;
    a->stsinglepicAttr.chnOSDAttr.osd_pixel_alpha_disable = IMPISP_TUNING_OPS_MODE_DISABLE;
    a->stsinglepicAttr.pic.pinum = (uint8_t)h;
# endif
    a->stsinglepicAttr.pic.osd_enable = 1; a->stsinglepicAttr.pic.osd_left = x; a->stsinglepicAttr.pic.osd_top = y;
    a->stsinglepicAttr.pic.osd_width = OPW; a->stsinglepicAttr.pic.osd_height = OPH;
    a->stsinglepicAttr.pic.osd_image = (char *)img; a->stsinglepicAttr.pic.osd_stride = OPW * 4;
}

static int isposd_geom(const IMPIspOsdAttrAsm *a, int x, int y)
{
    return a->type == ISP_OSD_REG_PIC && a->stsinglepicAttr.pic.osd_left == x && a->stsinglepicAttr.pic.osd_top == y && a->stsinglepicAttr.pic.osd_width == OPW
        && a->stsinglepicAttr.pic.osd_height == OPH && a->stsinglepicAttr.pic.osd_stride == OPW * 4;
}

/* a JPEG encoder on FS ch0 as the consumer of the ISP-drawn output (the OSD is committed per frame of MSCA output 0) */
static int isposd_cons_up(void)
{
    IMPCell fs0 = { DEV_ID_FS, 0, 0 }, en0 = { DEV_ID_ENC, 6, 0 };
    IMPEncoderChnAttr *ea = (IMPEncoderChnAttr *)calloc(1, sizeof(*ea));
    int up = 0;

    if (ea && IMP_Encoder_SetDefaultParam && IMP_Encoder_CreateGroup(6) == 0) {
        IMP_Encoder_SetDefaultParam(ea, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP, g_sw, g_sh, 15, 1, 0, 0, 30, 0);
        if (IMP_Encoder_CreateChn(6, ea) == 0 && IMP_Encoder_RegisterChn(6, 6) == 0 && IMP_System_Bind(&fs0, &en0) == 0) { up = 1; IMP_Encoder_StartRecvPic(6); }
    }
    free(ea);
    return up;
}

static void isposd_cons_down(int up)
{
    IMPCell fs0 = { DEV_ID_FS, 0, 0 }, en0 = { DEV_ID_ENC, 6, 0 };

    if (!up) return;
    IMP_Encoder_StopRecvPic(6); IMP_System_UnBind(&fs0, &en0); IMP_Encoder_UnRegisterChn(6); IMP_Encoder_DestroyChn(6); IMP_Encoder_DestroyGroup(6);
}

static void cs_isposdapi(void)
{
    NEED(IMP_OSD_CreateRgn_ISP) NEED(IMP_OSD_SetRgnAttr_PicISP) NEED(IMP_OSD_GetRgnAttr_ISPPic) NEED(IMP_OSD_ShowRgn_ISP) NEED(IMP_OSD_DestroyRgn_ISP) NEED(IMP_OSD_SetPoolSize_ISP) NEED(IMP_ISP_Tuning_GetOsdRgnAttr) {
        uint8_t *pic = (uint8_t *)malloc(OPW * OPH * 4);
        IMPIspOsdAttrAsm *a = (IMPIspOsdAttrAsm *)calloc(1, sizeof(*a)), *b = (IMPIspOsdAttrAsm *)calloc(1, sizeof(*a)), *c = (IMPIspOsdAttrAsm *)calloc(1, sizeof(*a));
        int x0 = getenv("APITEST_OSD_X") ? atoi(getenv("APITEST_OSD_X")) : 64, y0 = getenv("APITEST_OSD_Y") ? atoi(getenv("APITEST_OSD_Y")) : 64;
        int cx0 = x0 + 4, cy0 = y0 + 4, cx1 = x0 + OPW - 4, cy1 = y0 + OPH - 4;
        int rp, rbad, h, h2, rs, rg, rg3, rsh, rhide, rd, rd2, k, jenc, dark, g_ok, c_ok, appears, gone;
        double base, shown, hidden;
        /* T40: the ISP OSD controls 0x080000a1 / 0x080000a2 (Show, Get/SetOSDAttr, Get/SetSingleOSDAttr) oops tx_isp_t40 in our QEMU guest (kernel paging request);
         * by the QEMU-first rule they do not run on the Eufy: only with APITEST_T40_ISPOSD_CTL=1 */
        int show =
# ifdef APITEST_T40
            getenv("APITEST_T40_ISPOSD_CTL") != NULL;
# else
            1;
# endif
        void (*oexit)(void) = (void (*)(void))dsym("IMP_OSD_Exit_ISP");
        int (*oinit)(void) = (int (*)(void))dsym("IMP_OSD_Init_ISP");

        if (!pic || !a || !b || !c || cx1 <= cx0 + 2 || cy1 <= cy0 + 2) { rep(FN(IMP_OSD_CreateRgn_ISP), 0, V_SKIP, "no memory or sensor too small"); goto out; }
        base = region_y(cx0, cy0, cx1, cy1);
        dark = base < 140;
        for (k = 0; k < OPW * OPH; k++) { uint8_t v = dark ? 255 : 0; pic[k * 4] = v; pic[k * 4 + 1] = v; pic[k * 4 + 2] = v; pic[k * 4 + 3] = 255; }
        jenc = isposd_cons_up();
        if (oinit) oinit();
        rp = IMP_OSD_SetPoolSize_ISP(512 * 1024);
        rep(FN(IMP_OSD_SetPoolSize_ISP), rp, rp == 0 ? V_PASS : V_FAIL, "512 KB");
        rbad = IMP_OSD_CreateRgn_ISP(99, NULL);
        h = IMP_OSD_CreateRgn_ISP(0, NULL); h2 = IMP_OSD_CreateRgn_ISP(0, NULL);
        rep(FN(IMP_OSD_CreateRgn_ISP), h, (h >= 0 && h2 >= 0 && h2 != h && rbad < 0) ? V_PASS : V_FAIL, "handles %d and %d (distinct), channel 99 refused (ret %d)", h, h2, rbad);
        if (h < 0) { isposd_cons_down(jenc); goto out; }
        isposd_fill(a, h, x0, y0, pic);
        rs = IMP_OSD_SetRgnAttr_PicISP(0, h, a);
        rg = IMP_OSD_GetRgnAttr_ISPPic(0, h, b);
        g_ok = rg == 0 && isposd_geom(b, x0, y0);
        rsh = show ? IMP_OSD_ShowRgn_ISP(0, h, 1) : 0;
        shown = show ? region_y(cx0, cy0, cx1, cy1) : base;
        rg3 = IMP_ISP_Tuning_GetOsdRgnAttr(0, h, c);
        c_ok = rg3 == 0 && isposd_geom(c, x0, y0) && c->stsinglepicAttr.pic.osd_enable == (show ? 1 : 0);
        appears = !show || (dark ? shown > base + 40 : shown < base - 40);
        rep(FN(IMP_OSD_SetRgnAttr_PicISP), rs, (rs == 0 && appears) ? V_PASS : V_FAIL, "%s %dx%d picture at (%d,%d) through IMP_OSD_*_ISP: %s", dark ? "white" : "black", OPW, OPH, x0, y0, show ? (appears ? "visible after Show" : "NOT VISIBLE after Show") : "stored (rt: not shown, see APITEST_T40_ISPOSD_CTL)");
        rep(FN(IMP_OSD_GetRgnAttr_ISPPic), rg, g_ok ? V_PASS : V_FAIL, "stored attr: type %d, %ux%u at (%u,%u) stride %u", (int)b->type, b->stsinglepicAttr.pic.osd_width, b->stsinglepicAttr.pic.osd_height, b->stsinglepicAttr.pic.osd_left, b->stsinglepicAttr.pic.osd_top, b->stsinglepicAttr.pic.osd_stride);
        rep(FN(IMP_ISP_Tuning_GetOsdRgnAttr), rg3, c_ok ? V_PASS : V_FAIL, "%s: geometry %s, osd_enable %d (expected %d)", show ? "after Show" : "stored", isposd_geom(c, x0, y0) ? "as set" : "DIFFERENT", (int)c->stsinglepicAttr.pic.osd_enable, show ? 1 : 0);
# ifdef APITEST_T40
        {
            /* T40 direct controls: the shown region as the driver holds it, hidden/shown again through SetSingleOSDAttr, whole-ISP OSD attr round trip */
            IMPISPSingleOSDAttr *s = (IMPISPSingleOSDAttr *)calloc(1, sizeof(*s)), *s2 = (IMPISPSingleOSDAttr *)calloc(1, sizeof(*s));
            int rgs, rss0, rss1;
            double off, on;

            if (s && s2) {
                s->chx = 0; s->pic_num = h;
                rgs = show ? IMP_ISP_GetSingleOSDAttr(IMPVI_MAIN, s) : 0;
                *s2 = *s; s2->pic.osd_enable = 0;
                rss0 = show ? IMP_ISP_SetSingleOSDAttr(IMPVI_MAIN, s2) : 0; off = show ? region_y(cx0, cy0, cx1, cy1) : base;
                rss1 = show ? IMP_ISP_SetSingleOSDAttr(IMPVI_MAIN, s) : 0; on = show ? region_y(cx0, cy0, cx1, cy1) : (dark ? base + 100 : base - 100);
                if (show) rep(FN(IMP_ISP_GetSingleOSDAttr), rgs, (rgs == 0 && s->chx == 0 && s->pic_num == h && (!show || (s->pic.osd_enable == 1 && s->pic.osd_left == x0 && s->pic.osd_top == y0 && s->pic.osd_width == OPW))) ? V_PASS : V_FAIL,
                    "driver holds chx %d pic %d: enable %u at (%u,%u) %ux%u", s->chx, s->pic_num, s->pic.osd_enable, s->pic.osd_left, s->pic.osd_top, s->pic.osd_width, s->pic.osd_height);
                if (show) rep(FN(IMP_ISP_SetSingleOSDAttr), rss0 | rss1, (rss0 == 0 && rss1 == 0 && dabs(off - base) < 20 && (dark ? on > base + 40 : on < base - 40)) ? V_PASS : V_FAIL,
                    "enable 0 -> region luma %.1f (base %.1f), enable 1 -> %.1f", off, base, on);
                else rep(FN(IMP_ISP_SetSingleOSDAttr), 0, V_SKIP, "APITEST_T40_ISPOSD_CTL not set (QEMU oops of the ISP OSD control)");
                if (!show) rep(FN(IMP_ISP_GetSingleOSDAttr), 0, V_SKIP, "APITEST_T40_ISPOSD_CTL not set (QEMU oops of control 0x080000a2)");
                /* IMP_ISP_Get/SetOSDAttr: own round trip after the pool regions are destroyed, see below */
            }
            free(s); free(s2);
        }
# else
        {
            /* T41: per-channel OSD attr and block (control 0x080000a1 / a2), written back as read */
            IMPISPOSDAttr *oa = (IMPISPOSDAttr *)calloc(1, sizeof(*oa)), *ob = (IMPISPOSDAttr *)calloc(1, sizeof(*oa));
            IMPISPOSDBlockAttr *ba = (IMPISPOSDBlockAttr *)calloc(1, sizeof(*ba)), *bb = (IMPISPOSDBlockAttr *)calloc(1, sizeof(*ba));
            int ra, rsa, rb, rsb, rb2, ra2;
            double again, again_a, off_b;

            if (oa && ob && ba && bb) {
                ra = IMP_ISP_Tuning_GetOSDAttr(IMPVI_MAIN, oa);
                rb = IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, ba);
                rsa = IMP_ISP_Tuning_SetOSDAttr(IMPVI_MAIN, oa);
                again_a = region_y(cx0, cy0, cx1, cy1);
                /* the driver returns the picture address as a KSEG0 virtual address, the control takes the physical one */
                ba->osd_image = (char *)((uintptr_t)ba->osd_image & 0x1fffffffu);
                *bb = *ba; bb->osd_enable = 0;
                rsb = IMP_ISP_Tuning_SetOSDBlock(IMPVI_MAIN, bb);
                off_b = region_y(cx0, cy0, cx1, cy1);
                rsb |= IMP_ISP_Tuning_SetOSDBlock(IMPVI_MAIN, ba);
                ra2 = IMP_ISP_Tuning_GetOSDAttr(IMPVI_MAIN, ob);
                rb2 = IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, bb);
                again = region_y(cx0, cy0, cx1, cy1);
                rep(FN(IMP_ISP_Tuning_GetOSDAttr), ra | ra2, (ra == 0 && ra2 == 0 && oa->osd_type == IMP_ISP_PIC_ARGB_8888 && oa->osd_argb_type == IMP_ISP_ARGB_TYPE_ARGB) ? V_PASS : V_FAIL,
                    "type %d argb %d alpha_disable %d (region was sent as ARGB8888 / ARGB)", (int)oa->osd_type, (int)oa->osd_argb_type, (int)oa->osd_pixel_alpha_disable);
                rep(FN(IMP_ISP_Tuning_GetOSDBlock), rb | rb2, (rb == 0 && rb2 == 0 && ba->osd_width == OPW && ba->osd_height == OPH && ba->osd_left == x0 && ba->osd_top == y0) ? V_PASS : V_FAIL,
                    "pinum %u enable %u at (%u,%u) %ux%u stride %u", ba->pinum, ba->osd_enable, ba->osd_left, ba->osd_top, ba->osd_width, ba->osd_height, ba->osd_stride);
                rep(FN(IMP_ISP_Tuning_SetOSDAttr), rsa, (rsa == 0 && ob->osd_type == oa->osd_type && ob->osd_argb_type == oa->osd_argb_type && (dark ? again_a > base + 40 : again_a < base - 40)) ? V_PASS : V_FAIL,
                    "rt: attr as read written back (ret %d), read again equal, picture still there: region luma %.1f", rsa, again_a);
                rep(FN(IMP_ISP_Tuning_SetOSDBlock), rsb, (rsb == 0 && bb->osd_left == ba->osd_left && bb->osd_top == ba->osd_top && bb->osd_width == ba->osd_width && dabs(off_b - base) < 20 && (dark ? again > base + 40 : again < base - 40)) ? V_PASS : V_FAIL,
                    "block with osd_enable 0: region luma %.1f (base %.1f); block as read (image %p) written back: region luma %.1f (ret %d), read again equal", off_b, base, (void *)ba->osd_image, again, rsb);
            }
            free(oa); free(ob); free(ba); free(bb);
        }
# endif
        rhide = show ? IMP_OSD_ShowRgn_ISP(0, h, 0) : 0;
        hidden = show ? region_y(cx0, cy0, cx1, cy1) : base;
        gone = dabs(hidden - base) < 20;
        rd = IMP_OSD_DestroyRgn_ISP(0, h); rd2 = IMP_OSD_DestroyRgn_ISP(0, h2);
        if (!show) rep(FN(IMP_OSD_ShowRgn_ISP), 0, V_SKIP, "APITEST_T40_ISPOSD_CTL not set: Show and Get/SetSingleOSDAttr (control 0x080000a2) oops tx_isp_t40 in QEMU, not run on the Eufy"); else rep(FN(IMP_OSD_ShowRgn_ISP), rsh | rhide, (rsh == 0 && rhide == 0 && gone) ? V_PASS : V_FAIL, "show ret %d, hide ret %d, hidden: region luma %.1f (base %.1f)", rsh, rhide, hidden, base);
        rep(FN(IMP_OSD_DestroyRgn_ISP), rd | rd2, (rd == 0 && rd2 == 0 && flow()) ? V_PASS : V_FAIL, "handles %d and %d destroyed, frames flow", h, h2);
# ifdef APITEST_T40
        if (!show) { rep(FN(IMP_ISP_GetOSDAttr), 0, V_SKIP, "APITEST_T40_ISPOSD_CTL not set (control 0x080000a1)"); rep(FN(IMP_ISP_SetOSDAttr), 0, V_SKIP, "APITEST_T40_ISPOSD_CTL not set (control 0x080000a1)"); }
        else {
            /* IMP_ISP_Get/SetOSDAttr (control 0x080000a1, whole-ISP OSD attr): the driver (vendor tisp_s_osd_attr, same in
             * ours) kfree's the image of every slot it rewrites or clears and copies the user image (file path or pixel
             * array, osd_image is read as a user pointer) into a kernel buffer; GetOSDAttr returns that buffer's physical
             * address and no image type.  So the attr cannot be written back "as read" (strcpy/memcpy of a physical
             * address: oops), and SetOSDAttr must not run while an IMP_OSD_*_ISP region holds its pool pointer in a slot
             * (kfree of non-slab memory).  Round trip on slot h after the pool regions are destroyed: array image in,
             * read back, cleared again. */
            IMPISPOSDAttr *o1 = (IMPISPOSDAttr *)calloc(1, sizeof(*o1)), *o2 = (IMPISPOSDAttr *)calloc(1, sizeof(*o1)), *o3 = (IMPISPOSDAttr *)calloc(1, sizeof(*o1));
            int ro1, ro2, ro3, ro4, ro5, same = 0, cleared = 0, free_slot = 0;
            double on_a = base, off_a = base;

            if (o1 && o2 && o3) {
                IMPISPOSDPicAttr *pa = &o2->osd_chx[0].pic[h], *pb = &o3->osd_chx[0].pic[h];

                ro1 = IMP_ISP_GetOSDAttr(IMPVI_MAIN, o1);
                free_slot = o1->osd_chx[0].pic[h].osd_enable == 0;
                o2->osd_chx[0].osd_type = IMP_ISP_PIC_ARGB_8888; o2->osd_chx[0].osd_argb_type = IMP_ISP_ARGB_TYPE_ARGB;
                pa->osd_enable = 1; pa->osd_left = x0; pa->osd_top = y0; pa->osd_width = OPW; pa->osd_height = OPH;
                pa->osd_image = (char *)pic; pa->osd_image_type = IMP_ISP_OSD_PIC_ARRAY; pa->osd_stride = OPW * 4;
                ro2 = IMP_ISP_SetOSDAttr(IMPVI_MAIN, o2);
                on_a = region_y(cx0, cy0, cx1, cy1);
                ro3 = IMP_ISP_GetOSDAttr(IMPVI_MAIN, o3);
                same = pb->osd_enable == 1 && pb->osd_left == x0 && pb->osd_top == y0 && pb->osd_width == OPW && pb->osd_height == OPH && pb->osd_stride == OPW * 4
                    && o3->osd_chx[0].osd_type == o2->osd_chx[0].osd_type && o3->osd_chx[0].osd_argb_type == o2->osd_chx[0].osd_argb_type;
                memset(o2, 0, sizeof(*o2));
                ro4 = IMP_ISP_SetOSDAttr(IMPVI_MAIN, o2);
                off_a = region_y(cx0, cy0, cx1, cy1);
                ro5 = IMP_ISP_GetOSDAttr(IMPVI_MAIN, o1);
                cleared = o1->osd_chx[0].pic[h].osd_enable == 0 && o1->osd_chx[0].pic[h].osd_width == 0;
                rep(FN(IMP_ISP_GetOSDAttr), ro1 | ro3 | ro5, (ro1 == 0 && ro3 == 0 && ro5 == 0 && free_slot && same && cleared) ? V_PASS : V_FAIL,
                    "slot %d free after Destroy: %s; after Set: enable %u at (%u,%u) %ux%u stride %u type %d argb %d (%s); after clear: %s",
                    h, free_slot ? "yes" : "NO", pb->osd_enable, pb->osd_left, pb->osd_top, pb->osd_width, pb->osd_height, pb->osd_stride,
                    (int)o3->osd_chx[0].osd_type, (int)o3->osd_chx[0].osd_argb_type, same ? "as set" : "DIFFERENT", cleared ? "cleared" : "NOT cleared");
                rep(FN(IMP_ISP_SetOSDAttr), ro2 | ro4, (ro2 == 0 && ro4 == 0 && (dark ? on_a > base + 40 : on_a < base - 40) && dabs(off_a - base) < 20) ? V_PASS : V_FAIL,
                    "%dx%d array image into chx0 pic %d (ret %d): region luma %.1f (base %.1f); all slots cleared (ret %d): %.1f", OPW, OPH, h, ro2, on_a, base, ro4, off_a);
            }
            free(o1); free(o2); free(o3);
        }
# endif
        isposd_cons_down(jenc);
        if (oexit) oexit();
out:
        free(pic); free(a); free(b); free(c);
    }
}
#else
static void cs_isposdapi(void) {}
#endif

/* ---------------------------------------------------------------------------------------------- T41 misc ISP calls (backlog T41) */
#if defined(PLATFORM_T41) && !defined(APITEST_T40)
/* GetAEEvList: declared in the 1.2.5/1.2.6 English header only (the build uses the zh one): own prototype */
typedef struct { IMPISPTuningOpsMode enable; uint64_t ev[15]; } AtAeEvList;
extern int32_t IMP_ISP_Tuning_GetAEEvList(IMPVI_NUM num, AtAeEvList *attr);
# pragma weak IMP_ISP_Tuning_GetAEEvList

static void cs_aeevlist(void)
{
    NEED(IMP_ISP_Tuning_GetAEEvList) {
        G(AtAeEvList, l); G(AtAeEvList, l2);
        int r = IMP_ISP_Tuning_GetAEEvList(IMPVI_MAIN, l), r2 = IMP_ISP_Tuning_GetAEEvList(IMPVI_MAIN, l2), i, same = 1;

        gchk(l); gchk(l2);
        for (i = 0; i < 15; i++) if (l->ev[i] != l2->ev[i]) same = 0;
        rep(FN(IMP_ISP_Tuning_GetAEEvList), r | r2, (r == 0 && r2 == 0 && (l->enable == 0 || l->enable == 1) && same) ? V_PASS : V_FAIL,
            "enable %d, ev[0..2] = %llu %llu %llu, ev[14] = %llu; second read %s (static table)", (int)l->enable, (unsigned long long)l->ev[0], (unsigned long long)l->ev[1], (unsigned long long)l->ev[2], (unsigned long long)l->ev[14], same ? "equal" : "DIFFERENT");
        gfree(l); gfree(l2);
    }
}
#else
static void cs_aeevlist(void) {}
#endif

#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_GetFaceAeWeiget && HAS_IMP_ISP_Tuning_SetFaceAeWeiget
static void cs_faceaeweight(void)
{
    NEED(IMP_ISP_Tuning_GetFaceAeWeiget) NEED(IMP_ISP_Tuning_SetFaceAeWeiget) {
        G(IMPISPFaceAeWeiget, o); G(IMPISPFaceAeWeiget, n); G(IMPISPFaceAeWeiget, g);
        int r0 = IMP_ISP_Tuning_GetFaceAeWeiget(IMPVI_MAIN, o), r1, r2, r3;

        gchk(o);
        *n = *o; n->global_weight = o->global_weight ^ 1; n->roi_weight = o->roi_weight ^ 2; n->smooth = o->smooth ^ 1;
        r1 = IMP_ISP_Tuning_SetFaceAeWeiget(IMPVI_MAIN, n);
        r2 = IMP_ISP_Tuning_GetFaceAeWeiget(IMPVI_MAIN, g); gchk(g);
        r3 = IMP_ISP_Tuning_SetFaceAeWeiget(IMPVI_MAIN, o);
        rep(FN(IMP_ISP_Tuning_GetFaceAeWeiget), r0 | r2, (r0 == 0 && r2 == 0 && g->global_weight == n->global_weight && g->roi_weight == n->roi_weight && g->smooth == n->smooth) ? V_PASS : V_FAIL,
            "initial global %u roi %u smooth %u; after Set: %u %u %u (set %u %u %u)", o->global_weight, o->roi_weight, o->smooth, g->global_weight, g->roi_weight, g->smooth, n->global_weight, n->roi_weight, n->smooth);
        rep(FN(IMP_ISP_Tuning_SetFaceAeWeiget), r1 | r3, (r1 == 0 && r3 == 0 && flow()) ? V_PASS : V_FAIL, "rt: changed weights read back, initial values restored (ret %d), frames flow", r3);
        gfree(o); gfree(n); gfree(g);
    }
}
#else
static void cs_faceaeweight(void) {}
#endif

#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_SetTmoFaceae
static void cs_tmofaceae(void)
{
    NEED(IMP_ISP_Tuning_SetTmoFaceae) {
        G(IMPISPTmoFaceae, t);
        Fst base, on, back;
        int r1, r2, ms;

        base = fst(800);
        t->tmo_roinum = 1; t->tmo_xcent[0] = (uint16_t)(g_sw / 2); t->tmo_ycent[0] = (uint16_t)(g_sh / 2); t->tmo_distep[0] = (uint16_t)(g_sh / 3);
        t->tmo_uplumthr[0] = 128; t->tmo_upk[0] = 16;
        r1 = IMP_ISP_Tuning_SetTmoFaceae(IMPVI_MAIN, t); on = fst(1500);
        memset(t, 0, sizeof(*t));
        r2 = IMP_ISP_Tuning_SetTmoFaceae(IMPVI_MAIN, t); back = recover(base.y, &ms);
        {
            int eff = on.ok && base.ok && on.y > base.y + 3;
            rep(FN(IMP_ISP_Tuning_SetTmoFaceae), r1 | r2, (r1 == 0 && r2 == 0 && on.ok && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%sone brighten region at the centre: Y %.1f -> %.1f; 0 regions: back to Y %.1f in %d ms, frames flow", RTMSG(eff), base.y, on.y, back.y, ms);
        }
        gfree(t);
    }
}
#else
static void cs_tmofaceae(void) {}
#endif

#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_ISP_Tuning_SetDrawBlock && HAS_IMP_ISP_Tuning_GetDrawBlock
static void cs_drawblock(void)
{
    NEED(IMP_ISP_Tuning_SetDrawBlock) NEED(IMP_ISP_Tuning_GetDrawBlock) {
        G(IMPISPDrawBlockAttr, a); G(IMPISPDrawBlockAttr, g);
        int x = g_sw / 4, y = g_sh / 4, w = g_sw / 2, bw = 24, r1, r2, r3;
        double base, on, off;

        base = region_y(x + 4, y + 2, x + w - 4, y + bw - 2);
        a->pinum = 0; a->type = IMP_ISP_DRAW_WIND; a->color_type = IMPISP_MASK_TYPE_YUV;
        a->cfg.wind.enable = 1; a->cfg.wind.left = x; a->cfg.wind.top = y; a->cfg.wind.width = w; a->cfg.wind.height = g_sh / 2;
        a->cfg.wind.color.ayuv.y_value = base < 140 ? 235 : 16; a->cfg.wind.color.ayuv.u_value = 128; a->cfg.wind.color.ayuv.v_value = 128;
        a->cfg.wind.line_width = bw; a->cfg.wind.alpha = 7;
        r1 = IMP_ISP_Tuning_SetDrawBlock(IMPVI_MAIN, a);
        on = region_y(x + 4, y + 2, x + w - 4, y + bw - 2);
        memset(g, 0, sizeof(*g)); g->pinum = 0; g->type = IMP_ISP_DRAW_WIND;
        r2 = IMP_ISP_Tuning_GetDrawBlock(IMPVI_MAIN, g); gchk(g);
        a->cfg.wind.enable = 0; r3 = IMP_ISP_Tuning_SetDrawBlock(IMPVI_MAIN, a);
        off = region_y(x + 4, y + 2, x + w - 4, y + bw - 2);
        rep(FN(IMP_ISP_Tuning_SetDrawBlock), r1 | r3, (r1 == 0 && r3 == 0 && (base < 140 ? on > base + 30 : on < base - 30) && dabs(off - base) < 20 && flow()) ? V_PASS : V_FAIL,
            "window frame %d px at (%d,%d): top edge luma %.1f -> %.1f; disabled: %.1f (base %.1f); frames flow", bw, x, y, base, on, off, base);
        rep(FN(IMP_ISP_Tuning_GetDrawBlock), r2, (r2 == 0 && g->cfg.wind.enable == 1 && g->cfg.wind.left == x && g->cfg.wind.top == y && g->cfg.wind.width == w && g->cfg.wind.line_width == bw) ? V_PASS : V_FAIL,
            "read while drawn: enable %u at (%u,%u) width %u line %u", g->cfg.wind.enable, g->cfg.wind.left, g->cfg.wind.top, g->cfg.wind.width, g->cfg.wind.line_width);
        gfree(a); gfree(g);
    }
}
#else
static void cs_drawblock(void) {}
#endif

/* ---------------------------------------------------------------------------------------------- T40 ISP calls (backlog T40) */
#if defined(APITEST_T40)
static void cs_scalerlv_t40(void)
{
    NEED(IMP_ISP_SetScalerLv) {
        G(IMPISPScalerLvAttr, a);
        int r1, r2, r3, ms;
        Fst lo, hi, back, base;

        base = fst(800);
        a->chx = 1; a->mode = IMPISP_SCALER_FITTING_CRUVE; a->level = 0;
        r1 = IMP_ISP_SetScalerLv(IMPVI_MAIN, a); lo = fst(1200);
        a->level = 128;
        r2 = IMP_ISP_SetScalerLv(IMPVI_MAIN, a); hi = fst(1200);
        a->level = 64;
        r3 = IMP_ISP_SetScalerLv(IMPVI_MAIN, a); back = recover(base.y, &ms);
        {
            int eff = lo.ok && hi.ok && dabs(lo.h - hi.h) > 0.05;
            rep(FN(IMP_ISP_SetScalerLv), r1 | r2 | r3, (r1 == 0 && r2 == 0 && r3 == 0 && lo.ok && hi.ok && lo.y > 5 && hi.y > 5 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%sch1 level 0/128/64 accepted, detail H %.3f at 0 vs %.3f at 128, Y %.1f/%.1f, picture %s alive (Y %.1f)", RTMSG(eff), lo.h, hi.h, lo.y, hi.y, ms >= 0 ? "" : "NOT", back.y);
        }
        gfree(a);
    }
}

static void cs_fixedcontrast(void)
{
    NEED(IMP_ISP_Tuning_SetFixedContraster) {
        G(IMPISPFixedContrastAttr, a);
        Fst base, on, back;
        int r1, r2, ms;

        base = fst(800);
        a->mode = 1; a->range_low = 70; a->range_high = 180;
        r1 = IMP_ISP_Tuning_SetFixedContraster(IMPVI_MAIN, a); on = fst(1500);
        a->mode = 0; a->range_low = 16; a->range_high = 235;
        r2 = IMP_ISP_Tuning_SetFixedContraster(IMPVI_MAIN, a); back = recover(base.y, &ms);
        {
            int eff = on.ok && base.ok && dabs(on.sd - base.sd) > 3;
            rep(FN(IMP_ISP_Tuning_SetFixedContraster), r1 | r2, (r1 == 0 && r2 == 0 && on.ok && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%srange 70..180 on: luma std-dev %.1f -> %.1f (Y %.1f -> %.1f); off: back to Y %.1f in %d ms, frames flow", RTMSG(eff), base.sd, on.sd, base.y, on.y, back.y, ms);
        }
        gfree(a);
    }
}

static void cs_faceawb(void)
{
    NEED(IMP_ISP_Tuning_SetFaceAwb) NEED(IMP_ISP_Tuning_GetFaceAwb) {
        IMPISPFaceAttr o, f, g, z;
        Fst base, on, back;
        int r0, r1, r2, r3, ms;

        memset(&o, 0, sizeof(o)); memset(&g, 0, sizeof(g)); memset(&f, 0, sizeof(f)); memset(&z, 0, sizeof(z));
        r0 = IMP_ISP_Tuning_GetFaceAwb(IMPVI_MAIN, &o);
        base = fst(600);
        f.enable = IMPISP_TUNING_OPS_MODE_ENABLE; f.left = g_sw / 4; f.top = g_sh / 4; f.right = g_sw * 3 / 4; f.bottom = g_sh * 3 / 4; f.target = 128;
        r1 = IMP_ISP_Tuning_SetFaceAwb(IMPVI_MAIN, &f); on = fst(1500);
        r2 = IMP_ISP_Tuning_GetFaceAwb(IMPVI_MAIN, &g);
        z.enable = IMPISP_TUNING_OPS_MODE_DISABLE;
        r3 = IMP_ISP_Tuning_SetFaceAwb(IMPVI_MAIN, &z); back = recover(base.y, &ms);
        {
            int eff = on.ok && base.ok && (dabs(on.u - base.u) + dabs(on.v - base.v)) > 0.8;
            rep(FN(IMP_ISP_Tuning_GetFaceAwb), r0 | r2, (r0 == 0 && r2 == 0 && g.enable == f.enable && g.left == f.left && g.top == f.top && g.right == f.right && g.bottom == f.bottom) ? V_PASS : V_FAIL,
                "initial enable %d; while set: enable %d window (%u,%u)-(%u,%u) target %u", (int)o.enable, (int)g.enable, g.left, g.top, g.right, g.bottom, g.target);
            rep(FN(IMP_ISP_Tuning_SetFaceAwb), r1 | r3, (r1 == 0 && r3 == 0 && on.ok && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "%scentre window target 128: chroma U/V %.2f/%.2f -> %.2f/%.2f; off: back to Y %.1f in %d ms, frames flow", RTMSG(eff), base.u, base.v, on.u, on.v, back.y, ms);
        }
    }
}

/* #139: tuning node /dev/tisp, AE manual block of the OEM tuning tool: 0x20007404 get / 0x20007405 set on a
 * 0x5010-byte parameter block (+4 size, +0xc sensor 0 main / 1 sec, +0x10 the 164-byte block).  The get
 * uses the block of the previous call, so 0x20007402 (version, copies the block in and out) selects the
 * sensor first.  Before the fix every set oopsed (lifted stores through never-assigned pointers).
 * Same-value get/set/get per sensor block, then frames flow.  Not a libimp API: reported as tisp_node_*. */
#include <fcntl.h>
#include <sys/ioctl.h>
#define T40_TISP_PAR_BYTES 0x5010
static int t40_tisp_ae_get(int fd, uint32_t *p, uint32_t sel)
{
    int r;

    memset(p, 0, T40_TISP_PAR_BYTES); p[3] = sel;
    r = ioctl(fd, 0x20007402, p);
    p[3] = sel;
    return r | ioctl(fd, 0x20007404, p);
}

static void cs_aeinfo_t40(void)
{
    uint32_t *a = (uint32_t *)calloc(1, T40_TISP_PAR_BYTES), *b = (uint32_t *)calloc(1, T40_TISP_PAR_BYTES);
    int fd = open("/dev/tisp", O_RDWR);
    uint32_t sel;

    if (fd < 0 || !a || !b) {
        rep("tisp_node_ae_info", -1, V_SKIP, "/dev/tisp not usable (fd %d)", fd);
    } else {
        for (sel = 0; sel < 2; sel++) {
            int r1, r2, r3, same;

            r1 = t40_tisp_ae_get(fd, a, sel);
            a[3] = sel;
            r2 = ioctl(fd, 0x20007405, a);
            r3 = t40_tisp_ae_get(fd, b, sel);
            same = !memcmp(a + 4, b + 4, 0xa4);
            rep("tisp_node_ae_info", r1 | r2 | r3, (r1 == 0 && r2 == 0 && r3 == 0 && a[1] == 0xa4 && same && flow()) ? V_PASS : V_FAIL,
                "%s: get/set/get of the 164-byte AE manual block (size %u, ae_manual %u it %u ag %u, short ae_manual %u), read back %s, frames flow",
                sel ? "sec" : "main", a[1], a[4], a[4 + 3], a[4 + 1], a[4 + 24], same ? "equal" : "DIFFERENT");
        }
    }
    if (fd >= 0)
        close(fd);
    free(a); free(b);
}

/* #139: GetFaceAe (control 0x8000035) used to copy an uninitialised stack slot; it reads back what
 * SetFaceAe stored (no picture check: QEMU-safe round trip, the effect case is t40dev A9). */
static void cs_faceae_t40(void)
{
    NEED(IMP_ISP_Tuning_SetFaceAe) NEED(IMP_ISP_Tuning_GetFaceAe) {
        IMPISPFaceAttr o, f, g, h, z;
        int r0, r1, r2, r3, r4;

        memset(&o, 0, sizeof(o)); memset(&f, 0, sizeof(f)); memset(&g, 0x5a, sizeof(g)); memset(&h, 0x5a, sizeof(h)); memset(&z, 0, sizeof(z));
        r0 = IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &o);
        f.enable = IMPISP_TUNING_OPS_MODE_ENABLE; f.left = g_sw / 4; f.top = g_sh / 4; f.right = g_sw * 3 / 4; f.bottom = g_sh * 3 / 4; f.target = 160;
        r1 = IMP_ISP_Tuning_SetFaceAe(IMPVI_MAIN, &f);
        r2 = IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &g);
        z.enable = IMPISP_TUNING_OPS_MODE_DISABLE;
        r3 = IMP_ISP_Tuning_SetFaceAe(IMPVI_MAIN, &z);
        r4 = IMP_ISP_Tuning_GetFaceAe(IMPVI_MAIN, &h);
        rep(FN(IMP_ISP_Tuning_GetFaceAe), r0 | r2 | r4, (r0 == 0 && r2 == 0 && r4 == 0 && !memcmp(&g, &f, sizeof(f)) && h.enable == IMPISP_TUNING_OPS_MODE_DISABLE) ? V_PASS : V_FAIL,
            "initial enable %d; after set: enable %d (%u,%u)-(%u,%u) target %u (%s); after disable: enable %d",
            (int)o.enable, (int)g.enable, g.left, g.top, g.right, g.bottom, g.target, !memcmp(&g, &f, sizeof(f)) ? "as set" : "DIFFERENT", (int)h.enable);
        rep(FN(IMP_ISP_Tuning_SetFaceAe), r1 | r3, (r1 == 0 && r3 == 0 && flow()) ? V_PASS : V_FAIL, "centre window target 160 then disable accepted, frames flow");
    }
}

/* #139: SwitchBin on T40 (control 0x80000a5) oopsed in the lifted tisp_core_switch_bin (stores through
 * never-assigned pointers) and returned 1 on success.  The T41 case needs GetDefaultBinPath, which is -1 on
 * T40; this one switches to the bin file of the running sensor (/etc/sensor/<sensor>-t40.bin), then back
 * to the default with enable 0.  Ret 0 and frames flow afterwards. */
#include <glob.h>
static void cs_switchbin_t40(void)
{
    NEED(IMP_ISP_Tuning_SwitchBin) {
        G(IMPISPBinAttr, a);
        glob_t gl;
        int r1 = -1, r2 = -1, ms = 0;
        Fst base, back;

        char pat[96];

        memset(&gl, 0, sizeof(gl));
        snprintf(pat, sizeof(pat), "/etc/sensor/%s-t40.bin", g_sensor.name);   /* the running sensor's bin */
        if (glob(pat, 0, NULL, &gl) != 0 || gl.gl_pathc == 0) {
            rep(FN(IMP_ISP_Tuning_SwitchBin), -1, V_SKIP, "no %s", pat);
        } else {
            base = fst(800);
            a->enable = IMPISP_TUNING_OPS_MODE_ENABLE; snprintf(a->bname, sizeof(a->bname), "%s", gl.gl_pathv[0]);
            r1 = IMP_ISP_Tuning_SwitchBin(IMPVI_MAIN, a);
            a->enable = IMPISP_TUNING_OPS_MODE_DISABLE;
            r2 = IMP_ISP_Tuning_SwitchBin(IMPVI_MAIN, a); back = recover(base.y, &ms);
            rep(FN(IMP_ISP_Tuning_SwitchBin), r1 | r2, (r1 == 0 && r2 == 0 && ms >= 0 && flow()) ? V_PASS : V_FAIL,
                "rt: %.50s (ret %d), back to the default (ret %d), Y %.1f -> %.1f, frames flow", a->bname, r1, r2, base.y, back.y);
        }
        globfree(&gl);
        gfree(a);
    }
}
#else
static void cs_scalerlv_t40(void) {}
static void cs_fixedcontrast(void) {}
static void cs_faceawb(void) {}
static void cs_aeinfo_t40(void) {}
static void cs_faceae_t40(void) {}
static void cs_switchbin_t40(void) {}
#endif

void cases_list(void)
{
    static const char *n[] = { "sensorreg", "log", "waitframe", "videodrop", "movestate", "aefreeze", "aehistorigin", "autozoom", "framedrop", "cwf", "sinter", "temper", "process", "fwfreeze", "hilight", "inttime", "expr", "scene", "colorfx", "gamma", "modctl", "ccm", "autozoom_set", "isposd", "aeroi", "rawdrc", "wdr_t41", "bypass_t40", "camsel", "binpath", "rgbcoef", "ispbypass", "switchbin", "scalerlv", "contrast", "hue", "aecomp", "backlight", "dpc", "custom", "wdr", "wdrout", "ispflip", "hvflip", "ispbypass_old", "gamma_old", "csc", "awbct", "scalerlv_old", "stalesnap", "aestats", "faceae", "aeexplist", "dmic", "fspool", "sensorflip", "aestrategy", "alloc", "isposdapi", "aeevlist", "faceaeweight", "tmofaceae", "drawblock", "scalerlv_t40", "fixedcontrast", "faceawb", "aeinfo_t40", "faceae_t40", "switchbin_t40" };
    unsigned i;

    for (i = 0; i < sizeof(n) / sizeof(n[0]); i++) puts(n[i]);
}

void t_cases(void)
{
    rep_area("cases");
    wdog("cases", 60);
    { int i; for (i = 0; i < 25 && !flow(); i++) usleep(200000); }      /* warm-up: wait for the first frames (T40 at 1080p starts slowly) */
    measure_drift();
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
    if (cs_on("expr")) cs_expr();       wdog("cases", 90);
    if (cs_on("scene")) cs_scene();      wdog("cases", 90);
    if (cs_on("colorfx")) cs_colorfx();    wdog("cases", 90);
    if (cs_on("gamma")) cs_gamma();      wdog("cases", 90);
    if (cs_on("modctl")) cs_modctl();     wdog("cases", 90);
    if (cs_on("ccm")) cs_ccm();        wdog("cases", 60);
    if (cs_on("rgbcoef")) cs_rgbcoef();    wdog("cases", 90);
    if (cs_on("ispbypass")) cs_ispbypass();  wdog("cases", 60);
    if (cs_on("switchbin")) cs_switchbin();  wdog("cases", 90);
    if (cs_on("scalerlv")) cs_scalerlv();   wdog("cases", 90);
    if (cs_on("contrast")) cs_contrast();   wdog("cases", 90);
    if (cs_on("hue")) cs_hue();        wdog("cases", 90);
    if (cs_on("aecomp")) cs_aecomp();     wdog("cases", 90);
    if (cs_on("backlight")) cs_backlight();  wdog("cases", 90);
    if (cs_on("dpc")) cs_dpc();        wdog("cases", 60);
    if (cs_on("custom")) cs_custom();     wdog("cases", 60);
    if (cs_on("wdr")) cs_wdr();        wdog("cases", 60);
    if (cs_on("wdrout")) cs_wdrout();     wdog("cases", 60);
    if (cs_on("ispflip")) cs_ispflip();    wdog("cases", 60);
    if (cs_on("hvflip")) cs_hvflip();     wdog("cases", 60);
    if (getenv("APITEST_CASES") && cs_on("ispbypass_old")) cs_ispbypass_old();   /* explicit only: wedges the frame flow in QEMU (issue) */
    wdog("cases", 60);
    if (cs_on("gamma_old")) cs_gamma_old();  wdog("cases", 90);
    if (cs_on("wdr_t41")) cs_wdr_t41();   wdog("cases", 60);
    if (cs_on("bypass_t40")) cs_bypass_t40(); wdog("cases", 60);
    if (cs_on("camsel")) cs_camsel();     wdog("cases", 60);
    if (cs_on("aeroi")) cs_aeroi();      wdog("cases", 60);
    if (cs_on("rawdrc")) cs_rawdrc();     wdog("cases", 60);
    if (cs_on("isposd")) cs_isposd();     wdog("cases", 90);
    if (cs_on("autozoom_set")) cs_autozoom_set(); wdog("cases", 90);
    if (cs_on("csc")) cs_csc();        wdog("cases", 90);
    if (cs_on("awbct")) cs_awbct();      wdog("cases", 90);
    if (cs_on("scalerlv_old")) cs_scalerlv_old(); wdog("cases", 90);
    if (getenv("APITEST_CASES") && cs_on("stalesnap")) cs_stalesnap();   /* explicit only: ~3 min of pauses (issue #89) */
    wdog("cases", 240);
    if (cs_on("aestats")) cs_aestats();    wdog("cases", 60);
    if (cs_on("faceae")) cs_faceae();      wdog("cases", 90);
    if (cs_on("aeexplist")) cs_aeexplist(); wdog("cases", 60);
    if (cs_on("dmic")) t_dmic();        wdog("cases", 60);
    if (cs_on("fspool")) cs_fspool();     wdog("cases", 90);
    if (cs_on("sensorflip")) cs_sensorflip(); wdog("cases", 90);
    if (cs_on("aestrategy")) cs_aestrategy(); wdog("cases", 90);
    if (cs_on("alloc")) cs_alloc();       wdog("cases", 60);
    if (cs_on("isposdapi")) cs_isposdapi(); wdog("cases", 90);
    if (cs_on("aeevlist")) cs_aeevlist();  wdog("cases", 60);
    if (cs_on("faceaeweight")) cs_faceaeweight(); wdog("cases", 60);
    if (cs_on("tmofaceae")) cs_tmofaceae(); wdog("cases", 90);
    if (cs_on("drawblock")) cs_drawblock(); wdog("cases", 90);
    if (cs_on("scalerlv_t40")) cs_scalerlv_t40(); wdog("cases", 90);
    if (cs_on("fixedcontrast")) cs_fixedcontrast(); wdog("cases", 90);
    if (cs_on("faceawb")) cs_faceawb();   wdog("cases", 90);
    if (cs_on("aeinfo_t40")) cs_aeinfo_t40(); wdog("cases", 60);
    if (cs_on("faceae_t40")) cs_faceae_t40(); wdog("cases", 60);
    if (cs_on("switchbin_t40")) cs_switchbin_t40(); wdog("cases", 90);
    wdog("cases", 60);
}
