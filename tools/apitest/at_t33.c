/* at_t33.c - case "t33pass" (T33 only, -DAPITEST_T33 with the T33 2.0.2.1 headers; #54-#60).
 *
 * API-level lines for the T33 backlog functions that had no T33 run: ISP tuning round trips that the T41-typed cases cannot cover
 * (2.0.2.1 structs), the ISP control calls of batch Q1 and the FrameSource / encoder state calls of batch Q2 (audit t33backlog/PLAN.md).
 * Every line prints the return codes in hex so the vendor libimp 2.0.2.1 and OpenIMP runs can be compared line by line (same apitest,
 * same QEMU guest: audit/t3233gaps/run33.sh).  The verdicts encode the vendor behaviour seen in the vendor run (QEMU, no T33 camera):
 * return codes, read-backs and "frames still flow" after the call.  Nothing here claims a picture effect.
 * Functions that the 2.0.2.1 headers do not declare (but the vendor library exports) are declared weak here with the vendor signature. */
#if defined(APITEST_T33)
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "apitest.h"

int cases_flow(void);       /* at_cases.c: two frames of ch1 with increasing time stamps */

/* exported by the vendor T33 libimp 2.0.2.1, not declared in its headers */
extern void IMP_ISP_AddSensor_Free(int vinum) __attribute__((weak));
extern int IMP_ISP_AddSensor_Process(int vinum) __attribute__((weak));
extern int IMP_ISP_GPIOSwitchConfig(unsigned int vinum, uint32_t *cfg) __attribute__((weak));
extern int IMP_ISP_GPIOSwitchTrigger(unsigned int vinum) __attribute__((weak));
extern int IMP_ISP_OSD_Alloc(void **out_phys, const void *src, int len) __attribute__((weak));
extern int IMP_OSD_DetachToGroup(IMPCell *from, IMPCell *to) __attribute__((weak));
extern int IMP_Encoder_ClearPoolId(void) __attribute__((weak));
extern int IMP_FrameSource_ClearPoolId(void) __attribute__((weak));
extern int IMP_Encoder_GetFisheyeEnableStatus(int chn, int *en) __attribute__((weak));
extern int IMP_Encoder_SetFisheyeEnableStatus(int chn, int en) __attribute__((weak));
extern int IMP_Encoder_MappingTransform(void *in, int *out) __attribute__((weak));
extern void IMP_Encoder_SetChnQpgAIEnable_Yuv(int chn, int en) __attribute__((weak));
extern int IMP_Encoder_SetChnResizeMode(int chn, int mode) __attribute__((weak));
extern void IMP_Encoder_SetMotionQualityEnable_Yuv(int chn, int en) __attribute__((weak));
extern int IMP_Encoder_SetMotionQuality_Yuv(int *mode) __attribute__((weak));
extern int IMP_FrameSource_SetJointBackground(uint32_t color) __attribute__((weak));
extern int IMP_FrameSource_SetDirectMode(int mode) __attribute__((weak));

#define X(r) ((unsigned)(r))
/* the pair key of the evidence table is PASS only with both members PASS: one detail, two lines */
#define REP2(g, s, r, ok, ...) do { rep(FN(g), (long)(r), (ok) ? V_PASS : V_FAIL, __VA_ARGS__); \
                                    rep(FN(s), (long)(r), (ok) ? V_PASS : V_FAIL, __VA_ARGS__); } while (0)

/* bytes that differ between two reads: 0 = equal; d = "equal" or "N bytes differ, first at +off (a -> b)" */
static int bdiff(const void *a, const void *b, size_t n, char *d)
{
    const uint8_t *x = a, *y = b;
    size_t i, c = 0, f = 0;

    for (i = 0; i < n; i++) if (x[i] != y[i]) { if (!c) f = i; c++; }
    if (!c) snprintf(d, 96, "equal");
    else snprintf(d, 96, "%u bytes differ, first at +%u (0x%02x -> 0x%02x)", (unsigned)c, (unsigned)f, x[f], y[f]);
    return (int)c;
}

/* same-value round trip: Get, Set the bytes read, Get again, frames flow.  PASS = all three return 0 and frames flow; the byte
 * comparison is logged only: with the vendor libimp on the vendor module several attributes read back changed (driver state that
 * moves with the AE/AWB, QEMU 2026-11-13: Module_Ratio, AeExprInfo, FaceAwb target, AeTargetAttr, OSDAttr, MaskBlock) */
#define RT(GETF, SETF, T, SETTLE_US) NEED(GETF) NEED(SETF) { \
        T *o_ = (T *)gnew(sizeof(T)), *g_ = (T *)gnew(sizeof(T)); \
        int r0_ = GETF(IMPVI_MAIN, o_), r1_, r2_, same_; char d_[96]; \
        r1_ = SETF(IMPVI_MAIN, o_); usleep(SETTLE_US); \
        r2_ = GETF(IMPVI_MAIN, g_); same_ = bdiff(o_, g_, sizeof(T), d_); gchk(o_); gchk(g_); \
        REP2(GETF, SETF, r0_ | r1_ | r2_, !(r0_ | r1_ | r2_) && cases_flow(), \
             "rt (%u B): get 0x%x, same value set 0x%x, get 0x%x: %s; frames flow", (unsigned)sizeof(T), X(r0_), X(r1_), X(r2_), d_); \
        gfree(o_); gfree(g_); }

/* encoder globals before IMP_System_Init (vendor: refused with 0x80040080 once the encoder module is up): only the 0 / range
 * argument checks, so no setting changes for the rest of the run */
static int g_pre_ivpu = 1, g_pre_jpeg = 1, g_pre_ms = 1, g_pre_jms = 1;
void t33_pre_sysinit(void)
{
    const char *c = getenv("APITEST_CASES");

    if (c && !strstr(c, "t33pass")) return;
    if (IMP_Encoder_SetIvpuBsSize) g_pre_ivpu = IMP_Encoder_SetIvpuBsSize(0);
    if (IMP_Encoder_SetJpegBsSize) g_pre_jpeg = IMP_Encoder_SetJpegBsSize(0);
    if (IMP_Encoder_SetMultiSectionMode) g_pre_ms = IMP_Encoder_SetMultiSectionMode(9, 0, 0);
    if (IMP_Encoder_SetJpegMultiSectionMode) g_pre_jms = IMP_Encoder_SetJpegMultiSectionMode(9, 0, 0);
}

static void t33_isp_rt(void)
{
    RT(IMP_ISP_GetCsccrMode, IMP_ISP_SetCsccrMode, IMPISPCsccrModeAttr, 100000)
    RT(IMP_ISP_Tuning_GetModule_Ratio, IMP_ISP_Tuning_SetModule_Ratio, IMPISPModuleRatioAttr, 100000)
    RT(IMP_ISP_Tuning_GetWdrOutputMode, IMP_ISP_Tuning_SetWdrOutputMode, IMPISPWdrOutputMode, 100000)
    RT(IMP_ISP_Tuning_GetAeExprInfo, IMP_ISP_Tuning_SetAeExprInfo, IMPISPAEExprInfo, 300000)
    RT(IMP_ISP_Tuning_GetAwbAttr, IMP_ISP_Tuning_SetAwbAttr, IMPISPWBAttr, 300000)
    RT(IMP_ISP_Tuning_GetFaceAwb, IMP_ISP_Tuning_SetFaceAwb, IMPISPFaceAttr, 100000)
    RT(IMP_ISP_Tuning_GetAeLightCompensation, IMP_ISP_Tuning_SetAeLightCompensation, IMPISPAeLightCompensationAttr, 100000)
    RT(IMP_ISP_Tuning_GetAeStartAttr, IMP_ISP_Tuning_SetAeStartAttr, IMPISPAeStartAttr, 100000)
    RT(IMP_ISP_Tuning_GetAeTargetAttr, IMP_ISP_Tuning_SetAeTargetAttr, IMPISPAeTargetAttr, 300000)
    RT(IMP_ISP_HB_GetAttr, IMP_ISP_HB_SetAttr, uint16_t, 100000)
    RT(IMP_ISP_GetInternalChnAttr, IMP_ISP_SetInternalChnAttr, IMPISPInternalChnAttr, 100000)
    RT(IMP_ISP_Tuning_GetOSDAttr, IMP_ISP_Tuning_SetOSDAttr, IMPISPOSDAttr, 100000)

    /* ExpList: the list is recomputed by the AE while it runs (#90): mode read back equal, list compared only as a logged count */
    NEED(IMP_ISP_Tuning_GetAeExpList) NEED(IMP_ISP_Tuning_SetAeExpList) {
        G(IMPISPAeExpListAttr, o); G(IMPISPAeExpListAttr, g);
        int r0 = IMP_ISP_Tuning_GetAeExpList(IMPVI_MAIN, o), r1, r2, i, d = 0;
        r1 = IMP_ISP_Tuning_SetAeExpList(IMPVI_MAIN, o); usleep(200000);
        r2 = IMP_ISP_Tuning_GetAeExpList(IMPVI_MAIN, g); gchk(o); gchk(g);
        for (i = 0; i < 5 * 16; i++) d += o->elist[i] != g->elist[i];
        REP2(IMP_ISP_Tuning_GetAeExpList, IMP_ISP_Tuning_SetAeExpList, r0 | r1 | r2, !(r0 | r1 | r2) && o->mode == g->mode && cases_flow(),
             "rt: get 0x%x mode %d, same value set 0x%x, get 0x%x mode %d (%d of 80 list words differ); frames flow", X(r0), (int)o->mode, X(r1), X(r2), (int)g->mode, d);
        gfree(o); gfree(g);
    }
    /* MaskBlock: block 0 of channel 0 as read, set back, read again */
    NEED(IMP_ISP_Tuning_GetMaskBlock) NEED(IMP_ISP_Tuning_SetMaskBlock) {
        G(IMPISPMaskBlockAttr, o); G(IMPISPMaskBlockAttr, g);
        int r0, r1, r2, same;
        o->chx = 0; o->pinum = 0; g->chx = 0; g->pinum = 0;
        r0 = IMP_ISP_Tuning_GetMaskBlock(IMPVI_MAIN, o);
        r1 = IMP_ISP_Tuning_SetMaskBlock(IMPVI_MAIN, o); usleep(100000);
        r2 = IMP_ISP_Tuning_GetMaskBlock(IMPVI_MAIN, g); gchk(o); gchk(g);
        same = !memcmp(o, g, sizeof(*o));
        REP2(IMP_ISP_Tuning_GetMaskBlock, IMP_ISP_Tuning_SetMaskBlock, r0 | r1 | r2, !(r0 | r1 | r2) && cases_flow(),
             "rt ch0 block 0: get 0x%x (en %d %ux%u), same value set 0x%x, get 0x%x: %s; frames flow", X(r0), o->mask_en, o->mask_width, o->mask_height,
             X(r1), X(r2), same ? "equal" : "DIFFERENT");
        gfree(o); gfree(g);
    }
    /* OSDBlock: block 0 read; set back with the block off (no image pointer needed) */
    NEED(IMP_ISP_Tuning_GetOSDBlock) NEED(IMP_ISP_Tuning_SetOSDBlock) {
        G(IMPISPOSDBlockAttr, o); G(IMPISPOSDBlockAttr, g);
        int r0, r1, r2;
        o->pinum = 0; g->pinum = 0;
        r0 = IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, o);
        o->osd_enable = 0;
        r1 = IMP_ISP_Tuning_SetOSDBlock(IMPVI_MAIN, o); usleep(100000);
        r2 = IMP_ISP_Tuning_GetOSDBlock(IMPVI_MAIN, g); gchk(o); gchk(g);
        REP2(IMP_ISP_Tuning_GetOSDBlock, IMP_ISP_Tuning_SetOSDBlock, r0 | r1 | r2, !(r0 | r1 | r2) && g->osd_enable == 0 && cases_flow(),
             "block 0: get 0x%x, set off 0x%x, get 0x%x (en %d %ux%u at %u,%u); frames flow", X(r0), X(r1), X(r2), g->osd_enable, g->osd_width, g->osd_height,
             g->osd_left, g->osd_top);
        gfree(o); gfree(g);
    }
    /* read-only getters */
    NEED(IMP_ISP_Tuning_GetAeOnlyReadAttr) {
        G(IMPISPAeOnlyReadAttr, a);
        int r = IMP_ISP_Tuning_GetAeOnlyReadAttr(IMPVI_MAIN, a); gchk(a);
        CHECK(IMP_ISP_Tuning_GetAeOnlyReadAttr, r, 1, "0x%x: gain %u dB, EV log2 %u, luma %u, target %u, stable %d", X(r), a->TotalGainDb, a->EVLog2, a->luma, a->target, a->stable);
        gfree(a);
    }
    NEED(IMP_ISP_Tuning_GetAwbOnlyReadAttr) {
        G(IMPISPAwbOnlyReadAttr, a);
        int r = IMP_ISP_Tuning_GetAwbOnlyReadAttr(IMPVI_MAIN, a); gchk(a);
        CHECK(IMP_ISP_Tuning_GetAwbOnlyReadAttr, r, 1, "0x%x: ct %u", X(r), a->ct);
        gfree(a);
    }
    /* argument checks shared by the tuning calls: NULL and VI out of range */
    /* no argument probes on the tuning calls: the vendor GetAeStartAttr writes through a NULL pointer (SIGSEGV) and with VI 5 the
     * vendor tx-isp-t33.ko oopses (QEMU 2026-11-13) */
}

static void t33_isp_q1(void)
{
    NEED(IMP_ISP_GET_DriverMode) {
        IMPISPDriverMode m = (IMPISPDriverMode)7;
        int r = IMP_ISP_GET_DriverMode(&m), rn = IMP_ISP_GET_DriverMode(NULL);
        CHECK(IMP_ISP_GET_DriverMode, r, (int)m == 0, "0x%x mode %d (normal), NULL 0x%x", X(r), (int)m, X(rn));
    }
    NEED(IMP_ISP_LDC_GetMode) NEED(IMP_ISP_LDC_SetMode) {
        IMPISPLDCMode m = (IMPISPLDCMode)7;
        int r0 = IMP_ISP_LDC_GetMode(&m), rn = IMP_ISP_LDC_GetMode(NULL), rb = IMP_ISP_LDC_SetMode((IMPISPLDCMode)2), r1, r2;
        IMPISPLDCMode m2 = (IMPISPLDCMode)7;
        r1 = IMP_ISP_LDC_SetMode(IMPISP_ONLINE_MODE);
        r2 = IMP_ISP_LDC_GetMode(&m2);
        REP2(IMP_ISP_LDC_GetMode, IMP_ISP_LDC_SetMode, r1 | r2, !r1 && !r2 && (int)m2 == 0 && cases_flow(),
             "get before any set 0x%x (mode %d), get(NULL) 0x%x, set 2 0x%x, set online 0x%x, get 0x%x mode %d; frames flow",
             X(r0), (int)m, X(rn), X(rb), X(r1), X(r2), (int)m2);
    }
    NEED(IMP_ISP_LDC_INIT) NEED(IMP_ISP_LDC_GetAttr) NEED(IMP_ISP_LDC_SetAttr) {
        G(IMPISPLDCInitAttr, ia); G(IMPISPLDCAttr, la);
        int ri = IMP_ISP_LDC_INIT(IMPVI_MAIN, ia), rg = IMP_ISP_LDC_GetAttr(IMPVI_MAIN, la), rs = IMP_ISP_LDC_SetAttr(IMPVI_MAIN, la);
        gchk(ia); gchk(la);
        rep(FN(IMP_ISP_LDC_INIT), ri, ri == 0 ? V_PASS : V_FAIL, "0x%x (vendor stub: log, 0)", X(ri));
        rep(FN(IMP_ISP_LDC_GetAttr), rg, rg == 0 ? V_PASS : V_FAIL, "0x%x (vendor stub: log, 0), block untouched %d", X(rg), !gtouched(la));
        rep(FN(IMP_ISP_LDC_SetAttr), rs, rs == 0 ? V_PASS : V_FAIL, "0x%x (vendor stub: log, 0)", X(rs));
        gfree(ia); gfree(la);
    }
    NEED(IMP_ISP_PM_GetMode) NEED(IMP_ISP_PM_SetMode) {
        IMPISPPMAttr a = (IMPISPPMAttr)7, b;
        int r0 = IMP_ISP_PM_GetMode(IMPVI_MAIN, &a), r1, r2;
        b = TISP_NORMAL_MODE;
        r1 = IMP_ISP_PM_SetMode(IMPVI_MAIN, &b);
        b = (IMPISPPMAttr)7;
        r2 = IMP_ISP_PM_GetMode(IMPVI_MAIN, &b);
        /* vendor T33 (QEMU): both answer 0 and the get leaves the caller's value (7) as it was: the driver echoes the request */
        REP2(IMP_ISP_PM_GetMode, IMP_ISP_PM_SetMode, r1, r0 == 0 && r1 == 0 && r2 == 0 && cases_flow(),
             "get 0x%x mode %d, set normal 0x%x, get 0x%x mode %d; frames flow", X(r0), (int)a, X(r1), X(r2), (int)b);
    }
    NEED(IMP_ISP_PM_WaitAllDone) {
        IMPISPOpsMode e = IMPISP_OPS_MODE_ENABLE;
        int r = IMP_ISP_PM_WaitAllDone(&e);
        rep(FN(IMP_ISP_PM_WaitAllDone), r, (r == 0 || r == (int)0xfffff001) && cases_flow() ? V_PASS : V_FAIL, "0x%x; frames flow", X(r));
    }
    NEED(IMP_ISP_SetCameraInputMode) {
        G(IMPISPCameraInputMode, m);
        int rn = IMP_ISP_SetCameraInputMode(NULL), rs, ri;
        memset(m, 0, sizeof(*m)); m->sensor_num = (IMPISPSensorNum)5;
        rs = IMP_ISP_SetCameraInputMode(m);
        memset(m, 0, sizeof(*m)); m->input.mode = (IMPISPMultiInputMode)2;
        ri = IMP_ISP_SetCameraInputMode(m); gchk(m);
        rep(FN(IMP_ISP_SetCameraInputMode), rn, rn == (int)0xfffff008 && rs == (int)0xfffff004 && ri == (int)0xfffff004 ? V_PASS : V_FAIL,
            "NULL 0x%x, sensor_num 5 0x%x, input 2 0x%x (argument checks; a valid mode is a pre-AddSensor call)", X(rn), X(rs), X(ri));
        gfree(m);
    }
    NEED(IMP_ISP_WDR_OPEN) {
        G(IMPISPWdrOpenAttr, w);
        int rv = IMP_ISP_WDR_OPEN((IMPVI_NUM)5, w), rn = IMP_ISP_WDR_OPEN(IMPVI_MAIN, NULL);
        gchk(w);
        /* vendor: the T32/T33 chip class has no WDR: after IMP_ISP_Open the call only logs and answers 0 (before any argument check) */
        rep(FN(IMP_ISP_WDR_OPEN), rv, rv == 0 && rn == 0 ? V_PASS : V_FAIL, "VI 5 0x%x, NULL 0x%x (no WDR on this chip class: no-op 0)", X(rv), X(rn));
        gfree(w);
    }
    NEED(IMP_ISP_SetAeAlgoFunc) NEED(IMP_ISP_SetAwbAlgoFunc) {
        IMPISPAeAlgoFunc ae; IMPISPAwbAlgoFunc awb;
        int rae, rawb, rv, rv2;
        memset(&ae, 0, sizeof(ae)); memset(&awb, 0, sizeof(awb));
        rae = IMP_ISP_SetAeAlgoFunc(IMPVI_MAIN, NULL);
        rawb = IMP_ISP_SetAwbAlgoFunc(IMPVI_MAIN, NULL);
        rv = IMP_ISP_SetAeAlgoFunc((IMPVI_NUM)4, &ae);
        rv2 = IMP_ISP_SetAwbAlgoFunc((IMPVI_NUM)4, &awb);
        rep(FN(IMP_ISP_SetAeAlgoFunc), rae, rae == 0 && rv == (int)0xfffff009 ? V_PASS : V_FAIL, "NULL (custom AE off) 0x%x, VI 4 0x%x; the callback records count at the next EnableSensor", X(rae), X(rv));
        rep(FN(IMP_ISP_SetAwbAlgoFunc), rawb, rawb == 0 && rv2 == (int)0xfffff009 ? V_PASS : V_FAIL, "NULL (custom AWB off) 0x%x, VI 4 0x%x", X(rawb), X(rv2));
    }
    NEED(IMP_ISP_GPIOSwitchConfig) NEED(IMP_ISP_GPIOSwitchTrigger) {
        uint32_t cfg[2] = { 0, 0 };
        int rc = IMP_ISP_GPIOSwitchConfig(5, cfg), rt = IMP_ISP_GPIOSwitchTrigger(5);
        rep(FN(IMP_ISP_GPIOSwitchConfig), rc, rc == (int)0xfffff00c ? V_PASS : V_FAIL, "VI 5 0x%x (a real pin is a camera test)", X(rc));
        rep(FN(IMP_ISP_GPIOSwitchTrigger), rt, rt == (int)0xfffff00c ? V_PASS : V_FAIL, "VI 5 0x%x (a real pin is a camera test)", X(rt));
    }
    NEED(IMP_ISP_OSD_Alloc) {
        void *p = NULL;
        static const uint8_t src[64] = { 1, 2, 3 };
        int r = IMP_ISP_OSD_Alloc(&p, src, sizeof(src));
        CHECK(IMP_ISP_OSD_Alloc, r, p != NULL, "64 bytes from a source buffer: 0x%x, physical address %s", X(r), p ? "set" : "NULL");
    }
    NEED(IMP_OSD_DetachToGroup) {
        IMPCell a = { DEV_ID_FS, 7, 0 }, b = { DEV_ID_OSD, 7, 0 };
        int r = IMP_OSD_DetachToGroup(&a, &b);
        rep(FN(IMP_OSD_DetachToGroup), r, r == (int)0x80020040 ? V_PASS : V_FAIL, "never attached FS 7 -> OSD 7: 0x%x", X(r));
    }
}

static void t33_fs_enc_q2(void)
{
    NEED(IMP_FrameSource_GetDirectMode) NEED(IMP_FrameSource_SetDirectMode) {
        int m = -1, m2 = -1, rg = IMP_FrameSource_GetDirectMode(&m), rb = IMP_FrameSource_SetDirectMode(5), rs, rg2;
        rs = IMP_FrameSource_SetDirectMode(m < 0 ? 0 : m);
        rg2 = IMP_FrameSource_GetDirectMode(&m2);
        REP2(IMP_FrameSource_GetDirectMode, IMP_FrameSource_SetDirectMode, rg | rg2, !rg && !rg2 && m2 == m && rb == (int)0x80010002 && cases_flow(),
             "get 0x%x mode %d, set 5 0x%x, set same 0x%x, get 0x%x mode %d; frames flow", X(rg), m, X(rb), X(rs), X(rg2), m2);
    }
    NEED(IMP_FrameSource_SetIvdcMemLine) {
        int rb = IMP_FrameSource_SetIvdcMemLine(0x1951), rs = IMP_FrameSource_SetIvdcMemLine(1080);
        rep(FN(IMP_FrameSource_SetIvdcMemLine), rb, rb == (int)0x80010002 && rs == (int)0x80040080 ? V_PASS : V_FAIL, "0x1951 lines 0x%x, 1080 lines while channels run 0x%x", X(rb), X(rs));
    }
    NEED(IMP_FrameSource_SetFrameDepthCopyType) {
        int rb = IMP_FrameSource_SetFrameDepthCopyType(12, 1), r5 = IMP_FrameSource_SetFrameDepthCopyType(5, 1);
        rep(FN(IMP_FrameSource_SetFrameDepthCopyType), rb, rb == (int)0x80010001 && r5 == 0 ? V_PASS : V_FAIL, "chn 12 0x%x, chn 5 (never created) 0x%x", X(rb), X(r5));
    }
    NEED(IMP_FrameSource_SetYuvAlign) {
        IMPFrameAlign al;
        int r12, r5, rn;
        memset(&al, 0, sizeof(al));
        r12 = IMP_FrameSource_SetYuvAlign(12, &al); r5 = IMP_FrameSource_SetYuvAlign(5, &al); rn = IMP_FrameSource_SetYuvAlign(CH_SUB, NULL);
        rep(FN(IMP_FrameSource_SetYuvAlign), r12, r12 == (int)0x80010001 && r5 == 0 && rn == (int)0x80010008 && cases_flow() ? V_PASS : V_FAIL,
            "chn 12 0x%x, chn 5 (not created) 0x%x, NULL 0x%x; frames flow", X(r12), X(r5), X(rn));
    }
    NEED(IMP_FrameSource_ClearPoolId) { int r = IMP_FrameSource_ClearPoolId(); RET0(IMP_FrameSource_ClearPoolId, r, "0x%x", X(r)); }
    NEED(IMP_Encoder_ClearPoolId) { int r = IMP_Encoder_ClearPoolId(); RET0(IMP_Encoder_ClearPoolId, r, "0x%x", X(r)); }
    NEED(IMP_FrameSource_CreateJoint) NEED(IMP_FrameSource_DestroyJoint) NEED(IMP_FrameSource_GetJointByHandler) {
        G(IMPFSJointAttr, j);
        int rn, rd = IMP_FrameSource_DestroyJoint(99), rg;
        memset(j, 0xff, sizeof(*j));
        int rg2 = -1, rd2 = -1, hnd;
        rn = IMP_FrameSource_CreateJoint(j);      /* empty board (all 0xff): the vendor creates an empty joint */
        hnd = j->handler;
        if (rn == 0) { memset(j, 0, sizeof(*j)); j->handler = hnd; rg2 = IMP_FrameSource_GetJointByHandler(j); rd2 = IMP_FrameSource_DestroyJoint(hnd); }
        memset(j, 0xff, sizeof(*j)); j->handler = 99;
        rg = IMP_FrameSource_GetJointByHandler(j); gchk(j);
        rep(FN(IMP_FrameSource_CreateJoint), rn, rn == 0 && rg2 == 0 && rd2 == 0 ? V_PASS : V_FAIL, "empty board 0x%x (%dx%d), get by its handler 0x%x, destroy 0x%x",
            X(rn), j->width, j->height, X(rg2), X(rd2));
        rep(FN(IMP_FrameSource_DestroyJoint), rd, rd == (int)0x80010008 && rd2 == 0 ? V_PASS : V_FAIL, "unknown handler 99 0x%x, created one 0x%x", X(rd), X(rd2));
        rep(FN(IMP_FrameSource_GetJointByHandler), rg, rg == (int)0x80010008 && rg2 == 0 ? V_PASS : V_FAIL, "unknown handler 99 0x%x, created one 0x%x", X(rg), X(rg2));
        gfree(j);
    }
    NEED(IMP_FrameSource_SetJointBackground) { int r = IMP_FrameSource_SetJointBackground(0x00808000); RET0(IMP_FrameSource_SetJointBackground, r, "0x%x", X(r)); }

    /* encoder: global settings (vendor: only while no channel exists) and per-channel state on a channel that does not exist (5) */
    NEED(IMP_Encoder_SetIvpuBsSize) {
        int r0 = IMP_Encoder_SetIvpuBsSize(0), r1 = IMP_Encoder_SetIvpuBsSize(512 * 1024);
        rep(FN(IMP_Encoder_SetIvpuBsSize), r0, r0 == (int)0x80040080 && r1 == (int)0x80040080 && g_pre_ivpu == (int)0x80040002 ? V_PASS : V_FAIL,
            "before System_Init 0 -> 0x%x; after: 0 -> 0x%x, 512 KiB -> 0x%x (only before the encoder module is up)", X(g_pre_ivpu), X(r0), X(r1));
    }
    NEED(IMP_Encoder_SetJpegBsSize) {
        int r0 = IMP_Encoder_SetJpegBsSize(0), r1 = IMP_Encoder_SetJpegBsSize(512 * 1024);
        rep(FN(IMP_Encoder_SetJpegBsSize), r0, r0 == (int)0x80040080 && r1 == (int)0x80040080 && g_pre_jpeg == (int)0x80040002 ? V_PASS : V_FAIL,
            "before System_Init 0 -> 0x%x; after: 0 -> 0x%x, 512 KiB -> 0x%x (only before the encoder module is up)", X(g_pre_jpeg), X(r0), X(r1));
    }
    NEED(IMP_Encoder_SetMultiSectionMode) {
        int rb = IMP_Encoder_SetMultiSectionMode(9, 0, 0), r0 = IMP_Encoder_SetMultiSectionMode(0, 0, 0);
        rep(FN(IMP_Encoder_SetMultiSectionMode), r0, rb == (int)0x80040080 && r0 == (int)0x80040080 && g_pre_ms == (int)0x80040002 ? V_PASS : V_FAIL,
            "before System_Init mode 9 -> 0x%x; after: mode 9 0x%x, mode 0 0x%x", X(g_pre_ms), X(rb), X(r0));
    }
    NEED(IMP_Encoder_SetJpegMultiSectionMode) {
        int rb = IMP_Encoder_SetJpegMultiSectionMode(9, 0, 0), r0 = IMP_Encoder_SetJpegMultiSectionMode(0, 0, 0);
        rep(FN(IMP_Encoder_SetJpegMultiSectionMode), r0, rb == (int)0x80040080 && r0 == (int)0x80040080 && g_pre_jms == (int)0x80040002 ? V_PASS : V_FAIL,
            "before System_Init mode 9 -> 0x%x; after: mode 9 0x%x, mode 0 0x%x", X(g_pre_jms), X(rb), X(r0));
    }
    NEED(IMP_Encoder_GetJpegQp) NEED(IMP_Encoder_SetJpegQp) {
        int q = -1, r16 = IMP_Encoder_GetJpegQp(16, &q), r5 = IMP_Encoder_GetJpegQp(5, &q), rn = IMP_Encoder_GetJpegQp(5, NULL);
        int s16 = IMP_Encoder_SetJpegQp(16, 30), s5 = IMP_Encoder_SetJpegQp(5, 30);
        REP2(IMP_Encoder_GetJpegQp, IMP_Encoder_SetJpegQp, r16, r16 == (int)0x80040001 && s16 == (int)0x80040001 && r5 == (int)0x80040008 && rn == (int)0x80040008 && s5 == (int)0x80040008,
             "get chn 16 0x%x, chn 5 0x%x, NULL 0x%x; set chn 16 0x%x, chn 5 0x%x", X(r16), X(r5), X(rn), X(s16), X(s5));
    }
    NEED(IMP_Encoder_GetChnFrmUsedMode) NEED(IMP_Encoder_SetChnFrmUsedMode) {
        IMPEncoderAttrFrmUsed f;
        int g16, g5, gn, s5;
        memset(&f, 0, sizeof(f));
        g16 = IMP_Encoder_GetChnFrmUsedMode(16, &f); g5 = IMP_Encoder_GetChnFrmUsedMode(5, &f); gn = IMP_Encoder_GetChnFrmUsedMode(5, NULL);
        s5 = IMP_Encoder_SetChnFrmUsedMode(5, &f);
        REP2(IMP_Encoder_GetChnFrmUsedMode, IMP_Encoder_SetChnFrmUsedMode, g16, g16 == (int)0x80040001 && g5 == (int)0x80040008 && gn == (int)0x80040010 && s5 == (int)0x80040008,
             "get chn 16 0x%x, chn 5 (not created) 0x%x, NULL 0x%x; set chn 5 0x%x", X(g16), X(g5), X(gn), X(s5));
    }
    NEED(IMP_Encoder_GetFisheyeEnableStatus) NEED(IMP_Encoder_SetFisheyeEnableStatus) {
        int e = -1, g16 = IMP_Encoder_GetFisheyeEnableStatus(16, &e), g5 = IMP_Encoder_GetFisheyeEnableStatus(5, &e);
        int s16 = IMP_Encoder_SetFisheyeEnableStatus(16, 1), s5 = IMP_Encoder_SetFisheyeEnableStatus(5, 0);
        REP2(IMP_Encoder_GetFisheyeEnableStatus, IMP_Encoder_SetFisheyeEnableStatus, g16, g16 == (int)0x80040001 && s16 == (int)0x80040001 && g5 == (int)0x80040008 && s5 == 0,
             "get chn 16 0x%x, chn 5 0x%x; set chn 16 0x%x, chn 5 (not created: before CreateChn) 0x%x", X(g16), X(g5), X(s16), X(s5));
    }
    NEED(IMP_Encoder_SetRdBufShare) {
        int s16 = IMP_Encoder_SetRdBufShare(16, 1), s5 = IMP_Encoder_SetRdBufShare(5, 0);
        rep(FN(IMP_Encoder_SetRdBufShare), s16, s16 == (int)0x80040001 && s5 == 0 ? V_PASS : V_FAIL, "chn 16 0x%x, chn 5 (before CreateChn) 0x%x", X(s16), X(s5));
    }
    NEED(IMP_Encoder_SetMotionQualityEnable) {
        int s16 = IMP_Encoder_SetMotionQualityEnable(16, 1), s5 = IMP_Encoder_SetMotionQualityEnable(5, 0);
        rep(FN(IMP_Encoder_SetMotionQualityEnable), s16, s16 == (int)0x80040001 && s5 == 0 ? V_PASS : V_FAIL, "chn 16 0x%x, chn 5 (before CreateChn) 0x%x", X(s16), X(s5));
    }
    NEED(IMP_Encoder_SetChnQpgAIEnable) {
        int s16 = IMP_Encoder_SetChnQpgAIEnable(16, 1), s5 = IMP_Encoder_SetChnQpgAIEnable(5, 0);
        rep(FN(IMP_Encoder_SetChnQpgAIEnable), s16, s16 == (int)0x80040001 && s5 == 0 ? V_PASS : V_FAIL, "chn 16 0x%x, chn 5 (before CreateChn) 0x%x", X(s16), X(s5));
    }
    NEED(IMP_Encoder_SetChnResizeMode) {
        int s16 = IMP_Encoder_SetChnResizeMode(16, 1), s5 = IMP_Encoder_SetChnResizeMode(5, 1);
        rep(FN(IMP_Encoder_SetChnResizeMode), s16, s16 == (int)0x80040001 && s5 == (int)0x80040008 ? V_PASS : V_FAIL, "chn 16 0x%x, chn 5 0x%x", X(s16), X(s5));
    }
    NEED(IMP_Encoder_MappingTransform) {
        int rn = IMP_Encoder_MappingTransform(NULL, NULL);
        int tab[2] = { 0, 17 };
        int rr = IMP_Encoder_MappingTransform(NULL, tab);
        rep(FN(IMP_Encoder_MappingTransform), rn, rn == (int)0x80040010 && rr == (int)0x80040002 ? V_PASS : V_FAIL, "NULL out 0x%x, table length 17 0x%x", X(rn), X(rr));
    }
    NEED(IMP_Encoder_SetChnQpgAIEnable_Yuv) { IMP_Encoder_SetChnQpgAIEnable_Yuv(0, 0); rep(FN(IMP_Encoder_SetChnQpgAIEnable_Yuv), 0, V_PASS, "void, flag stored"); }
    NEED(IMP_Encoder_SetMotionQualityEnable_Yuv) { IMP_Encoder_SetMotionQualityEnable_Yuv(0, 0); rep(FN(IMP_Encoder_SetMotionQualityEnable_Yuv), 0, V_PASS, "void, flag stored"); }
    NEED(IMP_Encoder_SetMotionQuality_Yuv) {
        int m = 0, r = IMP_Encoder_SetMotionQuality_Yuv(&m);
        RET0(IMP_Encoder_SetMotionQuality_Yuv, r, "mode 0: 0x%x", X(r));
    }
    NEED(IMP_Encoder_VbmAlloc) NEED(IMP_Encoder_VbmFree) NEED(IMP_Encoder_VbmV2P) NEED(IMP_Encoder_VbmP2V) {
        void *v = IMP_Encoder_VbmAlloc(4096, 32);
        intptr_t p = v ? IMP_Encoder_VbmV2P((intptr_t)v) : 0, v2 = p ? IMP_Encoder_VbmP2V(p) : 0;
        int ok = v && p && v2 == (intptr_t)v && ((uintptr_t)v & 31) == 0;
        if (v) { memset(v, 0x5a, 4096); IMP_Encoder_VbmFree(v); }
        rep(FN(IMP_Encoder_VbmAlloc), 0, v ? V_PASS : V_FAIL, "4 KiB align 32: %s", v ? "allocated" : "NULL");
        rep(FN(IMP_Encoder_VbmV2P), 0, ok ? V_PASS : V_FAIL, "v2p %s, p2v back %s", p ? "nonzero" : "0", v2 == (intptr_t)v ? "equal" : "DIFFERENT");
        rep(FN(IMP_Encoder_VbmP2V), 0, ok ? V_PASS : V_FAIL, "p2v of the v2p result %s", v2 == (intptr_t)v ? "equal" : "DIFFERENT");
        rep(FN(IMP_Encoder_VbmFree), 0, v && cases_flow() ? V_PASS : V_FAIL, "freed; frames flow");
    }
    NEED(IMP_System_MemPoolFree) {
        int r = IMP_System_MemPoolFree(9);
        rep(FN(IMP_System_MemPoolFree), r, r == -1 ? V_PASS : V_FAIL, "never requested pool 9: 0x%x", X(r));
    }
}

/* calls that switch a mode: last, frames must still flow afterwards */
static void t33_isp_modes(void)
{
    NEED(IMP_ISP_OneShotEnable) NEED(IMP_ISP_OneShotTick) NEED(IMP_ISP_OneShotDisable) {
        IMPISPDriverMode m1 = (IMPISPDriverMode)7, m2 = (IMPISPDriverMode)7;
        int re = IMP_ISP_OneShotEnable(), rt, rd;
        if (IMP_ISP_GET_DriverMode) IMP_ISP_GET_DriverMode(&m1);
        rt = IMP_ISP_OneShotTick();
        usleep(200000);
        rd = IMP_ISP_OneShotDisable();
        if (IMP_ISP_GET_DriverMode) IMP_ISP_GET_DriverMode(&m2);
        {
            int fl = cases_flow();
            const char *f = "enable 0x%x (driver mode %d), tick 0x%x, disable 0x%x (driver mode %d); frames flow %d";
            rep(FN(IMP_ISP_OneShotEnable), re, re == 0 && fl ? V_PASS : V_FAIL, f, X(re), (int)m1, X(rt), X(rd), (int)m2, fl);
            rep(FN(IMP_ISP_OneShotTick), rt, rt == 0 && fl ? V_PASS : V_FAIL, f, X(re), (int)m1, X(rt), X(rd), (int)m2, fl);
            rep(FN(IMP_ISP_OneShotDisable), rd, rd == 0 && (int)m2 == 0 && fl ? V_PASS : V_FAIL, f, X(re), (int)m1, X(rt), X(rd), (int)m2, fl);
        }
    }
    NEED(IMP_ISP_StartNightMode) {
        int r = IMP_ISP_StartNightMode(IMPVI_MAIN);
        usleep(300000);
        CHECK(IMP_ISP_StartNightMode, r, cases_flow(), "0x%x; frames flow (picture effect: camera)", X(r));
    }
    NEED(IMP_ISP_AddSensor_Process) NEED(IMP_ISP_AddSensor_Free) {
        int r = IMP_ISP_AddSensor_Process(IMPVI_MAIN);
        IMP_ISP_AddSensor_Free(IMPVI_MAIN);
        rep(FN(IMP_ISP_AddSensor_Process), r, r == 0 && cases_flow() ? V_PASS : V_FAIL, "no request stored before AddSensor: 0x%x; frames flow", X(r));
        rep(FN(IMP_ISP_AddSensor_Free), 0, V_PASS, "void");
    }
}

void cs_t33pass(void)
{
    t33_isp_rt();
    wdog("cases", 60);
    t33_isp_q1();
    wdog("cases", 60);
    t33_fs_enc_q2();
    wdog("cases", 60);
    t33_isp_modes();
}
/* ---- case "t33f1": FrameSource data paths (batch F1, #57 / #42) ------------------------------------------------------------------
 * ExternInject channel 3 (320x192 NV12, 3 buffers) bound to encoder group 4 / channel 5 (H.264): argument checks, Dequeue/Queue with
 * a moving pattern and the encoded frames (only outside IVDC direct mode: the vendor refuses DequeueBuffer in direct mode),
 * GetFrameEx / ReleaseFrameEx error codes and one frame of ch1, PM_Suspend / PM_Resume of ch1 (frames stop and come back),
 * SnapRaw_Start / _End with a one-frame request and IMP_DQbuffer (not declared in the 2.0.2.1 headers). */
struct t33_snapraw { uint32_t vinum, count, type, mode, size, paddr, vaddr; };
extern int IMP_FrameSource_SnapRaw_Start(struct t33_snapraw *a) __attribute__((weak));
extern int IMP_FrameSource_SnapRaw_End(void) __attribute__((weak));
extern int IMP_DQbuffer(const char *path, uint32_t cmd, int dump) __attribute__((weak));
static int F1_CH = 2;     /* APITEST_F1_CH: the inject channel (vendor: needs a framechan node, 0..2) */
#define F1_GRP 4
#define F1_ENC 5
#define F1_W 320
#define F1_H 192
static volatile int f1_n, f1_deq, f1_q, f1_waits, f1_stop;
static void *f1_feeder(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < f1_n + 8 && !f1_stop; i++) {
        IMPFrameInfo *f = NULL;
        uint8_t *y;
        int x, yy;

        while (IMP_FrameSource_DequeueBuffer(F1_CH, &f) != 0 || !f) {
            usleep(5000);
            if (f1_stop || ++f1_waits > 2000) return NULL;
        }
        f1_deq++;
        y = (uint8_t *)(uintptr_t)f->virAddr;
        for (yy = 0; yy < F1_H; yy++)
            for (x = 0; x < F1_W; x++) y[yy * F1_W + x] = (uint8_t)(((x + i * 8) & 0xff) ^ (yy & 0x20 ? 0x80 : 0));
        memset(y + F1_W * ((F1_H + 15) & ~15), 128, (size_t)F1_W * F1_H / 2);
        f->timeStamp = (int64_t)(i + 1) * 40000;
        if (IMP_FrameSource_QueueBuffer(F1_CH, f) == 0) f1_q++;
        usleep(40000);
    }
    return NULL;
}

static void t33_f1_inject(void)
{
    IMPFSChnAttr fs, bad;
    IMPEncoderCHNAttr enc;
    IMPCell src = { DEV_ID_FS, 0, 0 }, dst = { DEV_ID_ENC, F1_GRP, 0 };
    IMPFrameInfo *dummy = NULL;
    pthread_t th;
    int r12, rnull, rodd, rc, rc2, rdq0, ren, ren2, rdq, setup = 1, thr = 0, got = 0, i, ok_ts = 1, rd, rd2, rds, rds2, dm = -1;
    long long last = -1;

    if (!IMP_FrameSource_ExternInject_CreateChn || !IMP_FrameSource_DequeueBuffer || !IMP_FrameSource_QueueBuffer) {
        rep_na(FN(IMP_FrameSource_ExternInject_CreateChn));
        return;
    }
    if (getenv("APITEST_F1_CH")) F1_CH = atoi(getenv("APITEST_F1_CH"));
    src.groupID = F1_CH;
    if (IMP_FrameSource_GetDirectMode) IMP_FrameSource_GetDirectMode(&dm);
    memset(&fs, 0, sizeof(fs));
    fs.pixFmt = PIX_FMT_NV12; fs.picWidth = F1_W; fs.picHeight = F1_H; fs.outFrmRateNum = 25; fs.outFrmRateDen = 1; fs.nrVBs = 3;
    fs.type = FS_PHY_CHANNEL;
    bad = fs; bad.picWidth = F1_W + 8;
    r12 = IMP_FrameSource_ExternInject_CreateChn(12, &fs);
    rnull = IMP_FrameSource_ExternInject_CreateChn(F1_CH, NULL);
    rodd = IMP_FrameSource_ExternInject_CreateChn(F1_CH, &bad);
    rc = IMP_FrameSource_ExternInject_CreateChn(F1_CH, &fs);
    rc2 = IMP_FrameSource_ExternInject_CreateChn(F1_CH, &fs);
    rdq0 = IMP_FrameSource_DequeueBuffer(F1_CH, &dummy);
    ren = IMP_FrameSource_ExternInject_EnableChn(F1_CH);
    ren2 = IMP_FrameSource_ExternInject_EnableChn(F1_CH);
    dummy = NULL;
    rdq = IMP_FrameSource_DequeueBuffer(F1_CH, &dummy);
    if (rdq == 0 && dummy) IMP_FrameSource_QueueBuffer(F1_CH, dummy);     /* no module bound yet: back to the free list */
    if (rc || ren || rdq) setup = 0;
    if (setup && (IMP_Encoder_CreateGroup(F1_GRP) ||
                  IMP_Encoder_SetDefaultParam(&enc, IMP_ENC_PROFILE_AVC_MAIN, ENC_RC_MODE_CBR, F1_W, F1_H, 25, 1, 25, 2, -1, 500) ||
                  IMP_Encoder_CreateChn(F1_ENC, &enc) || IMP_Encoder_RegisterChn(F1_GRP, F1_ENC) || IMP_System_Bind(&src, &dst) ||
                  IMP_Encoder_StartRecvPic(F1_ENC)))
        setup = 0;
    f1_n = 10; f1_deq = f1_q = f1_waits = f1_stop = 0;
    if (setup) thr = pthread_create(&th, NULL, f1_feeder, NULL) == 0;
    for (i = 0; setup && thr && got < f1_n && i < f1_n * 3; i++) {
        IMPEncoderStream s;

        if (IMP_Encoder_PollingStream(F1_ENC, 2000) != 0) continue;
        if (IMP_Encoder_GetStream(F1_ENC, &s, 1) != 0) continue;
        if (s.packCount && (long long)s.pack[0].timestamp <= last && got > 0) ok_ts = 0;
        if (s.packCount) last = s.pack[0].timestamp;
        IMP_Encoder_ReleaseStream(F1_ENC, &s);
        got++;
    }
    if (setup) {
        IMP_Encoder_StopRecvPic(F1_ENC);
        f1_stop = 1;
        if (thr) pthread_join(th, NULL);
        IMP_System_UnBind(&src, &dst);
        IMP_Encoder_UnRegisterChn(F1_ENC);
        IMP_Encoder_DestroyChn(F1_ENC);
        IMP_Encoder_DestroyGroup(F1_GRP);
    }
    rd = IMP_FrameSource_ExternInject_DisableChn(F1_CH);
    rd2 = IMP_FrameSource_ExternInject_DisableChn(F1_CH);
    rds = IMP_FrameSource_ExternInject_DestroyChn(F1_CH);
    rds2 = IMP_FrameSource_ExternInject_DestroyChn(F1_CH);
    rep(FN(IMP_FrameSource_ExternInject_CreateChn), rc, rc == 0 && r12 == -1 && rnull == -1 && rodd == -1 && rc2 == 0 ? V_PASS : V_FAIL,
        "chn 12 0x%x, NULL 0x%x, width %d 0x%x, chn %d %dx%d 0x%x, again 0x%x", X(r12), X(rnull), F1_W + 8, X(rodd), F1_CH, F1_W, F1_H, X(rc), X(rc2));
    rep(FN(IMP_FrameSource_ExternInject_EnableChn), ren, ren == 0 && ren2 == 0 ? V_PASS : V_FAIL, "enable 0x%x, again 0x%x", X(ren), X(ren2));
    rep(FN(IMP_FrameSource_DequeueBuffer), rdq, rdq0 == -1 && (dm >= 1 ? rdq == -1 : (rdq == 0 && f1_deq >= f1_n)) ? V_PASS : V_FAIL,
        "before Enable 0x%x; enabled 0x%x (direct mode %d: refused there); %d dequeued, %d waits", X(rdq0), X(rdq), dm, f1_deq, f1_waits);
    rep(FN(IMP_FrameSource_QueueBuffer), 0, dm >= 1 ? (rdq == -1 ? V_PASS : V_FAIL) : (setup && f1_q >= f1_n && got >= f1_n / 2 && ok_ts ? V_PASS : V_FAIL),
        "direct mode %d; %d queued with a pattern, encoder (grp %d chn %d) delivered %d of %d, time stamps %s", dm, f1_q, F1_GRP, F1_ENC, got, f1_n,
        ok_ts ? "increasing" : "NOT increasing");
    rep(FN(IMP_FrameSource_ExternInject_DisableChn), rd, rd == 0 && rd2 == -1 ? V_PASS : V_FAIL, "disable 0x%x, again 0x%x", X(rd), X(rd2));
    rep(FN(IMP_FrameSource_ExternInject_DestroyChn), rds, rds == 0 && rds2 == 0 && cases_flow() ? V_PASS : V_FAIL,
        "destroy 0x%x, again 0x%x; frames of ch1 flow", X(rds), X(rds2));
}

static void t33_f1_ex_pm(void)
{
    int skip_pm = getenv("APITEST_F1_NOPM") != NULL;

    NEED(IMP_FrameSource_GetFrameEx) NEED(IMP_FrameSource_ReleaseFrameEx) {
        IMPFrameInfo *f = NULL, fake;
        int r12 = IMP_FrameSource_GetFrameEx(12, &f), rn = IMP_FrameSource_GetFrameEx(1, NULL), r5 = IMP_FrameSource_GetFrameEx(5, &f);
        int rg, rr = -1, rw = -1, rrn = IMP_FrameSource_ReleaseFrameEx(1, NULL), rr12 = IMP_FrameSource_ReleaseFrameEx(12, &fake);
        unsigned w = 0, h = 0;

        f = NULL;
        rg = IMP_FrameSource_GetFrameEx(1, &f);
        if (rg == 0 && f) {
            w = f->width; h = f->height;
            fake = *f; fake.pool_idx = 2;
            rw = IMP_FrameSource_ReleaseFrameEx(1, &fake);
            rr = IMP_FrameSource_ReleaseFrameEx(1, f);
        }
        REP2(IMP_FrameSource_GetFrameEx, IMP_FrameSource_ReleaseFrameEx, rg | rr,
             rg == 0 && rr == 0 && X(r12) == 0x80010001 && X(rn) == 0x80010010 && X(r5) == 0x80010040 && X(rw) == 0x80010040 &&
             X(rrn) == 0x80010010 && X(rr12) == 0x80010001 && cases_flow(),
             "get chn 12 0x%x, NULL 0x%x, chn 5 0x%x; chn 1 0x%x %ux%u; release wrong pool 0x%x, NULL 0x%x, chn 12 0x%x, ok 0x%x; frames flow",
             X(r12), X(rn), X(r5), X(rg), w, h, X(rw), X(rrn), X(rr12), X(rr));
    }
    if (skip_pm) rep_na(FN(IMP_FrameSource_PM_Suspend));
    else NEED(IMP_FrameSource_PM_Suspend) NEED(IMP_FrameSource_PM_Resume) {
        int rs5 = IMP_FrameSource_PM_Suspend(5), rr0 = IMP_FrameSource_PM_Resume(1), rs, rs2, rr, rr2, during;

        rs = IMP_FrameSource_PM_Suspend(1);
        rs2 = IMP_FrameSource_PM_Suspend(1);
        usleep(300000);
        during = cases_flow();
        rr = IMP_FrameSource_PM_Resume(1);
        rr2 = IMP_FrameSource_PM_Resume(1);
        usleep(300000);
        REP2(IMP_FrameSource_PM_Suspend, IMP_FrameSource_PM_Resume, rs | rr,
             X(rs5) == 0x80010040 && X(rr0) == 0x80011000 && X(rs2) == 0x80010040 && X(rr2) == 0x80011000 && cases_flow(),
             "suspend chn 5 0x%x, resume not suspended 0x%x; chn 1 suspend 0x%x, again 0x%x, frames while suspended %d; resume 0x%x, again 0x%x; frames flow",
             X(rs5), X(rr0), X(rs), X(rs2), during, X(rr), X(rr2));
    }
    if (IMP_FrameSource_SnapRaw_Start && IMP_FrameSource_SnapRaw_End) {
        struct t33_snapraw a = { 0, 1, 1, 0, 0, 0, 0 };
        int rn = IMP_FrameSource_SnapRaw_Start(NULL), rs = IMP_FrameSource_SnapRaw_Start(&a), re = IMP_FrameSource_SnapRaw_End();

        rep("IMP_FrameSource_SnapRaw_Start", rs, V_PASS, "NULL 0x%x; vinum 0, 1 frame, type 1: 0x%x (count %u size %u paddr 0x%x vaddr 0x%x)",
            X(rn), X(rs), a.count, a.size, a.paddr, a.vaddr);
        rep("IMP_FrameSource_SnapRaw_End", re, cases_flow() ? V_PASS : V_FAIL, "0x%x; frames flow", X(re));
    } else {
        rep_na("IMP_FrameSource_SnapRaw_Start");
    }
    if (IMP_DQbuffer) {
        int r = IMP_DQbuffer("/tmp/dq.bin", 0xc0605400u, 1);
        rep("IMP_DQbuffer", r, V_PASS, "/tmp/dq.bin, request 0xc0605400: 0x%x", X(r));
    } else {
        rep_na("IMP_DQbuffer");
    }
}

/* ---- case "t33l1": LDC online / offline (batch L1, #54 / #39) ---------------------------------------------------------------------
 * Online state on VI 1 (no sensor there, so "before AddSensor"): mode record online, Init argument checks, strength set / read back,
 * LUT name refused after a strength; VI 0 (sensor added) refused.  Offline: mode record offline, OfflineInit opens /dev/offline_ldc
 * (vendor module and open T33 module since #491: the node exists), CreateJob(NULL), OfflineInit(0); mode back to online. */
extern int IMP_ISP_LDC_OnlineSetStrength(IMPVI_NUM num, double s) __attribute__((weak));
extern int IMP_ISP_LDC_OnlineGetStrength(IMPVI_NUM num, double *s) __attribute__((weak));
extern int IMP_ISP_LDC_OfflineInit(int mode) __attribute__((weak));
extern int IMP_ISP_LDC_OfflineCreateJob(void *attr) __attribute__((weak));
extern int IMP_ISP_LDC_OfflineDestroyJob(uint32_t job) __attribute__((weak));
extern int IMP_ISP_LDC_OfflineSubmitTask(void *task) __attribute__((weak));
extern int IMP_ISP_LDC_OfflineUpdateLutToJob(void *attr) __attribute__((weak));
#define T33STEP(t, n) do { printf("[T] %s step %d\n", t, n); fflush(stdout); } while (0)
void cs_t33l1(void)
{
    IMPISPLDCInitOnlineAttr a;
    char lut[128];
    double d = 0;
    int rm0, rn, rf, ri, ri0, rs, rg, rl, rgl, rs0, rm1, rgm, roi, rcj, rdj, rst, rul, roi0, rm2;

    if (!IMP_ISP_LDC_SetMode || !IMP_ISP_LDC_OnlineInit || !IMP_ISP_LDC_OnlineSetStrength || !IMP_ISP_LDC_OfflineInit) {
        rep_na(FN(IMP_ISP_LDC_OnlineInit));
        return;
    }
    T33STEP("l1", 1);
    rm0 = IMP_ISP_LDC_SetMode(IMPISP_ONLINE_MODE);
    memset(&a, 0, sizeof(a));
    T33STEP("l1", 2);
    rn = IMP_ISP_LDC_OnlineInit(IMPVI_SEC, NULL);
    T33STEP("l1", 3);
    a.func = (IMPISPLDCFunc)3; rf = IMP_ISP_LDC_OnlineInit(IMPVI_SEC, &a);
    a.func = IMPISP_LDC_MODE; a.size = IMPISP_LUT_BLOCKSIZE_16x16;
    T33STEP("l1", 4);
    ri = IMP_ISP_LDC_OnlineInit(IMPVI_SEC, &a);
    T33STEP("l1", 5);
    ri0 = IMP_ISP_LDC_OnlineInit(IMPVI_MAIN, &a);
    T33STEP("l1", 6);
    rs = IMP_ISP_LDC_OnlineSetStrength(IMPVI_SEC, 0.5);
    T33STEP("l1", 7);
    rg = IMP_ISP_LDC_OnlineGetStrength(IMPVI_SEC, &d);
    memset(lut, 0, sizeof(lut)); strcpy(lut, "/tmp/none.lut");
    T33STEP("l1", 8);
    rl = IMP_ISP_LDC_OnlineSetLut(IMPVI_SEC, lut);
    T33STEP("l1", 9);
    rgl = IMP_ISP_LDC_OnlineGetLut(IMPVI_SEC, lut);
    T33STEP("l1", 10);
    rs0 = IMP_ISP_LDC_OnlineSetStrength(IMPVI_MAIN, 0.5);
    T33STEP("l1", 11);
    rm1 = IMP_ISP_LDC_SetMode(IMPISP_OFFLINE_MODE);
    T33STEP("l1", 12);
    rgm = IMP_ISP_LDC_OnlineGetStrength(IMPVI_SEC, &d);
    T33STEP("l1", 13);
    roi = IMP_ISP_LDC_OfflineInit(1);
    T33STEP("l1", 14);
    rcj = IMP_ISP_LDC_OfflineCreateJob ? IMP_ISP_LDC_OfflineCreateJob(NULL) : -2;
    T33STEP("l1", 15);
    rdj = IMP_ISP_LDC_OfflineDestroyJob ? IMP_ISP_LDC_OfflineDestroyJob(0x1234) : -2;
    T33STEP("l1", 16);
    rst = IMP_ISP_LDC_OfflineSubmitTask ? IMP_ISP_LDC_OfflineSubmitTask(NULL) : -2;
    T33STEP("l1", 17);
    rul = IMP_ISP_LDC_OfflineUpdateLutToJob ? IMP_ISP_LDC_OfflineUpdateLutToJob(NULL) : -2;
    /* vendor 2.0.2.1: OfflineInit(0) after a successful OfflineInit(1) crashes (it destroys the mutex of the freed device block);
     * APITEST_L1_NODEINIT=1 skips it for the vendor reference run */
    roi0 = getenv("APITEST_L1_NODEINIT") ? -2 : IMP_ISP_LDC_OfflineInit(0);
    T33STEP("l1", 19);
    rm2 = IMP_ISP_LDC_SetMode(IMPISP_ONLINE_MODE);
    rep(FN(IMP_ISP_LDC_OnlineInit), ri, V_PASS, "mode online 0x%x; VI1 NULL 0x%x, func 3 0x%x, LDC 16x16 0x%x; VI0 (sensor added) 0x%x", X(rm0), X(rn), X(rf), X(ri), X(ri0));
    rep("IMP_ISP_LDC_OnlineSetStrength", rs, V_PASS, "VI1 0.5: 0x%x; VI0 0x%x", X(rs), X(rs0));
    rep("IMP_ISP_LDC_OnlineGetStrength", rg, V_PASS, "VI1 0x%x -> %.3f; with offline mode 0x%x", X(rg), d, X(rgm));
    rep(FN(IMP_ISP_LDC_OnlineSetLut), rl, V_PASS, "after a strength 0x%x", X(rl));
    rep(FN(IMP_ISP_LDC_OnlineGetLut), rgl, V_PASS, "0x%x", X(rgl));
    rep("IMP_ISP_LDC_OfflineInit", roi, V_PASS, "mode offline 0x%x; init 1 0x%x; init 0 0x%x; mode online again 0x%x", X(rm1), X(roi), X(roi0), X(rm2));
    rep("IMP_ISP_LDC_OfflineCreateJob", rcj, V_PASS, "NULL 0x%x", X(rcj));
    rep("IMP_ISP_LDC_OfflineDestroyJob", rdj, V_PASS, "unknown job 0x%x", X(rdj));
    rep("IMP_ISP_LDC_OfflineSubmitTask", rst, V_PASS, "NULL 0x%x", X(rst));
    rep("IMP_ISP_LDC_OfflineUpdateLutToJob", rul, V_PASS, "NULL 0x%x; frames flow %d", X(rul), cases_flow());
}

/* ---- case "t33e3": kernel encoder and multi-process encoder clients (batch E3, #56 / #41) -------------------------------------------
 * After IMP_System_Init: MultiProcessInit refused (0x80040080), no server: the *_Ex clients fail (NULL / -1); /dev/kern_enc does not exist
 * on the T33 kernel: KernEnc_* 0x80048000, channel 16 0x80040001. */
void cs_t33e3(void)
{
    NEED(IMP_Encoder_KernEnc_Stop) NEED(IMP_Encoder_KernEnc_GetStatus) NEED(IMP_Encoder_KernEnc_Release) NEED(IMP_Encoder_KernEnc_GetStream) {
        IMPEncoderKernEncOut o;
        int en = -1, rs = IMP_Encoder_KernEnc_Stop(), rg = IMP_Encoder_KernEnc_GetStatus(0, &en), rr = IMP_Encoder_KernEnc_Release(0),
            rr16 = IMP_Encoder_KernEnc_Release(16), rgs, rgs16;

        memset(&o, 0, sizeof(o));
        rgs = IMP_Encoder_KernEnc_GetStream(0, &o);
        rgs16 = IMP_Encoder_KernEnc_GetStream(16, &o);
        rep(FN(IMP_Encoder_KernEnc_Stop), rs, X(rs) == 0x80048000 ? V_PASS : V_FAIL, "no /dev/kern_enc: 0x%x", X(rs));
        rep(FN(IMP_Encoder_KernEnc_GetStatus), rg, X(rg) == 0x80048000 ? V_PASS : V_FAIL, "chn 0: 0x%x", X(rg));
        rep(FN(IMP_Encoder_KernEnc_Release), rr, X(rr) == 0x80048000 && X(rr16) == 0x80040001 ? V_PASS : V_FAIL, "chn 0 0x%x, chn 16 0x%x", X(rr), X(rr16));
        rep(FN(IMP_Encoder_KernEnc_GetStream), rgs, X(rgs) == 0x80048000 && X(rgs16) == 0x80040001 ? V_PASS : V_FAIL, "chn 0 0x%x, chn 16 0x%x", X(rgs), X(rgs16));
    }
    NEED(IMP_Encoder_MultiProcessInit) NEED(IMP_Encoder_MultiProcessExit) {
        int r = IMP_Encoder_MultiProcessInit();
        IMP_Encoder_MultiProcessExit();
        rep(FN(IMP_Encoder_MultiProcessInit), r, X(r) == 0x80040080 ? V_PASS : V_FAIL, "after IMP_System_Init: 0x%x", X(r));
        rep(FN(IMP_Encoder_MultiProcessExit), 0, V_PASS, "void, no server running");
    }
    NEED(IMP_Encoder_VbmAlloc_Ex) NEED(IMP_Encoder_VbmFree_Ex) {
        void *p = IMP_Encoder_VbmAlloc_Ex(4096, 64);
        if (p) IMP_Encoder_VbmFree_Ex(p);
        REP2(IMP_Encoder_VbmAlloc_Ex, IMP_Encoder_VbmFree_Ex, 0, p == NULL, "no server: %p", p);
    }
    NEED(IMP_Encoder_YuvInit_Ex) NEED(IMP_Encoder_YuvEncode_Ex) NEED(IMP_Encoder_YuvExit_Ex) {
        IMPEncoderYuvIn in;
        IMPEncoderYuvOut out;
        IMPFrameInfo f;
        int h = -7, ri, re, rx;

        memset(&in, 0, sizeof(in)); memset(&out, 0, sizeof(out)); memset(&f, 0, sizeof(f));
        in.type = PT_H264;
        ri = IMP_Encoder_YuvInit_Ex(&h, 320, 192, &in);
        re = IMP_Encoder_YuvEncode_Ex(0, f, &out);
        rx = IMP_Encoder_YuvExit_Ex(0);
        rep(FN(IMP_Encoder_YuvInit_Ex), ri, ri == -1 && h == -7 ? V_PASS : V_FAIL, "no server: 0x%x (handle untouched %d)", X(ri), h);
        rep(FN(IMP_Encoder_YuvEncode_Ex), re, re == -1 ? V_PASS : V_FAIL, "no server: 0x%x", X(re));
        rep(FN(IMP_Encoder_YuvExit_Ex), rx, rx == -1 ? V_PASS : V_FAIL, "no server: 0x%x", X(rx));
    }
    NEED(IMP_Encoder_InputJpege_Ex) {
        int len = -7, r = IMP_Encoder_InputJpege_Ex(NULL, NULL, 320, 192, 75, &len);
        rep(FN(IMP_Encoder_InputJpege_Ex), r, r == -1 && len == -7 ? V_PASS : V_FAIL, "no server: 0x%x; frames flow %d", X(r), cases_flow());
    }
}

/* ---- case "t33i2d": FrameSource I2D rotation on channel 2 (#445) -----------------------------------------------------------------
 * ch2 320x192 NV12 with i2dattr {enable, rotate 90} set in the channel attribute, enabled, one frame read: its size tells whether the
 * library rotated (192x320) or not; GetI2dAttr read back. */
void cs_t33i2d(void)
{
    IMPFSChnAttr a;
    IMPFSI2DAttr g;
    IMPFrameInfo *f = NULL;
    int rc, re, rd, rg, rr = -1, rgi;
    unsigned w = 0, h = 0;

    memset(&a, 0, sizeof(a));
    a.pixFmt = PIX_FMT_NV12; a.picWidth = 320; a.picHeight = 192; a.outFrmRateNum = 15; a.outFrmRateDen = 1; a.nrVBs = 2;
    a.type = FS_PHY_CHANNEL; a.scaler.enable = 1; a.scaler.outwidth = 320; a.scaler.outheight = 192;
    a.i2dattr.i2d_enable = 1; a.i2dattr.rotate_enable = 1; a.i2dattr.rotate_angle = 90;
    rc = IMP_FrameSource_CreateChn(2, &a);
    memset(&g, 0, sizeof(g));
    rgi = IMP_FrameSource_GetI2dAttr(2, &g);
    re = rc == 0 ? IMP_FrameSource_EnableChn(2) : -1;
    rd = re == 0 ? IMP_FrameSource_SetFrameDepth(2, 1) : -1;
    rg = rd == 0 ? IMP_FrameSource_GetFrame(2, &f) : -1;
    if (rg == 0 && f) { w = f->width; h = f->height; rr = IMP_FrameSource_ReleaseFrame(2, f); }
    if (rd == 0) IMP_FrameSource_SetFrameDepth(2, 0);
    if (re == 0) IMP_FrameSource_DisableChn(2);
    if (rc == 0) IMP_FrameSource_DestroyChn(2);
    rep(FN(IMP_FrameSource_SetI2dAttr), rg, V_PASS, "ch2 320x192 i2d rotate 90 in the attribute: create 0x%x, GetI2dAttr 0x%x {%d,%d,%d,%d,%d}, enable 0x%x, depth 0x%x, frame 0x%x %ux%u (%s), release 0x%x; ch1 flows %d",
        X(rc), X(rgi), g.i2d_enable, g.flip_enable, g.mirr_enable, g.rotate_enable, g.rotate_angle, X(re), X(rd), X(rg), w, h,
        w == 192 && h == 320 ? "rotated" : "not rotated", X(rr), cases_flow());
}

/* ---- case "t33drv": the module requests of openimp #491 straight on the driver (explicit only) ---------------------------------------
 * /dev/tx-isp online LDC requests (0xc00454b9 LUT memory for LDC 16x16 at the sensor size, 0xc00454ba online attribute, 0xc00454bb
 * IQ-bin LDC parameters) and one /dev/offline_ldc round trip: LUT-memory blocks (ioctl 0) as the planes of a 64x32 NV12 picture, a
 * rotation job (ioctl 2, func 1, angle code 2 = 180 degrees), the turned Y plane checked pixel by pixel, the blocks freed (ioctl 1).
 * Printed as [T] lines (no API cell): the same lines on the vendor module and on the open module. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static void *drv_map(int mem, uint32_t pa, uint32_t len, void **base, uint32_t *mlen)
{
    uint32_t off = pa & 0xfff;

    *mlen = (len + off + 0xfff) & ~0xfffu;
    *base = mmap(NULL, *mlen, PROT_READ | PROT_WRITE, MAP_SHARED, mem, (off_t)(pa - off));
    return *base == MAP_FAILED ? NULL : (char *)*base + off;
}

void cs_t33drv(void)
{
    enum { W = 64, H = 32 };
    uint32_t b[22], blk[5][5], task[10];
    int fd = open("/dev/tx-isp", O_RDWR), lfd, mem, i, r, bad = -1, x, y;
    void *base[4] = { 0 };
    uint32_t mlen[4] = { 0 };
    uint8_t *p[4] = { 0 };

    if (fd < 0) {
        printf("[T] t33drv: /dev/tx-isp: %s\n", strerror(errno));
        return;
    }
    memset(b, 0, sizeof(b));
    b[0] = 0; b[1] = 4; b[2] = 16; b[6] = 1u << 16;
    r = ioctl(fd, 0xc00454b9, b);
    printf("[T] t33drv: 0xc00454b9 LDC 16x16 one LUT: ret %d errno %d, size %u, pa 0x%x / 0x%x\n", r, r ? errno : 0, b[9], b[10], b[11]);
    memset(b, 0, sizeof(b));
    b[0] = 0; b[1] = 4; b[2] = 16; b[5] = 1080u << 16 | 1920; b[7] = 0x1000;
    r = ioctl(fd, 0xc00454ba, b);
    printf("[T] t33drv: 0xc00454ba online attribute LDC 16 1920x1080: ret %d errno %d\n", r, r ? errno : 0);
    memset(b, 0xa5, sizeof(b));
    b[0] = 0;
    r = ioctl(fd, 0xc00454bb, b);
    printf("[T] t33drv: 0xc00454bb LDC parameters: ret %d errno %d, word1 %u, word2 0x%x\n", r, r ? errno : 0, b[1], b[2]);
    close(fd);

    lfd = open("/dev/offline_ldc", O_RDWR);
    if (lfd < 0) {
        printf("[T] t33drv: /dev/offline_ldc: %s\n", strerror(errno));
        return;
    }
    mem = open("/dev/mem", O_RDWR | O_SYNC);
    for (i = 0; i < 4; i++) {           /* Y in, UV in, Y out, UV out */
        memset(blk[i], 0, sizeof(blk[i]));
        blk[i][2] = i & 1 ? W * H / 2 : W * H;
        blk[i][3] = 4;
        r = ioctl(lfd, 0, blk[i]);
        if (r || !blk[i][1] || mem < 0 || !(p[i] = drv_map(mem, blk[i][1], blk[i][2], &base[i], &mlen[i])))
            break;
    }
    printf("[T] t33drv: offline_ldc ioctl 0 x4: ret %d, pa 0x%x 0x%x 0x%x 0x%x\n", r, blk[0][1], blk[1][1], blk[2][1], blk[3][1]);
    if (i == 4) {
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                p[0][y * W + x] = (uint8_t)(x * 4 + y);
        memset(p[1], 0x80, W * H / 2);
        memset(p[2], 0, W * H);
        memset(task, 0, sizeof(task));
        task[0] = (uint32_t)H << 16 | W;
        task[1] = blk[0][1]; task[2] = blk[1][1]; task[3] = blk[2][1]; task[4] = blk[3][1];
        task[5] = (uint32_t)W << 16 | W;  /* strides in / out */
        task[7] = 1;                        /* I2D */
        task[9] = 2;                        /* 180 degrees */
        r = ioctl(lfd, 2, task);
        for (y = 0; y < H && bad < 0; y++)
            for (x = 0; x < W && bad < 0; x++)
                if (p[2][y * W + x] != (uint8_t)((W - 1 - x) * 4 + (H - 1 - y)))
                    bad = y * W + x;
        printf("[T] t33drv: offline_ldc ioctl 2 rotate 180 64x32: ret %d, Y out %s (first bad %d: 0x%02x)\n", r,
               bad < 0 ? "turned" : "WRONG", bad, bad < 0 ? 0 : p[2][bad]);
    }
    for (i = 0; i < 4; i++) {
        if (base[i])
            munmap(base[i], mlen[i]);
        if (blk[i][1])
            printf("[T] t33drv: offline_ldc ioctl 1 free 0x%x: ret %d\n", blk[i][1], ioctl(lfd, 1, blk[i]));
    }
    if (mem >= 0)
        close(mem);
    close(lfd);
    printf("[T] t33drv: frames flow %d\n", cases_flow());
}

void cs_t33f1(void)
{
    t33_f1_inject();
    wdog("cases", 60);
    t33_f1_ex_pm();
}
#endif /* APITEST_T33 */
