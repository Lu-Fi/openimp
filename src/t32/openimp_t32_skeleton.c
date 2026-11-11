/*
 * OpenIMP T32 platform skeleton (#286, planned; not a release gate).
 *
 * Builds against the OpenIMP public headers with PLATFORM_T32 so that every
 * T32 layout assert in include/imp and openimp_t32_abi.h is checked, and
 * exports the System / ISP / FrameSource / Encoder / OSD entry points of
 * the stream path.  The entry points not implemented yet log once per
 * function and return -1; the ISP / sensor lifecycle and System_Init/Exit
 * are real (openimp_t32_isp.c, #288), FrameSource and bind too
 * (openimp_t32_fs.c, #290), the soc_vpu layer and the JPEG encoder too
 * (openimp_t32_vpu.c #291, openimp_t32_encoder.c #292).  The real code follows in the T32 issues
 * (ioctl table, ISP lifecycle, IQ bin, FrameSource, soc_vpu, JPEG, HERA).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_osd.h>
#include <imp/imp_audio.h>

#include "imp_log_int.h"
#include "t32/openimp_t32_abi.h"
#include "t32/openimp_t32_ioctl.h"      /* request table asserts on the target (#287) */

#if !defined(PLATFORM_T32)
#error "openimp_t32_skeleton.c is the PLATFORM_T32 build only"
#endif

/* T41-equal audio objects (PLAN section 1) */
_Static_assert(sizeof(IMPAudioIOAttr) == 24, "T32 IMPAudioIOAttr ABI mismatch");
_Static_assert(sizeof(IMPAudioFrame) == 32, "T32 IMPAudioFrame ABI mismatch");
_Static_assert(sizeof(IMPAudioIChnParam) == 12, "T32 IMPAudioIChnParam ABI mismatch");
/* the rest of section 1, repeated here so one object shows the whole table */
_Static_assert(sizeof(IMPSensorInfo) == 116, "T32 IMPSensorInfo");
_Static_assert(sizeof(IMPFSChnAttr) == 104, "T32 IMPFSChnAttr");
_Static_assert(sizeof(IMPFrameInfo) == 48, "T32 IMPFrameInfo");
_Static_assert(sizeof(IMPEncoderStream) == 16, "T32 IMPEncoderStream");
_Static_assert(sizeof(IMPEncoderPack) == 32, "T32 IMPEncoderPack");
_Static_assert(sizeof(IMPEncoderCHNAttr) == 184, "T32 IMPEncoderCHNAttr");
_Static_assert(sizeof(IMPEncoderRcAttr) == 132, "T32 IMPEncoderRcAttr");
_Static_assert(sizeof(IMPEncoderAttrRcMode) == 64, "T32 IMPEncoderAttrRcMode");

#define T32_TAG "T32"

/* one line per function and process: "not implemented yet" */
static void t32_missing(const char *fn, int *logged)
{
    if (!*logged) {
        *logged = 1;
        IMP_LOG_ERR(T32_TAG, "%s: not implemented on T32 yet (platform skeleton)", fn);
    }
}

/* stubs keep the vendor parameter lists; the parameters are unused for now */
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define T32_STUB(name, params)                      \
    int name params                                 \
    {                                               \
        static int logged;                          \
        t32_missing(__func__, &logged);             \
        return -1;                                  \
    }

/* ---- System ------------------------------------------------------------ */
int IMP_System_GetVersion(IMPVersion *pstVersion)
{
    static const char text[] = "libimp.so T32 openimp (skeleton)";

    if (!pstVersion)
        return -1;
    memset(pstVersion->aVersion, 0, sizeof(pstVersion->aVersion));
    memcpy(pstVersion->aVersion, text, sizeof(text));
    return 0;
}

const char *IMP_System_GetCPUInfo(void)
{
    return "T32";
}

uint64_t IMP_System_GetTimeStamp(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* IMP_System_Init/Exit: openimp_t32_isp.c (#288) */
/* IMP_System_Bind/UnBind: openimp_t32_fs.c (#290) */
T32_STUB(IMP_System_RebaseTimeStamp, (uint64_t basets))

/* ---- ISP (vendor 2.x: /dev/tx-isp, /dev/isp-m0) ------------------------- */
/* Open/Close, Add/Del/Enable/DisableSensor: openimp_t32_isp.c (#288) */
T32_STUB(IMP_ISP_EnableTuning, (void))
T32_STUB(IMP_ISP_DisableTuning, (void))
T32_STUB(IMP_ISP_Tuning_SetSensorFPS, (uint32_t fps_num, uint32_t fps_den))
T32_STUB(IMP_ISP_Tuning_GetSensorFPS, (uint32_t *fps_num, uint32_t *fps_den))

/* ---- FrameSource (vendor 2.x: /dev/framechanN, base 130) ---------------- */
/* Create/Destroy/Set/GetChnAttr, Enable/DisableChn, Set/GetFrameDepth,
 * GetFrame/ReleaseFrame: openimp_t32_fs.c (#290) */

/* ---- Encoder (T23-family API; HERA + JPEG v1.2 behind /dev/soc_vpu) ----- */
/* groups, channels, streams, JPEG on the JPEG v1.2 engine: openimp_t32_encoder.c
 * (#292) over the soc_vpu channel layer openimp_t32_vpu.c (#291); H.264 on HERA
 * (#293: openimp_t32_hera.c, openimp_t32_h264.c); H.265 follows in a later issue */

/* ---- OSD ------------------------------------------------------------------ */
/* IPU OSD (groups, regions, covers, pictures incl. 2-bit, CPU lines and
 * mosaics, the FS -> OSD -> encoder chain): the shared src/t23 OSD module in
 * the T32 2.x layout (272-byte IMPOSDRgnAttr, 200-byte jz_ipu_v13 request), #297.
 * ISP-drawn OSD (MSCA OSD windows): open-tx-isp driver/t32 has the register
 * pusher (tx_isp_t32_msca_osd.h) but no request that reaches it yet, so these
 * stay stubs (#297 note). */
T32_STUB(IMP_OSD_SetRgnAttr_ISP, (int sensornum, IMPOSDRgnAttr *prAttr, int bosdshow))
T32_STUB(IMP_OSD_GetRgnAttr_ISP, (int sensornum, IMPOSDRgnAttr *prAttr, int *pbosdshow))
T32_STUB(IMP_OSD_Init_ISP, (void))
T32_STUB(IMP_OSD_SetPoolSize_ISP, (int size))
T32_STUB(IMP_OSD_CreateRgn_ISP, (int chn, IMPIspOsdAttrAsm *attr))
T32_STUB(IMP_OSD_DestroyRgn_ISP, (int chn, IMPRgnHandle handle))
T32_STUB(IMP_OSD_SetRgnAttr_PicISP, (int chn, IMPRgnHandle handle, IMPIspOsdAttrAsm *attr))
T32_STUB(IMP_OSD_GetRgnAttr_ISPPic, (int chn, IMPRgnHandle handle, IMPIspOsdAttrAsm *attr))
T32_STUB(IMP_OSD_ShowRgn_ISP, (int chn, IMPRgnHandle handle, int showFlag))
void IMP_OSD_Exit_ISP(void)
{
}
