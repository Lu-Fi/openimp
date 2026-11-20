/* #604: T41 HEVC default scaling-list tables in EP1 equal the vendor 1.2.6
 * AL_HEVC_WriteEncHwScalingList output (run on the T41 .34 with the default
 * lists: CRC-32 of the 0x6200 bytes 0x66244e9a; the T31/T40 coefficient
 * order gives a different table). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t40/t40_ep1.h"

#include <stdlib.h>

/* EncHwScalingList.c asserts through the vendor's __assert(..., &_gp);
 * the host test supplies both (no <assert.h>: glibc declares __assert) */
char _gp;
int32_t __assert(const char *expr, const char *file, int32_t line,
                 const char *fn, void *gp)
{
    (void)gp;
    fprintf(stderr, "%s:%d %s: %s\n", file, (int)line, fn, expr);
    abort();
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
    return 1; } } while (0)

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t i;
    int k;

    for (i = 0; i < n; ++i) {
        c ^= p[i];
        for (k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

int main(void)
{
    static uint8_t plain[0x6400u];
    static uint8_t scl[0x6400u];

    CHECK(openimp_t41_init_hevc_ep1(plain, sizeof(plain), 0) == 0);
    CHECK(openimp_t41_init_hevc_ep1(scl, sizeof(scl), 1) == 0);
    /* lambda unchanged, tables only from EP1+0x100 */
    CHECK(memcmp(plain, scl, 0x100u) == 0);
    CHECK(crc32_of(scl + 0x100u, 0x6200u) == 0x66244e9au);
    puts("T41 HEVC scaling-list EP1: OK");
    return 0;
}
