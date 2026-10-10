#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "t40/codec_frame_ts.h"

int main(void)
{
    uint8_t f[0x30];
    uint64_t a = 0x1122334455667788ull, b = 0xdeadbeefcafef00dull;
    memset(f, 0, sizeof(f));
    memcpy(f + 0x20, &a, 8);
    memcpy(f + 0x28, &b, 8);
#if defined(PLATFORM_T40)
    if (codec_frame_capture_timestamp(f) != b) { puts("FAIL T40 ts@0x28"); return 1; }
#else
    if (codec_frame_capture_timestamp(f) != a) { puts("FAIL ts@0x20"); return 1; }
#endif
    puts("codec_frame_ts_test OK");
    return 0;
}
