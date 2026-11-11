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
/* /dev/misc-ivdc GET_DIRECT: 1 when the ISP feeds the encoder directly */
int OpenIMP_T32_IvdcDirect(void);

#endif
