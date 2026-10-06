/*
 * t20_abi_test - T20 public structs in the vendor 3.12.0 layout.
 *
 * T20 streamers (timps, prudynt, raptor) are built against the vendor T20
 * 3.12.0 headers, so the sizes and offsets below are those of that header
 * (measured with the mipsel toolchain).  Arena/guard check: a caller-sized
 * buffer filled through the vendor byte offsets must read back through the
 * OpenIMP struct, and a copy of the OpenIMP struct must stay inside the
 * vendor size (no write past the caller's object).
 * Built with -m32 -c (headers hold pointer structs, host is 64-bit and no
 * 32-bit libc to link): the checks are all _Static_assert.
 */
#include <stdint.h>
#include <stdio.h>

#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>

#define VENDOR_CHN_ATTR 172
#define VENDOR_RC_ATTR 124
#define VENDOR_TRANS 144
#define VENDOR_JPEGQL 257
#define VENDOR_DRC 16
/* Arena/guard as compile-time facts: an object the library fills must fit
 * inside the vendor-sized caller object (guard = nothing written past it)
 * and the members must sit at the vendor byte offsets. */
#define FITS(T, V) _Static_assert(sizeof(T) <= (V), #T " overruns vendor object")
FITS(IMPEncoderCHNAttr, VENDOR_CHN_ATTR);
FITS(IMPEncoderRcAttr, VENDOR_RC_ATTR);
FITS(IMPEncoderH264TransCfg, VENDOR_TRANS);
FITS(IMPEncoderJpegeQl, VENDOR_JPEGQL);
FITS(IMPISPDrcAttr, VENDOR_DRC);

#define OFF(T, M, V) _Static_assert(offsetof(T, M) == (V), #T "." #M)
OFF(IMPEncoderCHNAttr, rcAttr.attrRcMode, 48 + 12);
OFF(IMPEncoderCHNAttr, rcAttr.attrFrmUsed, 48 + 56);
OFF(IMPEncoderCHNAttr, rcAttr.attrDemask, 48 + 68);
OFF(IMPEncoderCHNAttr, rcAttr.attrDenoise, 48 + 80);
OFF(IMPEncoderCHNAttr, rcAttr.attrHSkip, 48 + 96);
OFF(IMPEncoderH264TransCfg, chroma_qp_index_offset, 140);
OFF(IMPEncoderJpegeQl, qmem_table, 1);
OFF(IMPISPDrcAttr, dval_max, 5);
OFF(IMPISPDrcAttr, dval_min, 6);
OFF(IMPISPDrcAttr, slop_max, 7);
OFF(IMPISPDrcAttr, slop_min, 8);
OFF(IMPISPDrcAttr, black_level, 10);
OFF(IMPISPDrcAttr, white_level, 12);
OFF(IMPISPSinterDenoiseAttr, sval_max, 9);
OFF(IMPISPTemperDenoiseAttr, tval_max, 5);

/* FrameSource: vendor T20 3.12.0 imp_framesource.h / imp_common.h */
_Static_assert(sizeof(IMPFSChnCrop) == 20, "IMPFSChnCrop");
_Static_assert(sizeof(IMPFSChnScaler) == 12, "IMPFSChnScaler");
_Static_assert(sizeof(IMPFSChnAttr) == 0x3c, "IMPFSChnAttr size");
OFF(IMPFSChnAttr, picWidth, 0x00);
OFF(IMPFSChnAttr, picHeight, 0x04);
OFF(IMPFSChnAttr, pixFmt, 0x08);
OFF(IMPFSChnAttr, crop, 0x0c);
OFF(IMPFSChnAttr, scaler, 0x20);
OFF(IMPFSChnAttr, outFrmRateNum, 0x2c);
OFF(IMPFSChnAttr, outFrmRateDen, 0x30);
OFF(IMPFSChnAttr, nrVBs, 0x34);
OFF(IMPFSChnAttr, type, 0x38);
_Static_assert(sizeof(IMPFrameInfo) == 0x28, "IMPFrameInfo size");
OFF(IMPFrameInfo, index, 0x00);
OFF(IMPFrameInfo, pool_idx, 0x04);
OFF(IMPFrameInfo, width, 0x08);
OFF(IMPFrameInfo, height, 0x0c);
OFF(IMPFrameInfo, pixfmt, 0x10);
OFF(IMPFrameInfo, size, 0x14);
OFF(IMPFrameInfo, phyAddr, 0x18);
OFF(IMPFrameInfo, virAddr, 0x1c);
OFF(IMPFrameInfo, timeStamp, 0x20);
_Static_assert(sizeof(IMPFSChnFifoAttr) == 8, "IMPFSChnFifoAttr");
/* pixel formats the T20 frame channels use (vendor enum values) */
_Static_assert(PIX_FMT_YUV420P == 0 && PIX_FMT_YUYV422 == 1 &&
               PIX_FMT_UYVY422 == 2 && PIX_FMT_NV12 == 10 &&
               PIX_FMT_NV21 == 11, "IMPPixelFormat NV12/NV21/YUYV/UYVY");

int t20_abi_test_anchor(void)
{
    puts("t20_abi_test: ok (compile-time layout checks passed)");
    return 0;
}
