/*
 * Issues #357/#397 (development aid, on the camera): src/watermark against the
 * vendor embed_watermark on the camera's own CPU and FPU.
 *
 * Loads a vendor libimp.so (path argument, never installed: /tmp only),
 * takes its exported watermark_init/embed_watermark and runs them and
 * watermark_embed() (linked in) on the same random luma planes, comparing the
 * whole plane byte for byte.  Nothing of the vendor library is copied; it is
 * read from the path given.
 *
 *   mipsel-linux-gcc -O2 -Isrc -ffp-contract=off -o wm-devcheck tools/watermark_devcheck.c \
 *       src/watermark/watermark.c -ldl
 *   /tmp/wm-devcheck /tmp/vendor/libimp.so [cases] [seed]
 *
 * Prints "devcheck: N planes, M mismatches" (exit 0 only when M is 0).
 * WM_VECTORS=1 also prints every vendor result as a tests/watermark vector
 * line (tests/watermark/watermark_vectors_fused.txt comes from the T20 camera).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "watermark/watermark.h"

static uint32_t rng_state;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* the block kinds of tools/watermark_oracle.py: noise, dark, flat, ramp,
 * edge, flat with noise, black/white, near white */
static uint8_t pixel(int kind, int base, int x, int y)
{
    int v;

    switch (kind) {
    case 0: return (uint8_t)rnd();
    case 1: return (uint8_t)(rnd() % 12u);
    case 2: return (uint8_t)base;
    case 3: return (uint8_t)((base + 9 * x + 5 * y) & 255);
    case 4: return x + y > 7 ? 235 : 16;
    case 5:
        v = base + (int)(rnd() % 13u) - 6;
        return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    case 6: return (rnd() & 1u) ? 255 : 0;
    default: return (uint8_t)(240u + rnd() % 16u);
    }
}

int main(int argc, char **argv)
{
    void *(*vinit)(void);
    int (*vembed)(void *, uint8_t *, int, int);
    static const int strides[] = { 64, 72, 96, 128, 200, 1920 };
    int cases = argc > 2 ? atoi(argv[2]) : 2000, c, bad = 0;
    void *lib, *ctx;

    rng_state = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 357u;
    if (argc < 2 || !(lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL))) {
        fprintf(stderr, "usage: %s vendor-libimp.so [cases] [seed] (%s)\n", argv[0], dlerror());
        return 2;
    }
    vinit = (void *(*)(void))dlsym(lib, "watermark_init");
    vembed = (int (*)(void *, uint8_t *, int, int))dlsym(lib, "embed_watermark");
    if (!vinit || !vembed || !(ctx = vinit())) {
        fprintf(stderr, "no watermark_init/embed_watermark in %s\n", argv[1]);
        return 2;
    }
    for (c = 0; c < cases; c++) {
        int stride = strides[rnd() % 6u], height = 16 + (int)(rnd() % 3u) * 8;
        size_t n = (size_t)stride * (size_t)height, i;
        uint8_t *a = malloc(n), *b = malloc(n);
        int bx, by, x, y, ra, rb;

        if (!a || !b)
            return 2;
        for (i = 0; i < n; i++)
            a[i] = (uint8_t)rnd();
        for (by = 0; by < 2; by++)
            for (bx = 0; bx < 8; bx++) {
                int kind = (int)(rnd() % 8u), base = (int)(rnd() % 256u);

                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x++)
                        a[(by * 8 + y) * stride + bx * 8 + x] = pixel(kind, base, x, y);
            }
        memcpy(b, a, n);
        if (getenv("WM_DUMP") && c == atoi(getenv("WM_DUMP"))) {
            /* 'W stride height 0 : in : vendor' + ' : ours' (64x16 blocks, tools/watermark_oracle.py format) */
            uint8_t *v = malloc(n);

            memcpy(v, a, n);
            (void)vembed(ctx, v, stride, height);
            (void)watermark_embed(b, stride, height);
            printf("W %d %d 0 : ", stride, height);
            for (y = 0; y < 16; y++) for (x = 0; x < 64; x++) printf("%02x", a[y * stride + x]);
            printf(" : ");
            for (y = 0; y < 16; y++) for (x = 0; x < 64; x++) printf("%02x", v[y * stride + x]);
            printf(" : ");
            for (y = 0; y < 16; y++) for (x = 0; x < 64; x++) printf("%02x", b[y * stride + x]);
            printf("\n");
            free(v);
            memcpy(b, a, n);
        }
        if (getenv("WM_VECTORS")) {
            /* the vendor's result on this camera as test vectors (tests/watermark) */
            printf("W %d %d 0 : ", stride, height);
            for (y = 0; y < 16; y++) for (x = 0; x < 64; x++) printf("%02x", a[y * stride + x]);
        }
        ra = vembed(ctx, a, stride, height);
        if (getenv("WM_VECTORS")) {
            printf(" : ");
            for (y = 0; y < 16; y++) for (x = 0; x < 64; x++) printf("%02x", a[y * stride + x]);
            printf("\n");
        }
        rb = watermark_embed(b, stride, height);
        if (ra != rb || memcmp(a, b, n)) {
            if (bad < 5)
                fprintf(stderr, "case %d stride %d height %d: ret %d/%d differ\n", c, stride, height, ra, rb);
            bad++;
        }
        free(a);
        free(b);
    }
    printf("devcheck: %d planes, %d mismatches\n", cases, bad);
    return bad ? 1 : 0;
}
