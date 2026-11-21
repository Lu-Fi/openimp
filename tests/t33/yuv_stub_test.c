/* T33/T32 unbound YUV / JPEG encoder stage 1 (#752): return codes of the vendor 2.0.2.1 argument checks
 * (IMP_Encoder_YuvInit 0x1040c4, YuvEncode 0x104b4c, YuvExit 0x105248, InputJpege 0x105430). */
#include <stdio.h>
#include "t32/openimp_t32_yuvstub.h"

static int bad;
#define CHK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void)
{
    CHK(yuvstub_init_rc(1, 1, 320, 192) == (int)0x80048000);      /* valid H.264 reaches init */
    CHK(yuvstub_init_rc(1, 2, 1920, 1080) == (int)0x80048000);    /* valid H.265 */
    CHK(yuvstub_init_rc(1, 0, 320, 192) == (int)0x80040002);      /* JPEG / unknown codec */
    CHK(yuvstub_init_rc(1, 3, 320, 192) == (int)0x80040002);
    CHK(yuvstub_init_rc(1, 1, 322, 192) == (int)0x80040002);      /* width % 16 */
    CHK(yuvstub_init_rc(1, 2, 320, 191) == (int)0x80040002);      /* odd height */
    CHK(yuvstub_init_rc(0, 1, 320, 192) == (int)0x80040002);
    CHK(yuvstub_handle_rc(0) == (int)0x80040010);
    CHK(yuvstub_handle_rc(1) == (int)0x80040002);
    CHK(yuvstub_jpege_rc(320, 192) == (int)0x80048000);
    CHK(yuvstub_jpege_rc(100, 192) == (int)0x80040002);
    CHK(yuvstub_jpege_rc(320, 5) == (int)0x80040002);
    if (bad) return 1;
    puts("yuv_stub_test: all passed");
    return 0;
}
