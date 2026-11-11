/*
 * T32 HERA host test, H.264 (#293) and H.265 (#294): OpenIMP's command
 * lists and stream headers against vendor libimp 2.2.0's.
 *
 *   hera_list_test LISTS [HEADERS] [H265_HEADERS]
 *
 * LISTS: "V <name> <words>" lines, one vendor list each (pairs up to TERM,
 * tools/t32/hera_list.py vectors).  The picture's inputs the vendor chose
 * (addresses, size, QP, type) are read from its list; the test builds the
 * list from them with t32_hera_h264_list() and compares every word.
 * HEADERS: "H <name> <w> <h> <fps> <qp> <idr> <frame_num> <idr_pic_id> <hex>"
 * lines, the vendor NAL bytes (SPS, PPS, the slice NAL's first bytes); the
 * test writes its own and compares the same number of bytes.
 * A list with BC 0x60000 is an H.265 list (t32_hera_h265_list()).
 * H265_HEADERS: "H <name> <w> <h> <fps> <qp> <idr> <poc> <hex>" lines, the
 * vendor's VPS, SPS, PPS and slice NALs (first bytes) with start codes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_hera.h"
#include "t32/openimp_t32_h264.h"
#include "t32/openimp_t32_h265.h"
#include "t32/openimp_t32_hera_h265.h"

static uint32_t find(const uint32_t *w, size_t pairs, uint32_t reg, int nth)
{
    size_t i;

    for (i = 0; i < pairs; i++)
        if ((w[2 * i + 1] & 0xffffcu) == reg && nth-- == 0)
            return w[2 * i];
    return 0;
}

static int has(const uint32_t *w, size_t pairs, uint32_t reg)
{
    size_t i;

    for (i = 0; i < pairs; i++)
        if ((w[2 * i + 1] & 0xffffcu) == reg)
            return 1;
    return 0;
}

static int check_list(const char *name, const uint32_t *v, size_t pairs)
{
    static uint32_t out[T32_HERA_LIST_MAX_PAIRS * 2];
    T32HeraH264Pic p;
    uint32_t size = find(v, pairs, 0x20008, 0);
    size_t n, i;
    int bad = 0;

    memset(&p, 0, sizeof(p));
    p.width = size & 0xffffu;
    p.height = size >> 16;
    p.p_frame = has(v, pairs, 0x30060);
    p.qp = (int)(find(v, pairs, 0x68018, 0) >> 8 & 0x3fu);
    p.src_y = find(v, pairs, 0x2000c, 0);
    p.src_c = find(v, pairs, 0x20010, 0);
    p.src_stride = find(v, pairs, 0x20014, 0) & 0xffffu;
    p.ring_y = find(v, pairs, 0x10030, 0);
    p.ring_c = find(v, pairs, 0x10034, 0);
    p.ring_y_end = find(v, pairs, 0x10038, 0);
    p.ring_c_end = find(v, pairs, 0x1003c, 0);
    p.recon_y = find(v, pairs, 0x10008, 0);
    p.recon_c = find(v, pairs, 0x1000c, 0);
    p.ref_y = find(v, pairs, 0x28014, 0);
    p.ref_c = find(v, pairs, 0x28018, 0);
    p.bs0 = find(v, pairs, 0x90008, 0);
    p.bs1 = find(v, pairs, 0x9001c, 0);
    p.section_kb = find(v, pairs, 0x9005c, 0) ? find(v, pairs, 0x90064, 0) : 0;
    p.cps = find(v, pairs, 0x9000c, 0);
    p.ai = find(v, pairs, 0x90050, 0);
    p.qpt = find(v, pairs, 0x90028, 0);
    p.ipc = find(v, pairs, 0x90030, 0);
    n = t32_hera_h264_list(&p, out);
    if (n != pairs) {
        printf("%s: %zu pairs, vendor %zu\n", name, n, pairs);
        return 1;
    }
    for (i = 0; i < 2 * n; i++)
        if (out[i] != v[i]) {
            if (bad++ < 8)
                printf("%s: pair %zu %s: %08x, vendor %08x (reg %05x)\n", name, i / 2,
                       i & 1 ? "cmd" : "value", out[i], v[i], v[(i | 1)] & 0xffffcu);
        }
    if (bad)
        printf("%s: %d words differ (%dx%d %s qp %d)\n", name, bad, p.width, p.height,
               p.p_frame ? "P" : "I", p.qp);
    return bad != 0;
}

static int compare(const char *name, const uint32_t *out, size_t n, const uint32_t *v, size_t pairs,
                   const char *what)
{
    size_t i;
    int bad = 0;

    if (n != pairs) {
        printf("%s: %zu pairs, vendor %zu\n", name, n, pairs);
        return 1;
    }
    for (i = 0; i < 2 * n; i++)
        if (out[i] != v[i]) {
            if (bad++ < 8)
                printf("%s: pair %zu %s: %08x, vendor %08x (reg %05x)\n", name, i / 2,
                       i & 1 ? "cmd" : "value", out[i], v[i], v[(i | 1)] & 0xffffcu);
        }
    if (bad)
        printf("%s: %d words differ (%s)\n", name, bad, what);
    return bad != 0;
}

static int check_list5(const char *name, const uint32_t *v, size_t pairs)
{
    static uint32_t out[T32_HERA5_LIST_MAX_PAIRS * 2];
    static const uint32_t aux_reg[T32_HERA5_AUX] = { 0x9000c, 0x90024, 0x90028, 0x90018, 0x9002c };
    T32HeraH265Pic p;
    uint32_t size = find(v, pairs, 0x20008, 0);
    char what[64];
    int i;

    memset(&p, 0, sizeof(p));
    p.width = size & 0xffffu;
    p.height = size >> 16;
    p.p_frame = has(v, pairs, 0x30060);
    p.after_p = p.p_frame && find(v, pairs, 0x90034, 0) != 0;
    p.first = (find(v, pairs, 0x2001c, 0) >> 24) == (find(v, pairs, 0x2001c, 0) >> 16 & 0xffu);
    /* the first picture: QP window QP..QP */
    p.qp = (int)(find(v, pairs, 0x60004, 0) >> 1 & 0x3fu);
    p.src_y = find(v, pairs, 0x2000c, 0);
    p.src_c = find(v, pairs, 0x20010, 0);
    p.src_stride = find(v, pairs, 0x20014, 0) & 0xffffu;
    p.ring_y = find(v, pairs, 0x10030, 0);
    p.ring_c = find(v, pairs, 0x10034, 0);
    p.ring_y_end = find(v, pairs, 0x10038, 0);
    p.ring_c_end = find(v, pairs, 0x1003c, 0);
    p.recon_y = find(v, pairs, 0x10008, 0);
    p.recon_c = find(v, pairs, 0x1000c, 0);
    p.ref_y = find(v, pairs, 0x28014, 0);
    p.ref_c = find(v, pairs, 0x28018, 0);
    p.bs0 = find(v, pairs, 0x90008, 0);
    p.bs1 = find(v, pairs, 0x9001c, 0);
    p.section_kb = find(v, pairs, 0x9005c, 0) ? find(v, pairs, 0x90064, 0) : 0;
    for (i = 0; i < T32_HERA5_AUX; i++)
        p.aux[i] = find(v, pairs, aux_reg[i], 0);
    snprintf(what, sizeof(what), "H.265 %ux%u %s qp %d", p.width, p.height,
             p.p_frame ? (p.after_p ? "P after P" : "P") : "I", p.qp);
    return compare(name, out, t32_hera_h265_list(&p, out), v, pairs, what);
}

static int check_header5(char *line)
{
    char name[64], hex[1024];
    unsigned w, h, fps, idr, poc, i;
    int qp, esc = 0;
    uint8_t want[512], got[512];
    size_t n, len;
    T32H265Seq s;
    T32H265Slice sl;

    if (sscanf(line, "H %63s %u %u %u %d %u %u %1023s", name, &w, &h, &fps, &qp, &idr, &poc, hex) != 8)
        return 1;
    len = strlen(hex) / 2;
    for (i = 0; i < len; i++)
        sscanf(hex + 2 * i, "%2hhx", &want[i]);
    memset(&s, 0, sizeof(s));
    s.width = w;
    s.height = h;
    s.fps_num = fps;
    s.fps_den = 1;
    memset(&sl, 0, sizeof(sl));
    sl.idr = (int)idr;
    sl.poc = poc;
    sl.qp = qp;
    if (!strncmp(name, "vps", 3))
        n = t32_h265_vps(&s, got);
    else if (!strncmp(name, "sps", 3))
        n = t32_h265_sps(&s, got);
    else if (!strncmp(name, "pps", 3))
        n = t32_h265_pps(&s, got);
    else
        n = t32_h265_slice_head(&s, &sl, got, &esc);
    if (!n || (!strncmp(name, "slice", 5) ? n > len : n != len) || memcmp(got, want, n)) {
        printf("%s: %zu bytes, vendor %zu:", name, n, len);
        for (i = 0; i < n; i++)
            printf(" %02x/%02x", got[i], i < len ? want[i] : 0);
        printf("\n");
        return 1;
    }
    return 0;
}

static int check_header(char *line)
{
    char name[64], hex[1024];
    unsigned w, h, fps, idr, fnum, idr_id, i;
    int qp, esc = 0;
    uint8_t want[512], got[512];
    size_t n, len;
    T32H264Seq s;
    T32H264Slice sl;

    if (sscanf(line, "H %63s %u %u %u %d %u %u %u %1023s", name, &w, &h, &fps, &qp, &idr, &fnum,
               &idr_id, hex) != 9)
        return 1;
    len = strlen(hex) / 2;
    for (i = 0; i < len; i++)
        sscanf(hex + 2 * i, "%2hhx", &want[i]);
    memset(&s, 0, sizeof(s));
    s.width = w;
    s.height = h;
    s.fps_num = fps;
    s.fps_den = 1;
    s.init_qp = qp;
    memset(&sl, 0, sizeof(sl));
    sl.idr = (int)idr;
    sl.frame_num = fnum;
    sl.idr_pic_id = idr_id;
    sl.qp = qp;
    if (!strncmp(name, "sps", 3))
        n = t32_h264_sps(&s, got);
    else if (!strncmp(name, "pps", 3))
        n = t32_h264_pps(&s, got);
    else
        n = t32_h264_slice_head(&s, &sl, got, &esc);
    if (!strncmp(name, "slice", 5) ? n > len : n != len) {
        printf("%s: %zu bytes, vendor %zu\n", name, n, len);
        return 1;
    }
    if (memcmp(got, want, n)) {
        printf("%s: bytes differ:", name);
        for (i = 0; i < n; i++)
            printf(" %02x/%02x", got[i], want[i]);
        printf("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static char line[65536];
    static uint32_t v[T32_HERA_LIST_MAX_PAIRS * 2];
    int lists = 0, heads = 0, lists5 = 0, heads5 = 0, bad = 0, bad5 = 0;
    FILE *f;

    if (argc < 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: hera_list_test LISTS [HEADERS]\n");
        return 2;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[64], *p;
        size_t n = 0;
        int off;

        if (line[0] != 'V' || sscanf(line, "V %63s %n", name, &off) != 1)
            continue;
        for (p = line + off; n < T32_HERA_LIST_MAX_PAIRS * 2; ) {
            char *end;
            unsigned long x = strtoul(p, &end, 16);
            if (end == p)
                break;
            v[n++] = (uint32_t)x;
            p = end;
            if (!(n & 1) && v[n - 1] & (1u << 30))
                break;
        }
        if (has(v, n / 2, 0x60000)) {
            bad5 += check_list5(name, v, n / 2);
            lists5++;
        } else {
            bad += check_list(name, v, n / 2);
            lists++;
        }
    }
    fclose(f);
    if (argc > 2 && (f = fopen(argv[2], "r"))) {
        while (fgets(line, sizeof(line), f))
            if (line[0] == 'H') {
                bad += check_header(line);
                heads++;
            }
        fclose(f);
    }
    if (argc > 3 && (f = fopen(argv[3], "r"))) {
        while (fgets(line, sizeof(line), f))
            if (line[0] == 'H') {
                bad5 += check_header5(line);
                heads5++;
            }
        fclose(f);
    }
    printf("HERA H.264: %d lists, %d headers vs vendor libimp 2.2.0: %s\n", lists, heads,
           bad ? "DIFFERENT" : "identical");
    if (lists5 || heads5)
        printf("HERA H.265: %d lists, %d headers vs vendor libimp 2.2.0: %s\n", lists5, heads5,
               bad5 ? "DIFFERENT" : "identical");
    return bad + bad5 != 0;
}
