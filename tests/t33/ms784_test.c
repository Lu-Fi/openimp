/*
 * #784 group 3: T33 multi-section / bitstream pool set-up vs vendor libimp
 * 2.0.2.1 (src/t32/openimp_t33_sections.h; QEMU vms1 / vms0 / v5ms0 /
 * vjm1 / vjm0): pool sizes, the JPEG job words and the HERA list without
 * the BSFULL registers when the mode is off.
 */
#include <stdio.h>
#include <string.h>
#include "t32/openimp_t32_hera.h"
#include "t32/openimp_t33_hera.h"
#include "t32/openimp_t33_hera_h265.h"
#include "t32/openimp_t33_sections.h"

static int fails;

static void eq(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: %u (0x%x), vendor %u (0x%x)\n", what, got, got, want, want);
        fails++;
    } else
        printf("ok   %s: %u\n", what, got);
}

static int has_reg(const uint32_t *l, size_t n, uint32_t reg)
{
    size_t i;

    for (i = 0; i < n; i++)
        if ((l[2 * i + 1] & 0xffffcu) == reg)
            return 1;
    return 0;
}

int main(void)
{
    static uint32_t list[2 * T32_HERA5_LIST_MAX_PAIRS];
    static const uint32_t emc[3] = { 0x9001cu, 0x9005cu, 0x90064u };
    T33HeraH264Pic t;
    T33HeraH265Pic t5;
    uint32_t kb, bytes, i;
    size_t n;

    /* defaults: both modes on, 250 KiB x 2 (EMC 0x90064 = 0xfa, JPEG 0x4c = 0xfa) */
    t33_sections(1, T33_MS_KB_DEFAULT, T33_BS_IVPU_DEFAULT, &kb, &bytes);
    eq("default HERA section KiB", kb, 250u);
    eq("default ivpuBs bytes", bytes, 512000u);
    /* vms1 / vjm1: SetMultiSectionMode(1, 100, 2): 0x90064 = 0x64, second half +0x19000 */
    t33_sections(1, 100, T33_BS_IVPU_DEFAULT, &kb, &bytes);
    eq("mode 1 size 100: section KiB", kb, 100u);
    eq("mode 1 size 100: pool bytes", bytes, 204800u);
    /* vms0: SetMultiSectionMode(0) + SetIvpuBsSize(2 MiB): one buffer */
    t33_sections(0, 100, 2097152u, &kb, &bytes);
    eq("mode 0: section KiB", kb, 0u);
    eq("mode 0: pool = IvpuBsSize", bytes, 2097152u);
    /* vjb / vjm0 control words: 0x22e80770 (sections) / 0x06e90770 (limit) at quality index 0x3a */
    eq("JPEG ctrl sections", t33_jpeg_ctrl(0x06e90770u, 250u), 0x22e80770u);
    eq("JPEG ctrl mode 0", t33_jpeg_ctrl(0x06e90770u, 0u), 0x06e90770u);
    eq("JPEG limit mode 0 (0x2c = JpegBsSize)", t33_jpeg_limit(0u, 300000u), 300000u);
    eq("JPEG limit sections", t33_jpeg_limit(250u, 512000u), 0u);

    /* HERA lists: EMC 0x9001c / 0x9005c / 0x90064 only with a section size */
    memset(&t, 0, sizeof(t));
    t.pic.width = 640;
    t.pic.height = 368;
    t.pic.qp = 30;
    t.pic.src_stride = 640;
    t.pic.section_kb = 250;
    n = t33_hera_h264_list(&t, list);
    for (i = 0; i < 3; i++)
        eq("H.264 list, sections: EMC register present", (uint32_t)has_reg(list, n, emc[i]), 1u);
    t.pic.section_kb = 0;
    n = t33_hera_h264_list(&t, list);
    eq("H.264 list, mode 0: built", n != 0, 1u);
    for (i = 0; i < 3; i++)
        eq("H.264 list, mode 0: EMC register absent", (uint32_t)has_reg(list, n, emc[i]), 0u);

    memset(&t5, 0, sizeof(t5));
    t5.pic.width = 640;
    t5.pic.height = 368;
    t5.pic.qp = 30;
    t5.pic.src_stride = 640;
    t5.pic.section_kb = 250;
    n = t33_hera_h265_list(&t5, list);
    for (i = 0; i < 3; i++)
        eq("H.265 list, sections: EMC register present", (uint32_t)has_reg(list, n, emc[i]), 1u);
    t5.pic.section_kb = 0;
    n = t33_hera_h265_list(&t5, list);
    eq("H.265 list, mode 0: built", n != 0, 1u);
    for (i = 0; i < 3; i++)
        eq("H.265 list, mode 0: EMC register absent", (uint32_t)has_reg(list, n, emc[i]), 0u);

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
