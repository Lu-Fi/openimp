/*
 * T32 IMP_Decoder helpers (#301, planned): host test of the JPEG header parse
 * and the engine job (src/t32/openimp_t32_jpegd.c) against the vendor libimp
 * 2.0.0 rules (ijpegd_decode_header, T32P_JPEGD_SliceInit; RE notes in
 * audit/issue301/dec).  The file is built here from markers only (no
 * picture): DQT 0 and 1, SOF0 4:2:2, DHT, DRI, SOS and a scan with stuffing
 * and restart markers, then EOI.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_jpegd.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t put(uint8_t *b, uint32_t n, const uint8_t *s, uint32_t len)
{
    memcpy(b + n, s, len);
    return n + len;
}

static uint32_t build(uint8_t *b, uint32_t w, uint32_t h, uint32_t dri, int sixteen)
{
    uint32_t n = 0, i;
    uint8_t soi[] = { 0xff, 0xd8 }, app0[] = { 0xff, 0xe0, 0, 4, 'J', 'F' };
    uint8_t sof[] = { 0xff, 0xc0, 0, 17, 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3,
                      1, 0x21, 0, 2, 0x11, 1, 3, 0x11, 1 };
    uint8_t dht[] = { 0xff, 0xc4, 0, 5, 0x00, 0x00, 0x00 };
    uint8_t dri_seg[] = { 0xff, 0xdd, 0, 4, (uint8_t)(dri >> 8), (uint8_t)dri };
    uint8_t sos[] = { 0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    uint8_t scan[] = { 0x12, 0xff, 0x00, 0x34, 0xff, 0xd0, 0x56, 0xff, 0xd1, 0x78 };
    uint8_t eoi[] = { 0xff, 0xd9 };

    n = put(b, n, soi, sizeof soi);
    n = put(b, n, app0, sizeof app0);
    for (uint32_t t = 0; t < 2; t++) {       /* DQT: table t, entry k = 1 + k + 64 t */
        uint8_t hd[] = { 0xff, 0xdb, 0, (uint8_t)(sixteen ? 131 : 67), (uint8_t)((sixteen ? 0x10 : 0) | t) };
        n = put(b, n, hd, sizeof hd);
        for (i = 0; i < 64; i++) {
            if (sixteen)
                b[n++] = 0;
            b[n++] = (uint8_t)(1 + i + 64 * t);
        }
    }
    n = put(b, n, sof, sizeof sof);
    n = put(b, n, dht, sizeof dht);
    if (dri)
        n = put(b, n, dri_seg, sizeof dri_seg);
    n = put(b, n, sos, sizeof sos);
    n = put(b, n, scan, sizeof scan);
    return put(b, n, eoi, sizeof eoi);
}

int main(void)
{
    static uint8_t file[4096], scan[256];
    static const uint8_t want_scan[] = { 0x12, 0xff, 0x00, 0x34, 0xff, 0xd0, 0x56, 0xff, 0xd1, 0x78, 0xff, 0xd9 };
    T32DecHeader h;
    T32IocJpegDecRegInfo j;
    uint32_t n;

    /* 1920x1080, restart interval 8, 8-bit tables */
    n = build(file, 1920, 1080, 8, 0);
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, sizeof scan) == 0);
    CHECK(h.width == 1920 && h.height == 1080 && h.comps == 3 && h.restart == 8);
    CHECK(h.qt[0][0] == 1 && h.qt[0][63] == 64 && h.qt[1][0] == 65 && h.qt[1][63] == 128);
    /* the scan as it is: FF00, RSTn and fill bytes kept, EOI appended */
    CHECK(h.scan_len == sizeof want_scan && !memcmp(scan, want_scan, sizeof want_scan));
    OpenIMP_T32_DecBuildJob(&j, &h, 1920, 1080, 0, 0x5000000, 0x6000000);
    CHECK(j.qmem[0] == 0x04030201u && j.qmem[15] == 0x403f3e3du);
    CHECK(j.qmem[16] == 0x44434241u && j.qmem[31] == 0x807f7e7du);
    CHECK(j.jpeg_ctrl == 0x02000770u);
    CHECK(j.nmcu == 120u * 135u - 1);                 /* 16x8 MCUs: 0x3f47 */
    CHECK(j.frm_info == (1080u << 16 | 1920u) && j.stride == 1920);
    CHECK(j.y_addr == 0x5000000 && j.c_addr == 0 && j.bs_addr == 0x6000000);
    CHECK(j.pixel_num == 1920u * 1080u * 2);
    CHECK(j.marker_en == 1 && j.marker_num == 7);
    CHECK(j.limit_bs_len == 0 && j.bsfull_jpeg_intr_size == 0 && j.timeout_thrd == 0 && j.decode_mode == 0);

    /* no DRI: markers off; restart interval 1: the vendor's marker_en is 0 too */
    n = build(file, 640, 360, 0, 0);
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, sizeof scan) == 0 && h.restart == 0);
    OpenIMP_T32_DecBuildJob(&j, &h, 640, 360, 3, 1, 2);
    CHECK(j.marker_en == 0 && j.marker_num == 0 && j.decode_mode == 3);
    CHECK(j.stride == 640 && j.nmcu == 40u * 45u - 1 && j.frm_info == (360u << 16 | 640u));
    n = build(file, 640, 360, 1, 0);
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, sizeof scan) == 0 && h.restart == 1);
    OpenIMP_T32_DecBuildJob(&j, &h, 656, 360, 1, 1, 2);
    CHECK(j.marker_en == 0 && j.marker_num == 0 && j.stride == 768);

    /* 16-bit tables: values as the vendor packs them (no masking) */
    n = build(file, 320, 240, 0, 1);
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, sizeof scan) == 0 && h.qt[1][63] == 128);

    /* broken files are refused: no SOS, truncated segment, scan buffer too small */
    n = build(file, 320, 240, 0, 0);
    CHECK(OpenIMP_T32_DecParse(file, 40, &h, scan, sizeof scan) == -1);
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, 8) == -1);
    file[2 + 6 + 3] = 0xff;                           /* DQT length past the end */
    CHECK(OpenIMP_T32_DecParse(file, n, &h, scan, sizeof scan) == -1);

    if (fails)
        return 1;
    printf("T32 decoder header parse + engine job: ok\n");
    return 0;
}
