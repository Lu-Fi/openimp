/*
 * T32 frame rate / GOP glue (#295): src/t32/t32_gop33.h against the vendor
 * 2.2.0 encoder_get_fps_mask under tools/eprc_t33_gop_oracle.py: per picture
 * the rate and GOP sent (i264e_set_param 5 / 7), the IDR flag and the
 * channel's rate and GOP afterwards.
 *
 *   gop33_test VECTORS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/t32_gop33.h"

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    char line[256];
    T32Gop33 s;
    uint32_t src_n = 0, src_d = 0;
    unsigned scen = 0, pics = 0, bad = 0, sets = 0, zero = 0;
    int seed = -1, live = 0;

    if (!f) {
        fprintf(stderr, "usage: %s VECTORS\n", argv[0]);
        return 2;
    }
    memset(&s, 0, sizeof(s));
    while (fgets(line, sizeof(line), f)) {
        unsigned a, b, c, d, e, pic, idr, gop, num, den;
        char call[32], s5[32], s7[32];

        if (sscanf(line, "S %d %u %u %u %u %u", &seed, &a, &b, &c, &d, &e) == 6) {
            src_n = a;
            src_d = b;
            scen++;
            live = 0;
        } else if (sscanf(line, "C %u %u %u", &gop, &num, &den) == 3) {
            t32_gop33_create(&s, num, den, gop);
            live = 1;
        } else if (live && sscanf(line, "P %u %31s %31s %31s %u %u %u %u", &pic, call, s5, s7, &idr, &gop,
                                  &num, &den) == 8) {
            unsigned r, n = 0, dd = 0, g = 0, ok;
            char w5[32] = "-", w7[32] = "-";

            if (sscanf(call, "F%u/%u", &n, &dd) == 2)
                t32_gop33_set_rate(&s, n, dd);
            else if (sscanf(call, "G%u", &g) == 1)
                t32_gop33_set_gop(&s, g);
            r = t32_gop33_step(&s, src_n, src_d);
            if (!strcmp(s7, "0")) {     /* vendor GOP 0: it traps at this IDR, OpenIMP keeps the GOP */
                if (!(r & T32_GOP33_ZERO) && bad++ < 10)
                    fprintf(stderr, "seed %d pic %u: vendor GOP 0, ours %u\n", seed, pic, s.gop);
                zero++;
                live = 0;
                continue;
            }
            if (r & T32_GOP33_RATE)
                snprintf(w5, sizeof(w5), "%u/%u", s.num, s.den);
            if (r & T32_GOP33_GOP)
                snprintf(w7, sizeof(w7), "%u", s.gop);
            ok = !strcmp(w5, s5) && !strcmp(w7, s7) && !!(r & T32_GOP33_IDR) == !!idr && s.gop == gop &&
                 s.num == num && s.den == den;
            pics++;
            sets += strcmp(s5, "-") || strcmp(s7, "-");
            if (!ok) {
                if (bad++ < 10)
                    fprintf(stderr, "seed %d pic %u %s: vendor %s %s idr %u gop %u %u/%u, ours %s %s idr %u gop %u %u/%u\n",
                            seed, pic, call, s5, s7, idr, gop, num, den, w5, w7, !!(r & T32_GOP33_IDR), s.gop,
                            s.num, s.den);
                live = 0;           /* the rest of the scenario would only repeat it */
            }
        }
    }
    fclose(f);
    printf("frame rate / GOP glue (vendor 2.2.0 encoder_get_fps_mask): %u scenarios, %u pictures, %u sends, "
           "%u vendor GOP 0 (traps; GOP kept): %s\n", scen, pics, sets, zero, bad ? "FAIL" : "equal");
    return bad ? 1 : 0;
}
