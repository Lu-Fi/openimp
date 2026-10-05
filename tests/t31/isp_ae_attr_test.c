/*
 * isp_ae_attr_test - T31 IMPISPAEAttr <-> 152-byte kernel block.
 *
 * The expected positions are the stack offsets of the OEM T31 1.1.6 libimp
 * (IMP_ISP_Tuning_SetAeAttr 0x97558: user offset -> sp offset of its local
 * block, which starts at sp+48 and is 0x98 bytes long).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isp/isp_ae_attr.h"

static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

/* {offset in IMPISPAEAttr, sp offset in the OEM SetAeAttr} */
static const int oem[18][2] = {
    { 0, 48 }, { 4, 100 }, { 8, 60 }, { 12, 104 }, { 16, 52 }, { 20, 192 },
    { 24, 56 }, { 28, 108 }, { 32, 64 }, { 36, 144 }, { 40, 112 },
    { 44, 124 }, { 48, 116 }, { 52, 120 }, { 56, 196 }, { 60, 148 },
    { 64, 152 }, { 68, 156 }
};

int main(void)
{
    uint32_t user[ISP_AE_ATTR_WORDS + 4];
    uint32_t kernel[ISP_AE_KERNEL_WORDS + 4];
    uint32_t back[ISP_AE_ATTR_WORDS + 4];
    int i, w, mapped;

    CHECK(sizeof(uint32_t) * ISP_AE_ATTR_WORDS == 72, "IMPISPAEAttr is 72 bytes");
    CHECK(sizeof(uint32_t) * ISP_AE_KERNEL_WORDS == 0x98, "kernel block is 0x98");
    for (i = 0; i < ISP_AE_ATTR_WORDS + 4; i++)
        user[i] = 0x1000u + (uint32_t)i;
    memset(kernel, 0xcc, sizeof(kernel));
    isp_ae_attr_to_kernel(kernel, user);
    for (i = 0; i < 18; i++) {
        int kw = (oem[i][1] - 48) / 4;

        CHECK(kernel[kw] == user[oem[i][0] / 4],
              "user word %d should be kernel word %d (is %08x)", oem[i][0] / 4,
              kw, kernel[kw]);
    }
    for (w = 0; w < ISP_AE_KERNEL_WORDS; w++) {
        mapped = 0;
        for (i = 0; i < 18; i++)
            mapped |= (oem[i][1] - 48) / 4 == w;
        CHECK(mapped || kernel[w] == 0, "unmapped kernel word %d = %08x", w,
              kernel[w]);
    }
    for (w = ISP_AE_KERNEL_WORDS; w < ISP_AE_KERNEL_WORDS + 4; w++)
        CHECK(kernel[w] == 0xcccccccc, "past the 0x98 block");

    /* Get: exactly 18 words reach the caller, nothing behind them */
    for (w = 0; w < ISP_AE_KERNEL_WORDS; w++)
        kernel[w] = 0x2000u + (uint32_t)w;
    memset(back, 0xa5, sizeof(back));
    isp_ae_attr_from_kernel(back, kernel);
    for (i = 0; i < 18; i++)
        CHECK(back[i] == kernel[(oem[i][1] - 48) / 4], "get word %d", i);
    for (i = 18; i < 22; i++)
        CHECK(back[i] == 0xa5a5a5a5, "GetAeAttr wrote past 72 bytes");

    if (failures) {
        fprintf(stderr, "isp ae attr: %d check(s) failed\n", failures);
        return 1;
    }
    puts("isp ae attr tests passed");
    return 0;
}
