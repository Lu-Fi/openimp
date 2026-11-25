/* #889 IMP_OSD_SetRgnAttrWithTimestamp argument rule (src/t23/openimp_t23_osd.c, shared by
 * T23/T32/T33/T40/T41): like the vendor (T23 1.3.0 0xae81c, T32 2.2.0 0x17cd88, T41 1.2.5,
 * T40 1.3.1) only RECT (3) and COVER (6) regions pass, the type is checked before the
 * handle; other types: 0x80020040, on T40 -1.  Host test, no region created: a RECT / COVER
 * with a bad handle must reach IMP_OSD_SetRgnAttr (its handle error), not the type error. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <imp/imp_system.h>
#include "t23/openimp_t23_osd_abi.h"
#include "t23/openimp_t23_osd.h"

int IMP_OSD_SetRgnAttrWithTimestamp(IMPRgnHandle handle, IMPOSDRgnAttr *attr,
                                    IMPOSDRgnTimestamp *timestamp);

#if defined(PLATFORM_T40)
#define BADTYPE (-1)
#else
#define BADTYPE ((int)0x80020040)
#endif

int main(void)
{
    IMPOSDRgnAttr a;
    IMPOSDRgnTimestamp ts;
    int t, r;

    memset(&ts, 0, sizeof(ts));
    for (t = 0; t <= 13; t++) {
        memset(&a, 0, sizeof(a));
        a.type = (IMPOsdRgnType)t;
        r = IMP_OSD_SetRgnAttrWithTimestamp(-1, &a, &ts);
        if (t == OSD_REG_RECT || t == OSD_REG_COVER)
            assert(r != BADTYPE && r != 0);   /* handle error of IMP_OSD_SetRgnAttr */
        else
            assert(r == BADTYPE);
    }
    a.type = OSD_REG_RECT;
    assert(IMP_OSD_SetRgnAttrWithTimestamp(0, &a, NULL) == (int)0x80020040);
    assert(IMP_OSD_SetRgnAttrWithTimestamp(0, NULL, &ts) == (int)0x80020040);
    printf("osd_ts_test: OK (bad type -> 0x%x)\n", BADTYPE);
    return 0;
}
