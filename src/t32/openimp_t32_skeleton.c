/*
 * OpenIMP T32 platform skeleton (#286, planned; not a release gate).
 *
 * Builds against the OpenIMP public headers with PLATFORM_T32 so that every
 * T32 layout assert in include/imp and openimp_t32_abi.h is checked, and
 * exports the System / ISP / FrameSource / Encoder / OSD entry points of
 * the stream path.  Nothing talks to the kernel yet: each call logs once
 * per function and returns -1.  The real code follows in the T32 issues
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

T32_STUB(IMP_System_Init, (void))
T32_STUB(IMP_System_Exit, (void))
T32_STUB(IMP_System_Bind, (IMPCell *srcCell, IMPCell *dstCell))
T32_STUB(IMP_System_UnBind, (IMPCell *srcCell, IMPCell *dstCell))
T32_STUB(IMP_System_RebaseTimeStamp, (uint64_t basets))

/* ---- ISP (vendor 2.x: /dev/tx-isp, /dev/isp-m0) ------------------------- */
T32_STUB(IMP_ISP_Open, (void))
T32_STUB(IMP_ISP_Close, (void))
T32_STUB(IMP_ISP_AddSensor, (IMPVI_NUM num, IMPSensorInfo *info))
T32_STUB(IMP_ISP_DelSensor, (IMPVI_NUM num, IMPSensorInfo *info))
T32_STUB(IMP_ISP_EnableSensor, (IMPVI_NUM num, IMPSensorInfo *info))
T32_STUB(IMP_ISP_DisableSensor, (IMPVI_NUM num))
T32_STUB(IMP_ISP_EnableTuning, (void))
T32_STUB(IMP_ISP_DisableTuning, (void))
T32_STUB(IMP_ISP_Tuning_SetSensorFPS, (uint32_t fps_num, uint32_t fps_den))
T32_STUB(IMP_ISP_Tuning_GetSensorFPS, (uint32_t *fps_num, uint32_t *fps_den))
T32_STUB(IMP_ISP_Tuning_SetISPRunningMode, (IMPISPRunningMode mode))
T32_STUB(IMP_ISP_Tuning_GetISPRunningMode, (IMPISPRunningMode *pmode))

/* ---- FrameSource (vendor 2.x: /dev/framechanN, base 130) ---------------- */
T32_STUB(IMP_FrameSource_CreateChn, (int chnNum, IMPFSChnAttr *chn_attr))
T32_STUB(IMP_FrameSource_DestroyChn, (int chnNum))
T32_STUB(IMP_FrameSource_SetChnAttr, (int chnNum, IMPFSChnAttr *chn_attr))
T32_STUB(IMP_FrameSource_GetChnAttr, (int chnNum, IMPFSChnAttr *chn_attr))
T32_STUB(IMP_FrameSource_EnableChn, (int chnNum))
T32_STUB(IMP_FrameSource_DisableChn, (int chnNum))
T32_STUB(IMP_FrameSource_SetFrameDepth, (int chnNum, int depth))
T32_STUB(IMP_FrameSource_GetFrame, (int chnNum, void **frame))
T32_STUB(IMP_FrameSource_ReleaseFrame, (int chnNum, void *frame))

/* ---- Encoder (T23-family API; HERA + JPEG v1.2 behind /dev/soc_vpu) ----- */
T32_STUB(IMP_Encoder_CreateGroup, (int encGroup))
T32_STUB(IMP_Encoder_DestroyGroup, (int encGroup))
T32_STUB(IMP_Encoder_CreateChn, (int encChn, IMPEncoderCHNAttr *attr))
T32_STUB(IMP_Encoder_DestroyChn, (int encChn))
T32_STUB(IMP_Encoder_GetChnAttr, (int encChn, IMPEncoderChnAttr *attr))
T32_STUB(IMP_Encoder_RegisterChn, (int encGroup, int encChn))
T32_STUB(IMP_Encoder_UnRegisterChn, (int encChn))
T32_STUB(IMP_Encoder_StartRecvPic, (int encChn))
T32_STUB(IMP_Encoder_StopRecvPic, (int encChn))
T32_STUB(IMP_Encoder_PollingStream, (int encChn, uint32_t timeoutMsec))
T32_STUB(IMP_Encoder_GetStream, (int encChn, IMPEncoderStream *stream, int block))
T32_STUB(IMP_Encoder_ReleaseStream, (int encChn, IMPEncoderStream *stream))
T32_STUB(IMP_Encoder_RequestIDR, (int encChn))
T32_STUB(IMP_Encoder_FlushStream, (int encChn))
T32_STUB(IMP_Encoder_SetDefaultParam, (IMPEncoderChnAttr *attr, IMPEncoderProfile profile,
                                       IMPEncoderRcMode rcMode, int width, int height,
                                       int fpsNum, int fpsDen, int gopLen,
                                       int maxSameSceneCnt, int quality, int bitrate))

/* ---- OSD (IPU OSD; T32 region attribute is 272 bytes, see abi.h) -------- */
T32_STUB(IMP_OSD_SetPoolSize, (int size))
T32_STUB(IMP_OSD_CreateGroup, (int grpNum))
T32_STUB(IMP_OSD_DestroyGroup, (int grpNum))
T32_STUB(IMP_OSD_CreateRgn, (IMPOSDRgnAttr *prAttr))
T32_STUB(IMP_OSD_DestroyRgn, (IMPRgnHandle handle))
T32_STUB(IMP_OSD_RegisterRgn, (IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr))
T32_STUB(IMP_OSD_UnRegisterRgn, (IMPRgnHandle handle, int grpNum))
T32_STUB(IMP_OSD_SetRgnAttr, (IMPRgnHandle handle, IMPOSDRgnAttr *prAttr))
T32_STUB(IMP_OSD_GetRgnAttr, (IMPRgnHandle handle, IMPOSDRgnAttr *prAttr))
T32_STUB(IMP_OSD_UpdateRgnAttrData, (IMPRgnHandle handle, IMPOSDRgnAttrData *prAttrData))
T32_STUB(IMP_OSD_SetGrpRgnAttr, (IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr))
T32_STUB(IMP_OSD_GetGrpRgnAttr, (IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr))
T32_STUB(IMP_OSD_ShowRgn, (IMPRgnHandle handle, int grpNum, int showFlag))
T32_STUB(IMP_OSD_Start, (int grpNum))
T32_STUB(IMP_OSD_Stop, (int grpNum))
