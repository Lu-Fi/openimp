/*
 * T33 HERA v1.6 host test (#391): OpenIMP's H.264 command lists against
 * vendor libimp 2.0.2.1's (QEMU -M ingenic-t33 FIXQP traces).
 *
 *   t33_hera_list_test LISTS
 *
 * LISTS: "V <name> <words>" lines, one vendor list each (pairs up to TERM,
 * tools/t32/hera_list.py vectors).  The picture's inputs the vendor chose
 * (addresses, size, QP, type, header set) are read from its list; the test
 * builds the list with t33_hera_h264_list() and compares every word, and
 * checks t33_hera_ring_layout() against the vendor's area and header sizes.
 * A list with BC 0x60000 is an H.265 list (t33_hera_h265_list()).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t33_hera.h"
#include "t32/openimp_t33_hera_h265.h"

static uint32_t find(const uint32_t *w, size_t pairs, uint32_t reg)
{
    size_t i;

    for (i = 0; i < pairs; i++)
        if ((w[2 * i + 1] & 0xffffcu) == reg)
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

static int check_layout(const char *name, const T33HeraH264Pic *t, uint32_t hdr_a)
{
    const T32HeraH264Pic *p = &t->pic;
    T33HeraRing r;
    int bad = 0;

    t33_hera_ring_layout(p->width, p->height, &r);
    bad |= r.y != p->ring_y_end - p->ring_y || r.c != p->ring_c_end - p->ring_c;
    bad |= hdr_a != p->ring_c_end;                      /* the first set follows the C area */
    bad |= r.hy != t->hdr_wc - t->hdr_wy && r.hy != t->hdr_rc - t->hdr_ry;
    bad |= t->hdr_ry != t->hdr_wy + r.hy + r.hc && t->hdr_wy != t->hdr_ry + r.hy + r.hc;
    if (bad)
        printf("%s: %ux%u area Y %u C %u hdr %u %u, vendor Y %u C %u hdr Y %u\n", name, p->width,
               p->height, r.y, r.c, r.hy, r.hc, p->ring_y_end - p->ring_y, p->ring_c_end - p->ring_c,
               t->hdr_wc - t->hdr_wy);
    return bad;
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

static int check_h265(const char *name, const uint32_t *v, size_t pairs)
{
    static uint32_t out[T32_HERA5_LIST_MAX_PAIRS * 2];
    T33HeraH265Pic t;
    T32HeraH265Pic *p = &t.pic;
    uint32_t size = find(v, pairs, 0x20008), hy, hc, base;
    char what[64];
    int bad = 0;
    unsigned set;

    memset(&t, 0, sizeof(t));
    p->width = size & 0xffffu;
    p->height = size >> 16;
    p->p_frame = has(v, pairs, 0x30060);
    p->after_p = p->p_frame && (find(v, pairs, 0x30000) & (1u << 11));
    p->qp = (int)(find(v, pairs, 0x60004) >> 1 & 0x3fu);
    p->src_y = find(v, pairs, 0x2000c);
    p->src_c = find(v, pairs, 0x20010);
    p->src_stride = find(v, pairs, 0x20014) & 0xffffu;
    p->ring_y = find(v, pairs, 0x10030);
    p->ring_c = find(v, pairs, 0x10034);
    p->ring_y_end = find(v, pairs, 0x10038);
    p->ring_c_end = find(v, pairs, 0x1003c);
    p->bs0 = find(v, pairs, 0x90008);
    p->bs1 = find(v, pairs, 0x9001c);
    p->section_kb = find(v, pairs, 0x9005c) ? find(v, pairs, 0x90064) : 0;
    p->aux[T32_HERA5_CPS] = find(v, pairs, 0x9000c);
    p->aux[T32_HERA5_A24] = find(v, pairs, 0x90024);
    p->aux[T32_HERA5_QPT] = find(v, pairs, 0x90028);
    p->aux[T32_HERA5_AI] = find(v, pairs, 0x90018);
    p->aux[T32_HERA5_A2C] = find(v, pairs, 0x9002c);
    t.hdr_wy = find(v, pairs, 0x10020);
    t.hdr_wc = find(v, pairs, 0x10024);
    t.hdr_ry = find(v, pairs, 0x28008);
    t.hdr_rc = find(v, pairs, 0x2800c);
    t.recon_y = find(v, pairs, 0x10008);       /* the vendor's read-back (0 in QEMU) */
    t.recon_c = find(v, pairs, 0x1000c);
    t.ref_y = find(v, pairs, 0x28014);
    t.ref_c = find(v, pairs, 0x28018);
    /* the header sets follow the C area: Y A, Y B, C A, C B */
    base = t.hdr_wy < t.hdr_ry ? t.hdr_wy : t.hdr_ry;
    set = t.hdr_wy != base;
    t33_hera_h265_hdr(p->width, p->height, p->ring_c_end, set, &hy, &hc);
    if (base != p->ring_c_end || hy != t.hdr_wy || hc != t.hdr_wc) {
        printf("%s: header set %u at %08x / %08x, vendor %08x / %08x\n", name, set, hy, hc, t.hdr_wy,
               t.hdr_wc);
        bad = 1;
    }
    snprintf(what, sizeof(what), "H.265 %ux%u %s qp %d", p->width, p->height, p->p_frame ? "P" : "I",
             p->qp);
    return compare(name, out, t33_hera_h265_list(&t, out), v, pairs, what) | bad;
}

static int check_list(const char *name, const uint32_t *v, size_t pairs)
{
    static uint32_t out[T32_HERA_LIST_MAX_PAIRS * 2];
    T33HeraH264Pic t;
    T32HeraH264Pic *p = &t.pic;
    uint32_t size = find(v, pairs, 0x20008);
    size_t n, i;
    int bad = 0;

    memset(&t, 0, sizeof(t));
    p->width = size & 0xffffu;
    p->height = size >> 16;
    p->p_frame = has(v, pairs, 0x30060);
    p->qp = (int)(find(v, pairs, 0x68018) >> 8 & 0x3fu);
    p->src_y = find(v, pairs, 0x2000c);
    p->src_c = find(v, pairs, 0x20010);
    p->src_stride = find(v, pairs, 0x20014) & 0xffffu;
    p->ring_y = find(v, pairs, 0x10030);
    p->ring_c = find(v, pairs, 0x10034);
    p->ring_y_end = find(v, pairs, 0x10038);
    p->ring_c_end = find(v, pairs, 0x1003c);
    p->bs0 = find(v, pairs, 0x90008);
    p->bs1 = find(v, pairs, 0x9001c);
    p->section_kb = find(v, pairs, 0x9005c) ? find(v, pairs, 0x90064) : 0;
    p->cps = find(v, pairs, 0x9000c);
    p->ai = find(v, pairs, 0x90050);
    p->qpt = find(v, pairs, 0x90028);
    p->ipc = find(v, pairs, 0x90030);
    t.hdr_wy = find(v, pairs, 0x10020);
    t.hdr_wc = find(v, pairs, 0x10024);
    t.hdr_ry = find(v, pairs, 0x28008);
    t.hdr_rc = find(v, pairs, 0x2800c);
    t.recon_y = find(v, pairs, 0x10008);       /* the vendor's read-back (0 in QEMU) */
    t.recon_c = find(v, pairs, 0x1000c);
    t.ref_y = find(v, pairs, 0x28014);
    t.ref_c = find(v, pairs, 0x28018);
    bad = check_layout(name, &t, t.hdr_wy < t.hdr_ry ? t.hdr_wy : t.hdr_ry);
    n = t33_hera_h264_list(&t, out);
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
        printf("%s: %d words differ (%ux%u %s qp %d)\n", name, bad, p->width, p->height,
               p->p_frame ? "P" : "I", p->qp);
    return bad != 0;
}

int main(int argc, char **argv)
{
    static char line[65536];
    static uint32_t v[T32_HERA_LIST_MAX_PAIRS * 2];
    FILE *f;
    int lists = 0, fails = 0;

    if (argc < 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s LISTS\n", argv[0]);
        return 2;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[64], *s = line;
        size_t n = 0;
        int used;

        if (line[0] != 'V' || sscanf(line, "V %63s %n", name, &used) != 1)
            continue;
        s += used;
        while (n < 2 * T32_HERA_LIST_MAX_PAIRS) {
            char *end;
            unsigned long w = strtoul(s, &end, 16);

            if (end == s)
                break;
            v[n++] = (uint32_t)w;
            s = end;
        }
        lists++;
        fails += has(v, n / 2, 0x60000) ? check_h265(name, v, n / 2) : check_list(name, v, n / 2);
    }
    fclose(f);
    printf("T33 HERA H.264 / H.265 lists: %d of %d identical to vendor 2.0.2.1\n", lists - fails, lists);
    return lists && !fails ? 0 : 1;
}
