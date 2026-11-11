/*
 * OpenIMP T32 JPEG decoder helpers (#301, planned; not a release gate): the
 * vendor libimp 2.0.0 ijpegd_decode_header and T32P_JPEGD_SliceInit
 * (audit/issue301/dec/re).  The vendor's parser exits the process on a
 * marker it has no case for; here every unknown segment is skipped by its
 * length and a broken file is refused (-1).
 */
#include <stdint.h>
#include <string.h>

#include "t32/openimp_t32_jpegd.h"

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }


int OpenIMP_T32_DecParse(const uint8_t *p, uint32_t len, T32DecHeader *h, uint8_t *scan, uint32_t scan_max)
{
    uint32_t i = 0;

    memset(h, 0, sizeof(*h));
    while (i < len) {
        uint32_t m, seg;

        if (p[i++] != 0xff)
            continue;
        while (i < len && p[i] == 0xff)
            i++;
        if (i >= len)
            break;
        m = p[i++];
        if (m == 0x00 || m == 0x01 || (m >= 0xd0 && m <= 0xd8))
            continue;               /* stuffing, TEM, RSTn, SOI: no length */
        if (m == 0xd9)
            break;
        if (i + 2 > len)
            return -1;
        seg = (uint32_t)p[i] << 8 | p[i + 1];
        if (seg < 2 || i + seg > len)
            return -1;
        switch (m) {
        case 0xdb: {                /* DQT: Pq/Tq, 64 entries, 8 or 16 bits */
            uint32_t k = i + 2, end = i + seg;

            while (k < end) {
                uint32_t pq = p[k] >> 4, tq = p[k] & 15, n;

                k++;
                if (tq > 3 || k + (pq ? 128u : 64u) > end)
                    return -1;
                for (n = 0; n < 64; n++) {
                    h->qt[tq][n] = pq ? (uint32_t)p[k] << 8 | p[k + 1] : p[k];
                    k += pq ? 2 : 1;
                }
            }
            break;
        }
        case 0xc0: case 0xc1: case 0xc2: /* SOFn the vendor takes */
            if (seg < 8)
                return -1;
            h->height = (uint32_t)p[i + 3] << 8 | p[i + 4];
            h->width = (uint32_t)p[i + 5] << 8 | p[i + 6];
            h->comps = p[i + 7];
            h->sof = 1;
            break;
        case 0xdd:                  /* DRI */
            if (seg < 4)
                return -1;
            h->restart = (uint32_t)p[i + 2] << 8 | p[i + 3];
            break;
        case 0xda: {                /* SOS: the scan as it is, then EOI */
            uint32_t k = i + seg, n = 0;

            h->sos = 1;
            while (k < len) {
                uint8_t b = p[k++];

                if (b == 0xff && k < len && p[k] != 0x00 && (p[k] & 0xf8) != 0xd0)
                    break;          /* a marker other than stuffing / RSTn */
                if (n + 3 > scan_max)
                    return -1;
                scan[n++] = b;
            }
            scan[n++] = 0xff;
            scan[n++] = 0xd9;
            h->scan_len = n;
            return h->sof ? 0 : -1;
        }
        default:                    /* DHT (the engine has the Annex K tables), APPn, COM */
            break;
        }
        i += seg;
    }
    return -1;
}

void OpenIMP_T32_DecBuildJob(T32IocJpegDecRegInfo *j, const T32DecHeader *h, uint32_t w, uint32_t hgt,
                             uint32_t mode, uint32_t out_pa, uint32_t scan_pa)
{
    uint32_t rst = h->restart ? h->restart - 1 : 0;

    memset(j, 0, sizeof(*j));
    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t *q = &h->qt[i / 16][(i % 16) * 4];

        j->qmem[i] = q[0] | q[1] << 8 | q[2] << 16 | q[3] << 24;
    }
    j->jpeg_ctrl = 0x2000770u;
    j->nmcu = ((w >> 4) * hgt >> 3) - 1;
    j->frm_info = hgt << 16 | w;
    j->stride = align_up(w, 128);
    j->y_addr = out_pa;
    j->bs_addr = scan_pa;
    j->pixel_num = align_up(w, 16) * align_up(hgt, 8) * 2;
    j->marker_en = rst != 0;
    j->marker_num = rst;
    j->decode_mode = mode;
}

