/*
 * OpenIMP T32 ISP state shared with the later T32 modules (#288, planned).
 */
#ifndef OPENIMP_T32_ISP_H
#define OPENIMP_T32_ISP_H

#include <stdint.h>

/* /dev/tx-isp descriptor while the ISP is open, else -1 */
int OpenIMP_T32_IspFd(void);
/* sensor input size from S_INPUT (AddSensor); -1 without a sensor */
int OpenIMP_T32_SensorSize(uint32_t *width, uint32_t *height);
/* sensor of VI num: 0 none, 1 added, 2 enabled (streaming) */
int OpenIMP_T32_SensorState(int num);
/* /dev/misc-ivdc GET_DIRECT: the direct mode (0 = frames via memory, 1 = the
 * ISP feeds the encoder directly, 2..4 several sensors) */
int OpenIMP_T32_IvdcDirect(void);
/* the answer IMP_System_Init got (0 before System_Init) */
int OpenIMP_T32_IvdcDirectMode(void);
/* 1 between IMP_System_Init and IMP_System_Exit */
int OpenIMP_T32_SystemInitDone(void);

/* IQ-bin loader (openimp_t32_bin.c, #289) */
#include <imp/imp_common.h>
int OpenIMP_T32_BinOpen(void);
void OpenIMP_T32_BinClose(void);
int OpenIMP_T32_BinAddSensor(IMPVI_NUM num, const char *sensor);
void OpenIMP_T32_BinDelSensor(IMPVI_NUM num);

#endif
