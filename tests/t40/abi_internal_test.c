/* OpenIMP's own T40 ABI headers: their static asserts (OSD attribute 1948
 * bytes with fontData at 1640 and mosaicAttr at 1920, 1772 readable prefix,
 * IVS frame record 0x30 with the pool at 0x20) are the test.  Compile-only
 * with a 32-bit MIPS compiler, -DPLATFORM_T40. */
#ifndef PLATFORM_T40
#error "build with -DPLATFORM_T40"
#endif
#include "t23/openimp_t23_osd_abi.h"
#include "t31/openimp_t31_ivs_abi.h"

_Static_assert(sizeof(void *) == 4, "needs a 32-bit target compiler");
_Static_assert(T23_OSD_RGN_ATTR_T40_MIN == 1772u, "T40 shared prefix");
_Static_assert(offsetof(IMPOSDRgnAttr, data) == 32, "T40 OSD data");
int t40_abi_internal_checked;
