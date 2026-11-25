/* #899 host test: vendor 2.0.2.1 YuvInit takes a QP byte of the rate-control head only below 52 and
 * the bit rate only when non-zero (else the i264e_param_default value stays); OpenIMP's stand-alone
 * encoder maps the attribute the same way before the shared channel set-up (t32_rc_yuv_h264_defaults). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"

int main(void)
{
    IMPEncoderAttrRcMode m;
    IMPEncoderAttrH264CBR *h = &m.attrH264Cbr;

    /* SetDefaultParam with initial QP -1: 0xff must become the i264e default 35, the rest stays */
    memset(&m, 0, sizeof(m));
    m.rcMode = IMP_ENC_RC_MODE_CBR;
    t32_rc_default_attr(&m, PT_H264, -1, 800);
    assert(h->initialQp == 0xff);
    t32_rc_yuv_h264_defaults(&m);
    assert(h->initialQp == 35 && h->maxQp == 45 && h->minQp == 15 && h->maxIQp == 45 && h->minIQp == 15);
    assert(h->outBitRate == 800);
    /* every byte >= 52 and a zero rate fall back */
    memset(h, 0, sizeof(*h));
    h->maxQp = 60; h->minQp = 52; h->initialQp = 51; h->maxIQp = 200; h->minIQp = 0;
    t32_rc_yuv_h264_defaults(&m);
    assert(h->maxQp == 45 && h->minQp == 15 && h->initialQp == 51 && h->maxIQp == 45 && h->minIQp == 0);
    assert(h->outBitRate == 2000);
    /* FIXQP has no head: untouched */
    memset(&m, 0, sizeof(m));
    m.rcMode = IMP_ENC_RC_MODE_FIXQP;
    m.attrH264FixQp.IQp = 99;
    t32_rc_yuv_h264_defaults(&m);
    assert(m.attrH264FixQp.IQp == 99);
    printf("yuvrc_test: OK\n");
    return 0;
}
