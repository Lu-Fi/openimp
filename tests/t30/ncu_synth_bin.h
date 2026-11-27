/* Synthetic /etc/sensor/ncu_<sensor>.bin in the vendor 1.0.4 layout (0x1548 bytes: size word,
 * day set at +4, night set at +4+0xaa0) for the T30 NCU tests (#701).  Deterministic, no vendor
 * data: every tuning word is 1..255, the 9 gain points of each set are increasing.  Shared by
 * tests/t30/ncu_test.c and tools/t30_ncu/ncu_oracle.c (which records the vendor register trace). */
#ifndef NCU_SYNTH_BIN_H
#define NCU_SYNTH_BIN_H
#include <stdint.h>
#include <string.h>

static void ncu_synth_bin(uint32_t *w, uint32_t seed, uint32_t mode)
{
    uint32_t x = seed * 2654435761u + 1u;
    unsigned i, s, k;

    memset(w, 0, 0x1548);
    w[0] = 0x1548;
    for (i = 1; i < 0x1548 / 4; i++) {
        x = x * 1103515245u + 12345u;
        w[i] = 1u + ((x >> 16) % 255u);
    }
    for (s = 0; s < 2; s++) {
        uint32_t *set = &w[1 + s * (0xaa0 / 4)];
        uint32_t g = 256;

        set[0] = (mode & 3u) << 16;   /* flags: 3D mode, no debug bits */
        for (k = 0; k < 9; k++) {      /* gain points at +0x08..+0x28 */
            x = x * 1103515245u + 12345u;
            g += 200u + ((x >> 16) % (2000u + 4000u * k));
            set[2 + k] = g;
        }
    }
}

/* Same for the vendor 1.0.5 layout (0xec8 bytes: size word, day set at +4, night set at +4+0x760,
 * flags word of each set = version 1 << 24 | 3D mode << 16 | debug bits).  The night set carries the
 * flag bits 3 and 4 (0x18: extra 0x24 / 0x4 writes of ncu_func_par_download), the day set none. */
static void ncu_synth_bin105(uint32_t *w, uint32_t seed, uint32_t mode)
{
    uint32_t x = seed * 2654435761u + 1u;
    unsigned i, s, k;

    memset(w, 0, 0xec8);
    w[0] = 0xec8;
    for (i = 1; i < 0xec8 / 4; i++) {
        x = x * 1103515245u + 12345u;
        w[i] = 1u + ((x >> 16) % 255u);
    }
    for (s = 0; s < 2; s++) {
        uint32_t *set = &w[1 + s * (0x760 / 4)];
        uint32_t g = 256;

        set[0] = (1u << 24) | ((mode & 3u) << 16) | (s ? 0x18u : 0u);
        for (k = 0; k < 9; k++) {      /* gain points at +0x08..+0x28 */
            x = x * 1103515245u + 12345u;
            g += 200u + ((x >> 16) % (2000u + 4000u * k));
            set[2 + k] = g;
        }
    }
}
#endif
