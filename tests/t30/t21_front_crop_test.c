/* isp_front_crop.h: IMPISPFrontCrop <-> FRONT_CROP words (T10/T20/T21). */
#include <stdio.h>
#include <string.h>
#include "isp/isp_front_crop.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    IMPISPFrontCrop fc, back;
    uint32_t w[ISP_FRONT_CROP_WORDS];

    /* 20 bytes, the size the kernels copy */
    CHECK(sizeof(IMPISPFrontCrop) == 20);
    CHECK(sizeof(w) == 20);

    memset(&fc, 0xa5, sizeof(fc));	/* garbage in the bool's padding */
    fc.fcrop_enable = true;
    fc.fcrop_top = 0;
    fc.fcrop_left = 0;
    fc.fcrop_width = 1920;
    fc.fcrop_height = 1080;
    isp_front_crop_pack(&fc, w);
    CHECK(w[0] == 1);
    CHECK(w[1] == 0 && w[2] == 0 && w[3] == 1920 && w[4] == 1080);

    fc.fcrop_enable = false;
    fc.fcrop_top = 10;
    fc.fcrop_left = 20;
    isp_front_crop_pack(&fc, w);
    CHECK(w[0] == 0 && w[1] == 10 && w[2] == 20);

    /* kernel words with junk above the enable byte */
    w[0] = 0x100;
    isp_front_crop_unpack(w, &back);
    CHECK(!back.fcrop_enable);
    w[0] = 0x101;
    isp_front_crop_unpack(w, &back);
    CHECK(back.fcrop_enable && back.fcrop_top == 10 && back.fcrop_left == 20 &&
          back.fcrop_width == 1920 && back.fcrop_height == 1080);

    if (failures)
        return 1;
    printf("t21_front_crop_test: ok\n");
    return 0;
}
