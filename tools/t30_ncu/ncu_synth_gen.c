/* Writes the synthetic T30 NCU test bin (tests/t30/ncu_synth_bin.h) for the vendor oracle.
 * cc -I../../tests/t30 -o ncu_synth_gen ncu_synth_gen.c; ./ncu_synth_gen out.bin [seed] [mode] [layout 104|105] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ncu_synth_bin.h"

int main(int argc, char **argv)
{
    static uint32_t bin[0x1548 / 4];
    int l105 = argc > 4 && strtoul(argv[4], NULL, 0) == 105;
    FILE *f;

    if (argc < 2)
        return 2;
    (l105 ? ncu_synth_bin105 : ncu_synth_bin)(bin, argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u,
                                              argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1u);
    f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(bin, l105 ? 0xec8 : sizeof(bin), 1, f) != 1)
        return 1;
    return fclose(f) != 0;
}
