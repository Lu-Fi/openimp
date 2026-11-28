/* T33/T32 stand-alone YUV / JPEG encoder (#752): return codes and parameter maps of the vendor 2.0.2.1 argument
 * checks (IMP_Encoder_YuvInit 0x1040c4, YuvEncode 0x104b4c, YuvExit 0x105248, InputJpege 0x105430,
 * i264e_reconfig_qpgai_set 0x843f4). */
#include <stdio.h>
#include "t32/openimp_t32_yuvrc.h"

static int bad;
#define CHK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void)
{
    CHK(yuvrc_init(1, 1, 320, 192, 0, 15) == 0);                     /* valid H.264 reaches the init */
    CHK(yuvrc_init(1, 2, 1920, 1080, 1, 15) == 0);                   /* valid H.265 */
    CHK(yuvrc_init(1, 1, 320, 192, 5, 15) == 0);
    CHK(yuvrc_init(1, 1, 320, 192, 6, 15) == (int)0x80040002);       /* rcMode >= 6 */
    CHK(yuvrc_init(1, 0, 320, 192, 0, 15) == (int)0x80040002);       /* JPEG / unknown codec */
    CHK(yuvrc_init(1, 3, 320, 192, 0, 15) == (int)0x80040002);
    CHK(yuvrc_init(1, 1, 322, 192, 0, 15) == (int)0x80040002);       /* width % 16 */
    CHK(yuvrc_init(1, 2, 320, 191, 0, 15) == (int)0x80040002);       /* odd height */
    CHK(yuvrc_init(0, 1, 320, 192, 0, 15) == (int)0x80040002);
    CHK(yuvrc_init(1, 1, 320, 192, 0, 0) == (int)0x80040002);    /* #998: maxGop 0 (zeroed attribute), H.264 */
    CHK(yuvrc_init(1, 2, 320, 192, 1, 0) == (int)0x80040002);    /* H.265 */
    CHK(yuvrc_handle(0, 0, 0) == (int)0x80040010);
    CHK(yuvrc_handle(1, 0, 7) == (int)0x80040002);
    CHK(yuvrc_handle(1, 1, 0) == (int)0x80040002);
    CHK(yuvrc_handle(1, 0, 1) == (int)0x80040002);               /* foreign handle */
    CHK(yuvrc_handle(1, 1, 1) == 0 && yuvrc_handle(1, 1, 2) == 0);
    CHK(yuvrc_frame(320, 192, 320, 192) == 0);
    CHK(yuvrc_frame(320, 180, 320, 192) == (int)0x80040002);
    CHK(yuvrc_jpege(320, 192) == 0);
    CHK(yuvrc_jpege(100, 192) == (int)0x80040002);
    CHK(yuvrc_jpege(320, 5) == (int)0x80040002);
    CHK(yuvrc_jpege_q(-1) == 10 && yuvrc_jpege_q(0) == 0 && yuvrc_jpege_q(40) == 40);
    CHK(yuvrc_jpege_q(100) == 100 && yuvrc_jpege_q(101) == 98);
    CHK(yuvrc_clip(5, 0, 2) == 2 && yuvrc_clip(-1, 0, 3) == 0 && yuvrc_clip(3, 0, 3) == 3);
    if (bad) return 1;
    puts("yuv_rc_test: all passed");
    return 0;
}
