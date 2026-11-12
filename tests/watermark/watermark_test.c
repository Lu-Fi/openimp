/* tests/watermark: src/watermark against the vendor embed_watermark
 * (tools/watermark_oracle.py, the vendor routine run under unicorn: the
 * builds with separate multiply and add) and against the fused-madd builds as
 * measured on a camera (watermark_vectors_fused.txt, tools/watermark_devcheck.c).
 * Usage: watermark-test VECTORS [fused]
 * Vector line: 'W stride height ret : <64x16 input hex> : <64x16 output hex>'
 * ('-' for both blocks when the vendor refused the plane). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "watermark/watermark.h"

#define BLK (WATERMARK_WIDTH * WATERMARK_HEIGHT)

static int unhex(const char *s, unsigned char *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned int v;

        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static char line[8192];
    unsigned char in[BLK], want[BLK];
    unsigned long n = 0, bad = 0;
    FILE *f;
    int fused = argc > 2 && atoi(argv[2]) != 0;

    if (argc < 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s watermark_vectors.txt [fused]\n", argv[0]);
        return 2;
    }
    while (fgets(line, sizeof(line), f)) {
        int stride, height, ret, got, y;
        char a[2 * BLK + 2], b[2 * BLK + 2];
        unsigned char *plane;

        if (line[0] != 'W')
            continue;
        if (sscanf(line, "W %d %d %d : %2050s : %2050s", &stride, &height, &ret, a, b) != 5) {
            fprintf(stderr, "bad line: %.60s\n", line);
            return 2;
        }
        n++;
        /* the plane around the block: a fixed pattern that must survive */
        plane = malloc((size_t)(stride > 0 ? stride : 1) * (size_t)(height + 1));
        if (!plane)
            return 2;
        for (y = 0; y < stride * height; y++)
            plane[y] = (unsigned char)(y * 7 + 3);
        if (a[0] != '-') {
            if (unhex(a, in, BLK) || unhex(b, want, BLK))
                return 2;
            for (y = 0; y < WATERMARK_HEIGHT; y++)
                memcpy(plane + y * stride, in + y * WATERMARK_WIDTH, WATERMARK_WIDTH);
        }
        got = watermark_embed_mode(plane, stride, height, fused);
        if (got != ret) {
            fprintf(stderr, "line %lu: ret %d, vendor %d\n", n, got, ret);
            bad++;
        } else if (a[0] != '-') {
            int diff = 0, i;

            for (y = 0; y < WATERMARK_HEIGHT; y++)
                diff |= memcmp(plane + y * stride, want + y * WATERMARK_WIDTH, WATERMARK_WIDTH);
            for (i = 0; i < stride * height; i++)
                if ((i % stride >= WATERMARK_WIDTH || i / stride >= WATERMARK_HEIGHT) &&
                    plane[i] != (unsigned char)(i * 7 + 3))
                    diff = 1;
            if (diff) {
                if (bad < 5)
                    for (i = 0; i < BLK; i++)
                        if (plane[(i / 64) * stride + i % 64] != want[i]) {
                            fprintf(stderr, "line %lu: pixel %d,%d %u, vendor %u\n", n, i % 64, i / 64,
                                    plane[(i / 64) * stride + i % 64], want[i]);
                            break;
                        }
                bad++;
            }
        }
        free(plane);
    }
    fclose(f);
    printf("watermark (%s): %lu vectors, %lu mismatches\n", fused ? "fused" : "unfused", n, bad);
    return bad || !n ? 1 : 0;
}
