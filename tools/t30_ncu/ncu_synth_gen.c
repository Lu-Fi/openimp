/* Writes the synthetic T30 NCU test bin (tests/t30/ncu_synth_bin.h) for the vendor oracle.
 * cc -I../../tests/t30 -o ncu_synth_gen ncu_synth_gen.c; ./ncu_synth_gen out.bin [seed] [mode] */
#include <stdio.h>
#include <stdlib.h>
#include "ncu_synth_bin.h"

int main(int argc, char **argv)
{
    static uint32_t bin[0x1548 / 4];
    FILE *f;

    if (argc < 2)
        return 2;
    ncu_synth_bin(bin, argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1u,
                  argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1u);
    f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(bin, sizeof(bin), 1, f) != 1)
        return 1;
    return fclose(f) != 0;
}
