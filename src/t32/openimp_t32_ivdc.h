/*
 * OpenIMP T32 IVDC direct mode (#301, planned): shared by System,
 * FrameSource and the encoder (openimp_t32_ivdc.c).
 */
#ifndef OPENIMP_T32_IVDC_H
#define OPENIMP_T32_IVDC_H

#include <stdint.h>

/* a frame over IVDC: the encoders' source addresses (no memory behind them) */
#define OI_T32_IVDC_SRC_Y      0x80000000u
#define OI_T32_IVDC_SRC_C      0x81000000u
#define OI_T32_IVDC_STRIDE     4096u
#define OI_T32_FRAME_TYPE_IVDC 3u       /* soc_vpu channel_node frame_type */

enum { OI_T32_IVDC_JPEG, OI_T32_IVDC_H264, OI_T32_IVDC_H265 };

/* System_Init with the GET_DIRECT answer / System_Exit; 0 or -1 */
int OpenIMP_T32_IvdcSystemInit(int mode);
void OpenIMP_T32_IvdcSystemExit(void);
/* the GET_DIRECT answer of this System_Init (0 = memory mode) */
int OpenIMP_T32_IvdcMode(void);
/* an ioctl on the kept /dev/misc-ivdc: 0 or -1 */
int OpenIMP_T32_IvdcIoctl(uint32_t req, int32_t *arg, const char *name);
/* IVDC register by offset: 0 or -1 (direct mode only) */
int OpenIMP_T32_IvdcRead(uint32_t off, uint32_t *v);
int OpenIMP_T32_IvdcWrite(uint32_t off, uint32_t v);
/* encoder channel start / stop (codec OI_T32_IVDC_*; direct: fed by the
 * direct FrameSource channel) */
void OpenIMP_T32_IvdcEncStart(int codec, uint32_t w, uint32_t h, int direct);
void OpenIMP_T32_IvdcEncStop(int codec, int direct);

/* FrameSource: 1 when chn's frames come over IVDC (openimp_t32_fs.c) */
int OpenIMP_T32_FsChnDirect(int chn);
/* the FrameSource channel feeding (devID, grpID), through OSD groups; -1 */
int OpenIMP_T32_FsSourceOf(int devID, int grpID);

#endif
