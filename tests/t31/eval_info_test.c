/* eval_info_test - layout of the T31 IMP_Encoder_GetChnEvalInfo record (vendor 36 bytes). */
#include <stdio.h>
#include <string.h>
#include "t31_eval_info.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void)
{
    uint8_t m[T31_SLICE_STATUS_SIZE], out[T31_EVAL_SIZE];
    uint32_t w[6] = { 11, 22, 33, 44, 55, 66 }, g;
    uint16_t mn = 17, mx = 43, q;
    int i;

    t31_eval_status_init(m);
    memcpy(&q, m + 0x3e, 2);
    CHECK(q == 32767, "min QP starts at 32767, not %u", q);
    for (i = 0; i < 0x70; i++)
        CHECK(i == 0x3e || i == 0x3f || m[i] == 0, "byte %#x not zero", i);

    memcpy(m + 0x1c, w, sizeof(w));
    memcpy(m + 0x3e, &mn, 2);
    memcpy(m + 0x40, &mx, 2);
    memset(out, 0xee, sizeof(out));
    t31_eval_pack(m, 28, out);
    for (i = 0; i < 6; i++) {
        memcpy(&g, out + 4 * i, 4);
        CHECK(g == w[i], "word %d = %u want %u", i, g, w[i]);
    }
    memcpy(&q, out + 24, 2); CHECK(q == 28, "slice qp %u", q);
    memcpy(&q, out + 26, 2); CHECK(q == 17, "min qp %u", q);
    memcpy(&q, out + 28, 2); CHECK(q == 43, "max qp %u", q);
    for (i = 30; i < 36; i++)
        CHECK(out[i] == 0, "pad byte %d = %02x", i, out[i]);
    if (failures) return 1;
    printf("t31 eval info record tests passed\n");
    return 0;
}
