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
#endif /* APITEST_T33 */
