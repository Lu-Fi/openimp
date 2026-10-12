#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t40/t40_stream_layout.h"

static int expect_valid(uint32_t capacity, uint32_t payload_offset,
                        uint32_t header_size, uint32_t payload_status,
                        uint32_t payload_size, uint32_t payload_end,
                        uint32_t access_unit_size)
{
    OpenIMPT40StreamLayout layout = {0};

    if (openimp_t40_stream_layout(capacity, payload_offset, header_size,
                                  payload_status, &layout) != 0 ||
        layout.payload_size != payload_size ||
        layout.payload_end != payload_end ||
        layout.access_unit_size != access_unit_size) {
        fprintf(stderr,
                "valid layout mismatch: cap=%u off=%u hdr=%u status=0x%08x got=%u/%u/%u expected=%u/%u/%u\n",
                capacity, payload_offset, header_size, payload_status,
                layout.payload_size, layout.payload_end,
                layout.access_unit_size, payload_size, payload_end,
                access_unit_size);
        return 1;
    }
    return 0;
}

static int expect_invalid(uint32_t capacity, uint32_t payload_offset,
                          uint32_t header_size, uint32_t payload_status)
{
    OpenIMPT40StreamLayout layout = {0};

    if (openimp_t40_stream_layout(capacity, payload_offset, header_size,
                                  payload_status, &layout) == 0) {
        fprintf(stderr,
                "invalid layout accepted: cap=%u off=%u hdr=%u status=0x%08x\n",
                capacity, payload_offset, header_size, payload_status);
        return 1;
    }
    return 0;
}

/* user_data_unregistered SEI: NAL bytes and the insertion in front of the
 * slice NAL of a header region */
static int sei_tests(void)
{
    static const uint8_t uuid[16] = {
        0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
        0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef };
    uint8_t out[OPENIMP_T40_SEI_MAX_NAL], big[130], hdr[128];
    uint8_t data[4] = { 'a', 0, 0, 1 };      /* 00 00 01 needs escaping */
    uint32_t n, h;
    int failed = 0;

    n = openimp_t40_user_sei_build(0, uuid, data, 4, out, sizeof(out));
    /* 4 start + 1 header + type + size(20) + 16 + 4 + 1 escape + 0x80 */
    if (n != 4u + 1u + 2u + 16u + 4u + 1u + 1u || out[3] != 1 || out[4] != 6 ||
        out[5] != 5 || out[6] != 20 || memcmp(out + 7, uuid, 16) != 0 ||
        out[23] != 'a' || out[24] != 0 || out[25] != 0 || out[26] != 3 ||
        out[27] != 1 || out[28] != 0x80) {
        fprintf(stderr, "AVC SEI NAL wrong (n=%u)\n", n);
        failed = 1;
    }
    n = openimp_t40_user_sei_build(1, uuid, data, 0, out, sizeof(out));
    if (n != 4u + 2u + 2u + 16u + 1u || out[4] != 0x4e || out[5] != 0x01 ||
        out[6] != 5 || out[7] != 16) {
        fprintf(stderr, "HEVC SEI NAL wrong (n=%u)\n", n);
        failed = 1;
    }
    memset(big, 0x41, sizeof(big));
    n = openimp_t40_user_sei_build(0, uuid, big, 128, out, sizeof(out));
    if (n != 4u + 1u + 2u + 16u + 128u + 1u || out[6] != 144) {
        fprintf(stderr, "max SEI wrong (n=%u)\n", n);
        failed = 1;
    }
    if (openimp_t40_user_sei_build(0, uuid, big, 129, out, sizeof(out)) != 0 ||
        openimp_t40_user_sei_build(0, NULL, big, 4, out, sizeof(out)) != 0 ||
        openimp_t40_user_sei_build(0, uuid, NULL, 4, out, sizeof(out)) != 0 ||
        openimp_t40_user_sei_build(0, uuid, big, 128, out, 40) != 0) {
        fprintf(stderr, "bad SEI arguments accepted\n");
        failed = 1;
    }

    /* IDR header: SPS PPS slice -> SEI goes between PPS and slice */
    {
        static const uint8_t base[] = {
            0, 0, 0, 1, 0x27, 1, 2, 3,  0, 0, 0, 1, 0x28, 4, 5,
            0, 0, 0, 1, 0x25, 6, 7, 8 };
        uint8_t sei[8] = { 0, 0, 0, 1, 6, 9, 9, 9 };

        memcpy(hdr, base, sizeof(base));
        h = openimp_t40_sei_insert(hdr, sizeof(hdr), sizeof(base), sei, 8);
        if (h != sizeof(base) + 8u || memcmp(hdr, base, 15) != 0 ||
            memcmp(hdr + 15, sei, 8) != 0 || memcmp(hdr + 23, base + 15, 8) != 0) {
            fprintf(stderr, "IDR SEI insert wrong (h=%u)\n", h);
            failed = 1;
        }
        /* P header: a single slice NAL, SEI in front */
        memcpy(hdr, base + 15, 8);
        h = openimp_t40_sei_insert(hdr, sizeof(hdr), 8, sei, 8);
        if (h != 16u || memcmp(hdr, sei, 8) != 0 || memcmp(hdr + 8, base + 15, 8) != 0) {
            fprintf(stderr, "P SEI insert wrong (h=%u)\n", h);
            failed = 1;
        }
        /* three-byte start code */
        { static const uint8_t b3[] = { 0, 0, 1, 0x41, 9, 9 };
          memcpy(hdr, b3, 6);
          h = openimp_t40_sei_insert(hdr, sizeof(hdr), 6, sei, 8);
          if (h != 14u || memcmp(hdr, sei, 8) != 0 || memcmp(hdr + 8, b3, 6) != 0) {
              fprintf(stderr, "3-byte start code insert wrong (h=%u)\n", h);
              failed = 1;
          } }
        /* refusals: no start code, no room */
        memset(hdr, 7, 16);
        if (openimp_t40_sei_insert(hdr, sizeof(hdr), 16, sei, 8) != 0 ||
            openimp_t40_sei_insert(hdr, 20, 16, sei, 8) != 0) {
            fprintf(stderr, "bad insert accepted\n");
            failed = 1;
        }
    }
    return failed;
}

int main(void)
{
    int failed = 0;

    /* Archived T40 values: OEM sub-IDR=0x5e9, flat probe=0x127. */
    failed |= expect_valid(0x384000u, 0x220u, 42u, 0x5e9u,
                           0x5e9u, 0x809u, 0x613u);
    failed |= expect_valid(0x384000u, 0x220u, 60u, 0xc0000127u,
                           0x127u, 0x347u, 0x163u);
    failed |= expect_valid(0x1000u, 0x220u, 0x220u, 0xde0u,
                           0xde0u, 0x1000u, 0x1000u);
    failed |= expect_invalid(0x1000u, 0x220u, 42u, 0u);
    failed |= expect_invalid(0x1000u, 0x220u, 0x221u, 1u);
    failed |= expect_invalid(0x1000u, 0x1001u, 42u, 1u);
    failed |= expect_invalid(0x1000u, 0x220u, 42u, 0xde1u);

    if (openimp_t40_stream_layout(0x1000u, 0x220u, 42u, 1u,
                                  NULL) == 0) {
        fprintf(stderr, "NULL result accepted\n");
        failed = 1;
    }

    failed |= sei_tests();

    if (failed)
        return 1;
    puts("T40 stream-layout tests passed");
    return 0;
}
