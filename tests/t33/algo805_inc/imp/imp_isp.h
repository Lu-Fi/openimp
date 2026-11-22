/* Minimal T33 2.0.2.1 algorithm-callback types for algo805_test.c (sizes as the vendor header: info 1996 / 12316,
 * attr 56 / 16, init 128 / 20; only the fields openimp_p1_algo.c touches are named). */
#ifndef ALGO805_IMP_ISP_H
#define ALGO805_IMP_ISP_H
#include <stdint.h>
typedef enum { IMPVI_MAIN = 0 } IMPVI_NUM;
typedef enum { ISP_CORE_EXPR_UNIT_LINE, ISP_CORE_EXPR_UNIT_US } IMPISPAEIntegrationTimeUnit;
typedef enum { IMPISP_AE_NOTIFY_FPS_CHANGE } IMPISPAeNotify;
typedef enum { IMPISP_AWB_NOTIFY_MODE_CHANGE } IMPISPAwbNotify;
typedef struct { uint32_t hts, vts, fps, width, height; } IMPISPSENSORAttr;
typedef struct { uint8_t ae_info[1934]; IMPISPAEIntegrationTimeUnit AeIntegrationTimeUnit; uint32_t AeIntegrationTime;
	uint32_t w[8]; IMPISPSENSORAttr sensor_attr; } IMPISPAeInfo;
typedef struct { uint32_t change; IMPISPAEIntegrationTimeUnit AeIntegrationTimeUnit; uint32_t AeIntegrationTime, AeAGain, AeDGain,
	AeIspDGain, AeShortIntegrationTime, w[7]; } IMPISPAeAttr;
typedef struct { uint32_t w[32]; } IMPISPAeInitAttr;
typedef struct { uint32_t cur_r_gain, cur_b_gain, w[5]; uint32_t zone[3][1024]; } IMPISPAwbInfo;
typedef struct { uint32_t change, r_gain, b_gain, ct; } IMPISPAwbAttr;
typedef struct { uint32_t w[5]; } IMPISPAwbInitAttr;
typedef struct {
	void *priv_data;
	int (*open)(void *priv_data, IMPISPAeInitAttr *AeInitAttr);
	void (*close)(void *priv_data);
	void (*handle)(void *priv_data, IMPISPAeInfo *AeInfo, IMPISPAeAttr *AeAttr);
	int (*notify)(void *priv_data, IMPISPAeNotify notify, void *data);
} IMPISPAeAlgoFunc;
typedef struct {
	void *priv_data;
	int (*open)(void *priv_data, IMPISPAwbInitAttr *AwbInitAttr);
	void (*close)(void *priv_data);
	void (*handle)(void *priv_data, IMPISPAwbInfo *AwbInfo, IMPISPAwbAttr *AwbAttr);
	int (*notify)(void *priv_data, IMPISPAwbNotify notify, void *data);
} IMPISPAwbAlgoFunc;
#endif
