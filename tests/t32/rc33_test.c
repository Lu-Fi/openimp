/*
 * T32 H.264 rate control, eprc T33 revision (#295) host test: OpenIMP's
 * port (src/eprc/eprc_t33_h264.c + src/t32/openimp_t32_rc33.c) on the
 * synthetic size model of tools/eprc_t33_oracle.py must give the vendor
 * 2.2.0 QP of every picture:
 *
 *   rc33_test [-m N] [-r] [-l] FILE ...   N: the per-picture motion mode of the runs
 *                               in the files after it (default 0); -r: the
 *                               HERA per-row IDR slice sizes of the model go in
 *                               and their sum (P+0x6bc) must equal the vendor's;
 *                               -l: each 'F' line has an 'L' line, the HERA
 *                               command-list words vendor H264E_T32V_SliceInit
 *                               makes from the slice block, which OpenIMP's list
 *                               (t32_hera_h264_list on the port's slice block)
 *                               must carry the same, in the same place
 *
 * FILE: 'S mode kbps pictures' runs (oracle run(), vendor-2.2.0-rc-h264.txt)
 * or random scenarios (oracle scenario(), vendor-2.2.0-rc33-h264.txt).  The
 * older files are vendor runs with the motion mode the harness left at 0;
 * i264e_update_fenc sets 2 on the device (--motion 2, *-motion2-*.txt).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_hera.h"
#include "t32/openimp_t32_rc33.h"

/* the registers of an 'L' line (tools/eprc_t33_oracle.py SLICE_REGS), then the row words */
static const uint32_t slice_regs[] = { 0x2001c, 0x2029c, 0x40100, 0x40104, 0x40108, 0x40114, 0x40118, 0x4011c,
                                       0x40120, 0x40124 };
#define NSLICE (sizeof(slice_regs) / sizeof(slice_regs[0]))

/* OpenIMP's list for the picture in work: the 'L' words; -1 when a register or the row block is missing */
struct cfg_pic {
    uint32_t w, h;
    int idr;
};

static int list_words(T32Rc33 *rc, const struct cfg_pic *c, uint32_t *out)
{
    static uint32_t list[T32_HERA_LIST_MAX_PAIRS * 2];
    T32HeraH264Pic p;
    size_t n, i;
    unsigned k, got = 0;
    int nw = -1;

    memset(&p, 0, sizeof(p));
    p.width = c->w;
    p.height = c->h;
    p.p_frame = !c->idr;
    p.qp = rc->qp;
    p.rc_slice = t32_rc33_slice(rc);
    n = t32_hera_h264_list(&p, list);
    for (i = 0; i < n; i++) {
        uint32_t reg = list[2 * i + 1] & 0xffffcu;

        for (k = 0; k < NSLICE; k++)
            if (reg == slice_regs[k]) {
                out[k] = list[2 * i];
                got |= 1u << k;
            }
        if (reg == slice_regs[NSLICE - 1]) {
            unsigned r = 0;

            while (i + 1 + r < n && (list[2 * (i + 1 + r) + 1] & 0xffffcu) == 0x40900u + 4u * r) {
                out[NSLICE + 1 + r] = list[2 * (i + 1 + r)];
                r++;
            }
            out[NSLICE] = r;
            nw = (int)(NSLICE + 1 + r);
        }
    }
    return got == (1u << NSLICE) - 1u ? nw : -1;
}

static uint32_t model_bytes(int qp, int idr, uint32_t mbs, double cplx)
{
    double bits = cplx * mbs * pow(2.0, (26.0 - qp) / 6.0) * (idr ? 5.0 : 1.0);

    return (uint32_t)(int64_t)(bits / 8.0) + 16u;
}

static void model_stats(int qp, uint32_t nbytes, uint32_t mbs, uint32_t st[T33_RC_STATS])
{
    memset(st, 0, T33_RC_STATS * sizeof(st[0]));
    st[8] = (uint32_t)qp * mbs & 0xffffu;
    st[10] = mbs & 0xffffu;
    st[12] = nbytes * 3u;
    st[15] = mbs * 4u;
    st[20] = nbytes / 2u;
    st[21] = nbytes / 3u;
}

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

struct scene {
    uint32_t x;
    double lvl;
    int still;
};

static double scene_next(struct scene *s)
{
    static const double levels[5] = { 5.0, 20.0, 60.0, 120.0, 250.0 };
    uint32_t u;

    s->x = u = xorshift32(s->x);
    if (u % 100 < 3)
        s->lvl = levels[(u >> 8) % 5];
    if ((u >> 16) % 100 < 2)
        s->still = 10 + (int)((u >> 20) % 50);
    if (s->still > 0) {
        s->still--;
        return 1.0 + ((u >> 4) & 255) / 256.0;
    }
    {
        double v = s->lvl * (0.8 + 0.4 * ((u >> 12) & 1023) / 1024.0);

        return v > 0.5 ? v : 0.5;
    }
}

struct cfg {
    int mode, kbps, w, h, gop, fps, minqp, maxqp, initqp, pics;
    uint32_t seed;              /* scene seed, 0: the run() model */
};

/* the per-row IDR slice sizes of the model (tools/eprc_t33_oracle.py model_rows) */
static unsigned model_rows(uint32_t nbytes, int qp, unsigned n, uint32_t *rows)
{
    for (unsigned i = 0; i < n; i++)
        rows[i] = (nbytes * 8u / n) * (6u + (i * 5u + (unsigned)qp) % 7u) / 9u;
    return n;
}

/* one oracle run; returns the number of pictures whose QP differs */
static int run(const struct cfg *c, FILE *ref, int *faults, int motion, int with_rows, int with_list)
{
    T32Rc33 rc;
    uint32_t mbs = (uint32_t)(c->w / 16) * (uint32_t)(c->h / 16), st[T33_RC_STATS];
    struct scene sc = { c->seed, 60.0, 0 };
    char line[1024];
    int bad = 0;

    if (t32_rc33_open(&rc, (uint32_t)c->w, (uint32_t)c->h) != 0) {
        printf("FAIL: no memory\n");
        return c->pics;
    }
    t32_rc33_set(&rc, T33P_GOP, (uint32_t)c->gop);
    t32_rc33_set(&rc, T33P_FPS_NUM, (uint32_t)c->fps);
    t32_rc33_set(&rc, T33P_FPS_DEN, 1);
    t32_rc33_set(&rc, T33P_MODE, (uint32_t)c->mode);
    t32_rc33_set(&rc, T33P_KBPS, (uint32_t)c->kbps);
    t32_rc33_set(&rc, T33P_MIN_QP, (uint32_t)c->minqp);
    t32_rc33_set(&rc, T33P_MAX_QP, (uint32_t)c->maxqp);
    t32_rc33_set(&rc, T33P_INIT_QP, (uint32_t)c->initqp);
    t32_rc33_set(&rc, T33P_MAX_IQP, (uint32_t)c->maxqp);
    t32_rc33_set(&rc, T33P_MIN_IQP, (uint32_t)c->minqp);
    if (c->mode == 4) {
        t32_rc33_set(&rc, T33P_LONG_MAX_KBPS, (uint32_t)c->kbps);
        t32_rc33_set(&rc, T33P_LONG_MIN_KBPS, (uint32_t)c->kbps / 4u);
    }
    t32_rc33_init(&rc);
    t32_rc33_set_motion(&rc, (uint8_t)motion);
    for (int i = 0; i < c->pics; i++) {
        double cplx = c->seed ? scene_next(&sc) : 60.0 * (c->pics / 3 <= i && i < 2 * c->pics / 3 ? 2.0 : 1.0);
        int idr = i % c->gop == 0, qp, ri, rt, rq, nf;
        uint32_t nb, rows[T33_RC_ROWS];
        unsigned long rb, rs = 0;
        unsigned nrows;

        uint32_t lw[NSLICE + 1 + T33_RC_ROWS];
        int nlw = 0;

        (void)t32_rc33_start(&rc, rc.n % (uint32_t)c->gop);
        qp = rc.qp;
        if (with_list) {
            struct cfg_pic cp = { (uint32_t)c->w, (uint32_t)c->h, idr };

            nlw = list_words(&rc, &cp, lw);
        }
        nb = model_bytes(qp, idr, mbs, cplx);
        model_stats(qp, nb, mbs, st);
        nrows = with_rows ? t32_rc33_idr_rows(&rc) : 0u;
        t32_rc33_end_rows(&rc, nb, st, rows, model_rows(nb, qp, nrows, rows));
        if (!fgets(line, sizeof(line), ref) ||
            (nf = sscanf(line, "F %d %d %d %lu %lu", &ri, &rt, &rq, &rb, &rs)) < 4 || ri != i) {
            printf("FAIL: reference ends at picture %d\n", i);
            bad += c->pics - i;
            break;
        }
        /* with rows: the IDR slice size sum the controller keeps (P+0x6bc) */
        if (rq != qp || rt != (idr ? 2 : 0) || rb != nb || (nf == 5 && rs != t32_rc33_get(&rc, 0x6bc))) {
            if (bad < 3)
                printf("  picture %d: vendor type %d qp %d size %lu, OpenIMP qp %d size %u\n", i, rt, rq, rb, qp,
                       nb);
            bad++;
        } else if (with_list) {
            /* the vendor's list words of this picture */
            char *t = fgets(line, sizeof(line), ref) && line[0] == 'L' ? line + 1 : NULL;
            int k = 0, same = nlw > 0;

            while (t && same) {
                char *e;
                unsigned long v = strtoul(t, &e, 16);

                if (e == t)
                    break;
                same = k < nlw && lw[k] == (uint32_t)v;
                t = e;
                k++;
            }
            if (!t || !same || k != nlw) {
                if (bad < 3)
                    printf("  picture %d: list word %d differs (OpenIMP %08x, %d words)\n", i, k - 1,
                           k > 0 && k <= nlw ? lw[k - 1] : 0u, nlw);
                bad++;
            }
        }
    }
    *faults += rc.X.fault;
    t32_rc33_close(&rc);
    return bad;
}

int main(int argc, char **argv)
{
    int runs = 0, failed = 0, pics = 0, faults = 0, motion = 0, with_rows = 0, with_list = 0;

    for (int a = 1; a < argc; a++) {
        FILE *f;
        char line[256];

        if (!strcmp(argv[a], "-m") && a + 1 < argc) {
            motion = atoi(argv[++a]);
            continue;
        }
        if (!strcmp(argv[a], "-r")) {
            with_rows = 1;
            continue;
        }
        if (!strcmp(argv[a], "-l")) {
            with_list = 1;
            continue;
        }
        f = fopen(argv[a], "r");
        if (!f) {
            printf("FAIL: %s\n", argv[a]);
            return 1;
        }
        while (fgets(line, sizeof(line), f)) {
            struct cfg c = { 0, 0, 1920, 1088, 50, 25, 15, 48, 30, 0, 0 };
            int n, bad;

            if (line[0] != 'S')
                continue;
            n = sscanf(line, "S %d %d %d %d %d %d %d %d %d %d %u", &c.mode, &c.kbps, &c.w, &c.h, &c.gop, &c.fps,
                       &c.minqp, &c.maxqp, &c.initqp, &c.pics, &c.seed);
            if (n == 3) {               /* S mode kbps pictures */
                c.pics = c.w;
                c.w = 1920;
                c.h = 1088;
                c.gop = 50;
                c.fps = 25;
                c.minqp = 15;
                c.maxqp = 48;
                c.initqp = 30;
                c.seed = 0;
            } else if (n != 11 || c.seed == 0) {
                printf("FAIL: %s: %s", argv[a], line);
                failed++;
                continue;
            }
            bad = run(&c, f, &faults, motion, with_rows, with_list);
            runs++;
            pics += c.pics;
            if (bad) {
                failed++;
                printf("FAIL %s: %s  %d of %d pictures differ\n", argv[a], line, bad, c.pics);
            }
        }
        fclose(f);
    }
    if (faults) {
        printf("FAIL: %d controller accesses outside the arena\n", faults);
        failed++;
    }
    printf("eprc T33 H.264 vs vendor 2.2.0: %d runs, %d pictures, %d runs differ\n", runs, pics, failed);
    return failed != 0;
}
