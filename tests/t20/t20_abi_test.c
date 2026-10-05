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

int t20_abi_test_anchor(void)
{
    puts("t20_abi_test: ok (compile-time layout checks passed)");
    return 0;
}
