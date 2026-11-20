/*
 * T33 software OSD (#675): src/t32/openimp_t33_osd_sw.c against vendor
 * libimp 2.0.2.1 (tools/t33_osd_oracle.py vectors): every case is drawn with
 * the same frame, region and group attribute, the return code and the
 * FNV-1a hash of frame + attributes must equal the vendor's.
 *
 *   osd_sw_test VECTORS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t33_osd_sw.h"

static uint32_t xs(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void fill(uint8_t *p, uint32_t seed, size_t n, unsigned zero_share)
{
    uint32_t s = seed ? seed : 1u;

    for (size_t i = 0; i < n; i++) {
        s = xs(s);
        p[i] = (zero_share && ((s >> 8) & 0xffu) < zero_share) ? 0 : (uint8_t)s;
    }
}

static uint32_t fnv(uint32_t h, const void *d, size_t n)
{
    const uint8_t *b = d;

    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x01000193u;
    return h;
}

/* the vendor attribute bytes the oracle hashes (0x20 of the region: no data pointer, 0x24 of the group) */
static uint32_t hash_attrs(uint32_t h, const T33OsdRgn *r, const T33OsdGrp *g)
{
    int32_t rw[8] = { r->type, r->x0, r->y0, r->x1, r->y1, r->line[0], r->line[1], r->fmt };

    h = fnv(h, rw, sizeof(rw));
    return fnv(h, g, sizeof(*g));
}

int main(int argc, char **argv)
{
    static uint8_t frame[1 << 20], pic[0x40000];
    char line[512], kind;
    unsigned n = 0, bad = 0, drawn = 0;
    FILE *f;

    if (argc != 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s VECTORS\n", argv[0]);
        return 2;
    }
    _Static_assert(sizeof(T33OsdGrp) == 0x24, "group attribute view");
    while (fgets(line, sizeof(line), f)) {
        int w, h, x0, y0, x1, y1, fmt, offx, offy, aen, fa, rc;
        unsigned sx, sy, colour, fseed, dseed, vrc, vh;
        T33OsdFrame fr;
        T33OsdRgn r;
        T33OsdGrp g;
        size_t size;
        uint32_t hh;

        if (line[0] == '#')
            continue;
        if (sscanf(line, "%c %d %d %d %d %d %d %d %d %d %x %x %d %d %x %x %x %x %x", &kind, &w, &h, &x0, &y0, &x1,
                   &y1, &fmt, &offx, &offy, &sx, &sy, &aen, &fa, &colour, &fseed, &dseed, &vrc, &vh) != 19) {
            fprintf(stderr, "bad line: %s", line);
            return 2;
        }
        size = (size_t)((w + 15) & ~15) * (size_t)((h + 15) & ~15) * 3u / 2u;
        fill(frame, fseed, size, 0);
        fill(pic, dseed, sizeof(pic), kind == 'B' ? 64u : 0u);
        fr.width = w;
        fr.height = h;
        fr.virt = frame;
        memset(&r, 0, sizeof(r));
        r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1; r.fmt = fmt;
        r.data = pic;
        memset(&g, 0, sizeof(g));
        g.show = 1; g.offx = offx; g.offy = offy;
        memcpy(&g.scalex, &sx, 4);
        memcpy(&g.scaley, &sy, 4);
        g.alpha_en = aen; g.fg_alpha = fa;
        hh = fnv(0x811c9dc5u, frame, size);
        if (kind == 'P')
            rc = t33_osd_draw_pic(&fr, &r, &g);
        else if (kind == 'C')
            rc = (t33_osd_draw_cover(&fr, &r, &g, colour), 0);
        else
            rc = (t33_osd_draw_bitmap(&fr, &r, &g), 0);
        drawn += fnv(0x811c9dc5u, frame, size) != hh;
        hh = fnv(0x811c9dc5u, frame, size);
        /* the oracle's region word 8 is the data pointer or the cover colour: hash it as the vendor saw it */
        hh = hash_attrs(hh, &r, &g);
        n++;
        if ((unsigned)rc != vrc || hh != vh) {
            if (bad++ < 10)
                fprintf(stderr, "MISMATCH %s  -> rc %08x hash %08x\n", strtok(line, "\n"), (unsigned)rc, hh);
        }
    }
    fclose(f);
    printf("t33 osd sw: %u cases (%u changed the frame), %u mismatches\n", n, drawn, bad);
    return bad ? 1 : 0;
}
