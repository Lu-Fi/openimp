/* src/eprc/eprc_t30.c against the OEM T30 rate controller: the vectors were
 * produced by tools/eprc_t30_oracle.py, which runs the OEM T30 1.0.5 code
 * (the uclibc 4.7.2 build the T30X ships) on the same synthetic picture
 * sizes and statistics as below.  An S line with a 19th field 1 uses the
 * "real" statistics (counts in the ranges of a real picture and a non-zero
 * channel cmpx).  H / G lines are H.265 (tools/eprc_t30_oracle.py --h265):
 * per picture the QPs and the CRC-32 of the parameter block and the CU QP
 * map.  An R line (--h265-reconf) changes the rate-control fields of the
 * running H.265 controller before the next picture (EPRC30_Reinit); in an
 * H.264 scenario (--h264-reconf, #366) it is the change that the OEM
 * i264e_idr_reconfig applies at the next (IDR) picture: that picture starts
 * on the old controller, which is then re-initialised from its QP clipped
 * to the new range and starts the picture again. */
#include "eprc/eprc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t xorshift(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void statistics(uint32_t frame, uint32_t regs[EPRC30_STAT_REGS])
{
    uint32_t s = 0x9e3779b9u ^ (frame * 0x85ebca6bu);
    int i;

    for (i = 0; i < EPRC30_STAT_REGS; i++) {
        s = xorshift(s);
        regs[i] = s >> (s & 15u);
    }
}

static uint32_t real_statistics(uint32_t frame, uint32_t w, uint32_t h,
                                uint32_t regs[EPRC30_STAT_REGS])
{
    uint32_t mbs = ((w + 15u) / 16u) * ((h + 15u) / 16u);
    uint32_t k = (frame / 20u) % 5u, m;
    int j;

    statistics(frame, regs);
    m = mbs / (1u + k);
    regs[0] = ((regs[0] % (m ? m : 1u)) & 0x7fffu) |
              (((regs[4] % (mbs / 3u ? mbs / 3u : 1u)) & 0x7fffu) << 16);
    regs[1] = (regs[1] % (mbs * (1u + 20u * k))) & 0x7ffffffu;
    regs[2] = (regs[2] % (mbs * (1u + 10u * k))) & 0x7ffffffu;
    for (j = 10; j <= 12; j++)
        regs[j] = regs[j] % (mbs ? mbs : 1u);
    return 1u + (regs[3] % 40000u) * (1u + (frame / 30u) % 3u);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t picture_bytes(uint32_t frame, uint32_t qp, int idr,
                              uint64_t pixels, int pattern)
{
    uint64_t cplx = pattern ? ((frame / 40u) % 2u ? 5u : 1u) : 1u + frame % 7u;
    uint64_t size = pixels * cplx / 40u;
    uint32_t i;

    for (i = 0; i < qp; i++)
        size = size * 89u / 100u;
    if (idr)
        size *= 4u;
    if (frame % 37u == 36u)
        size *= 120u;
    return size < 20u ? 20u : (uint32_t)size;
}

/* The 'W' line fields (#217 step 2, tools/eprc_t30_oracle.py --regs):
 * 0x40074, the 13 macroblock rate-control registers, 0x4006c, basic unit
 * on, target count, its five registers, CRC-32 of the targets (zero
 * targets when this try computed none, eprc.h) */
#define W_WORDS 23
static void mbrc_words(const uint8_t *b, int p_slice, const int32_t *tgt,
                       uint32_t got[W_WORDS])
{
    static const int32_t zero[128];
    Eprc30MbRc m;
    int i;

    EPRC30_MbRcRegs(b, p_slice, &m);
    got[0] = m.qp_flags | (uint32_t)b[0x184] << 8 | (uint32_t)b[0x186] << 16 |
             (uint32_t)b[0x185] << 24;
    memcpy(got + 1, m.reg, sizeof(m.reg));
    got[14] = m.qpmap;
    got[15] = (uint32_t)m.bu_on;
    got[16] = m.bu_on ? m.bu_n : 0u;
    for (i = 0; i < 5; i++)
        got[17 + i] = m.bu_on ? m.bu[i] : 0u;
    got[22] = crc32((const uint8_t *)(m.bu_fresh ? tgt : zero),
                    m.bu_on ? 4u * m.bu_n : 0u);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "eprc_t30_vectors.txt";
    FILE *f = fopen(path, "r");
    char line[512];
    Eprc30 rc;
    static uint8_t slice[0x1204];
    static uint8_t qpmap[0x10000];
    EprcParams p, p264;
    int scenario = -1, frame = 0, active = 0, real = 0, reconf264 = 0;
    uint32_t ybytes = 0;   /* 'Y': coded size of the next F picture (--replay) */
    /* 'W' lines (#217 step 2): the register words of the slice block of
     * the last picture (last_p: a P picture), or of the last 'Z' block */
    static uint8_t zslice[0x1204];
    static int32_t ztgt[128];
    static uint32_t wq[16][W_WORDS];   /* per coding try of the last picture */
    int wn = 0, wk = 0, zp = 0, zmode = 0;
    long checked = 0, failed = 0;

    if (!f) {
        perror(path);
        return 2;
    }
    memset(&rc, 0, sizeof(rc));
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'S') {
            int v[19] = {0}, nv;

            nv = sscanf(line, "S %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
                        &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16],
                        &v[17], &v[18]);
            if (nv != 18 && nv != 19)
                return 2;
            if (active)
                EPRC30_Free(&rc);
            memset(&p, 0, sizeof(p));
            p.rc_mode = (uint32_t)v[0];
            p.width = (uint32_t)v[1];
            p.height = (uint32_t)v[2];
            p.gop = (uint32_t)v[3];
            p.fps_num = (uint32_t)v[4];
            p.fps_den = 1;
            p.min_qp = (uint32_t)v[5];
            p.max_qp = (uint32_t)v[6];
            p.bitrate = (uint32_t)v[7];
            p.max_bitrate = (uint32_t)v[8];
            p.frm_qp_step = (uint32_t)v[9];
            p.gop_qp_step = (uint32_t)v[10];
            p.i_bias = v[11];
            p.static_time = (uint32_t)v[12];
            p.change_pos = (uint32_t)v[13];
            p.quality = (uint32_t)v[14];
            p.init_qp = v[16];
            real = nv == 19 && v[18];
            memset(slice, 0, sizeof(slice));
            if (EPRC30_Init(&rc, &p, slice) != 0)
                return 2;
            /* the oracle's macroblock input block (i264e handle +0xba4 ->
             * E+140, copied into the slice parameters at +0x18c) */
            EPRC30_SetQpMap(&rc, qpmap, 0x50040000u);
            active = 1;
            scenario++;
            frame = 0;
        } else if (line[0] == 'R' && active && !p.codec) {
            /* R (--h264-reconf, #366): the rate-control fields of a run-time
             * change; the next F picture is the IDR where the OEM
             * i264e_idr_reconfig applies it */
            int v[18], i;
            char *t = line + 1;

            for (i = 0; i < 18; i++)
                v[i] = (int)strtol(t, &t, 10);
            memset(&p264, 0, sizeof(p264));
            p264.rc_mode = (uint32_t)v[0];
            p264.width = p.width;
            p264.height = p.height;
            p264.gop = (uint32_t)v[3];
            p264.fps_num = (uint32_t)v[4];
            p264.fps_den = 1;
            p264.min_qp = (uint32_t)v[5];
            p264.max_qp = (uint32_t)v[6];
            p264.bitrate = (uint32_t)v[7];
            p264.max_bitrate = (uint32_t)v[8];
            p264.frm_qp_step = (uint32_t)v[9];
            p264.gop_qp_step = (uint32_t)v[10];
            p264.i_bias = v[11];
            p264.static_time = (uint32_t)v[12];
            p264.change_pos = (uint32_t)v[13];
            p264.quality = (uint32_t)v[14];
            reconf264 = 1;
        } else if (line[0] == 'H' || (line[0] == 'R' && active)) {
            int v[16], i, reinit = line[0] == 'R';
            char *t = line + 1;

            for (i = 0; i < 16; i++)
                v[i] = (int)strtol(t, &t, 10);
            if (active && !reinit)
                EPRC30_Free(&rc);
            memset(&p, 0, sizeof(p));
            p.codec = 1;
            p.rc_mode = (uint32_t)v[0];
            p.width = (uint32_t)v[1];
            p.height = (uint32_t)v[2];
            p.gop = (uint32_t)v[3];
            p.fps_num = (uint32_t)v[4];
            p.fps_den = 1;
            p.min_qp = (uint32_t)v[5];
            p.max_qp = (uint32_t)v[6];
            p.bitrate = p.max_bitrate = (uint32_t)v[7];
            p.frm_qp_step = (uint32_t)v[8];
            p.gop_qp_step = (uint32_t)v[9];
            p.i_bias = v[10];
            p.static_time = (uint32_t)v[11];
            p.change_pos = (uint32_t)v[12];
            p.quality = (uint32_t)v[13];
            p.init_qp = p.cqp = v[14];
            p.fluct_lvl = (uint32_t)v[15];
            p.ip_ratio = 1.399999976158142;
            if (reinit) {
                /* R: a run-time change at this IDR (I265e::reset) */
                if (EPRC30_Reinit(&rc, &p) != 0)
                    return 2;
                continue;
            }
            memset(slice, 0, sizeof(slice));
            if (EPRC30_Init(&rc, &p, slice) != 0 || EPRC30_QpMapSize(&rc) > sizeof(qpmap))
                return 2;
            EPRC30_SetQpMap(&rc, qpmap, 0x10000000u);
            active = 1;
            scenario++;
            frame = 0;
        } else if (line[0] == 'G' && active) {
            int want[16], n = 0, i, idr;
            unsigned long crc_b, crc_m;
            char *t = line + 1;
            EprcPicture pic;
            uint32_t regs[EPRC30_STAT_REGS], bytes;

            idr = (int)strtol(t, &t, 10);
            crc_b = strtoul(t, &t, 16);
            crc_m = strtoul(t, &t, 16);
            while (n < 16) {
                char *e;
                long v = strtol(t, &e, 10);
                if (e == t)
                    break;
                want[n++] = (int)v;
                t = e;
            }
            EPRC30_FrameStart(&rc, idr, &pic);
            statistics((uint32_t)frame, regs);
            bytes = picture_bytes((uint32_t)frame, pic.qp, idr,
                                  (uint64_t)p.width * p.height, scenario % 2);
            for (i = 0;; i++) {
                checked++;
                if (i >= n || pic.qp != want[i]) {
                    if (failed++ < 10)
                        fprintf(stderr, "%s: scenario %d picture %d step %d: QP %d, OEM %d\n",
                                path, scenario, frame, i, pic.qp, i < n ? want[i] : -1);
                    break;
                }
                if (EPRC30_FrameEnd(&rc, bytes, regs, 0, &pic, 1) != 1)
                    break;
                bytes = bytes / 2u < 20u ? 20u : bytes / 2u;
            }
            if (i + 1 < n && failed++ < 10)
                fprintf(stderr, "%s: scenario %d picture %d: %d re-encodes, OEM %d\n",
                        path, scenario, frame, i, n - 1);
            memset(slice + 0xb4, 0, 4);
            checked++;
            if ((crc32(slice, sizeof(slice)) != crc_b ||
                 crc32(qpmap, EPRC30_QpMapSize(&rc)) != crc_m) && failed++ < 10)
                fprintf(stderr, "%s: scenario %d picture %d: parameter block / QP map differ\n",
                        path, scenario, frame);
            frame++;
        } else if (line[0] == 'Z') {
            /* --regs-fuzz: a random slice block, +0x184..+0x1f3 */
            char *t = line + 1;
            uint32_t i, salt;

            zp = (int)strtol(t, &t, 10);
            while (*t == ' ')
                t++;
            memset(zslice, 0, sizeof(zslice));
            for (i = 0x184; i < 500u && t[0] && t[1]; i++, t += 2) {
                char h[3] = {t[0], t[1], 0};
                zslice[i] = (uint8_t)strtoul(h, NULL, 16);
            }
            salt = (uint32_t)zslice[0x1cc] | (uint32_t)zslice[0x1cd] << 8 |
                   (uint32_t)zslice[0x1ce] << 16 | (uint32_t)zslice[0x1cf] << 24;
            for (i = 0; i < 128u; i++)      /* fuzz_target() of the oracle */
                ztgt[i] = (int32_t)(i * 0x9e3779b1u + salt);
            zslice[0x1e4] = 1;              /* the oracle passes the targets */
            zslice[0x1e5] = zslice[0x1e6] = zslice[0x1e7] = 0;
            mbrc_words(zslice, zp, ztgt, wq[0]);
            wn = 1;
            wk = 0;
            zmode = 1;
        } else if (line[0] == 'W' && (active || zmode)) {
            unsigned long want[W_WORDS];
            char *t = line + 1;
            int i;

            for (i = 0; i < W_WORDS; i++)
                want[i] = strtoul(t, &t, i == 15 || i == 16 ? 10 : 16);
            checked++;
            if (wk >= wn) {
                if (failed++ < 10)
                    fprintf(stderr, "%s: scenario %d picture %d: extra W line\n",
                            path, scenario, frame - 1);
                continue;
            }
            for (i = 0; i < W_WORDS; i++)
                if (wq[wk][i] != (uint32_t)want[i])
                    break;
            if (i < W_WORDS && failed++ < 10)
                fprintf(stderr, "%s: scenario %d picture %d try %d: SliceInit word %d "
                        "%08x, OEM %08lx\n", path, scenario, frame - 1, wk, i,
                        wq[wk][i], want[i]);
            wk++;
            zmode = 0;
        } else if (line[0] == 'B' && active) {
            /* the slice-parameter block after the picture (h264_get_mb_qp
             * output); +0x1e4 holds an OEM stack address */
            char *t = line + 1;
            unsigned long want = strtoul(t, &t, 16), want_t = strtoul(t, NULL, 16);
            const char *dump = getenv("EPRC30_DUMP");
            int32_t nt = (int32_t)(slice[0x1d8] | slice[0x1d9] << 8 | slice[0x1da] << 16 |
                                   (uint32_t)slice[0x1db] << 24);
            uint32_t tgt_crc;
            int have_tgt = slice[0x1e4] != 0;

            memset(slice + 0x1e4, 0, 4);
            if (dump) {
                char fn[512];
                FILE *o;

                snprintf(fn, sizeof(fn), "%s/%d_%d.bin", dump, scenario, frame - 1);
                o = fopen(fn, "wb");
                if (o) {
                    fwrite(slice, 1, sizeof(slice), o);
                    fclose(o);
                }
            }
            /* the targets exist when the basic unit runs (+0x1c7) */
            tgt_crc = crc32((const uint8_t *)rc.bu_tgt,
                            have_tgt && nt > 0 ? 4u * (uint32_t)(nt < 128 ? nt : 128) : 0u);
            checked++;
            if ((crc32(slice, sizeof(slice)) != want || tgt_crc != want_t) && failed++ < 10)
                fprintf(stderr, "%s: scenario %d picture %d: slice parameters differ\n",
                        path, scenario, frame - 1);
        } else if (line[0] == 'Y' && active) {
            ybytes = (uint32_t)strtoul(line + 1, NULL, 10);
        } else if (line[0] == 'F' && active) {
            int want[16], n = 0, since, i, idr;
            char *s = line + 1;
            EprcPicture pic;
            uint32_t regs[EPRC30_STAT_REGS], bytes, cmpx = 0;

            since = (int)strtol(s, &s, 10);
            while (n < 16) {
                char *e;
                long v = strtol(s, &e, 10);
                if (e == s)
                    break;
                want[n++] = (int)v;
                s = e;
            }
            if (reconf264) {
                /* i264e_idr_reconfig: the old controller starts the IDR,
                 * ratecontrol_init from its QP clipped to the new range,
                 * ratecontrol_start again */
                int32_t q;

                EPRC30_FrameStart(&rc, 1, &pic);
                q = pic.qp;
                q = q < (int32_t)p264.min_qp ? (int32_t)p264.min_qp :
                    q > (int32_t)p264.max_qp ? (int32_t)p264.max_qp : q;
                p264.init_qp = q;
                p = p264;
                reconf264 = 0;
                if (EPRC30_Reinit(&rc, &p) != 0)
                    return 2;
            }
            idr = since % (int)p.gop == 0;
            EPRC30_FrameStart(&rc, idr, &pic);
            mbrc_words(slice, !idr, rc.bu_tgt, wq[0]);
            wn = 1;
            wk = 0;
            if (real)
                cmpx = real_statistics((uint32_t)frame, p.width, p.height, regs);
            else
                statistics((uint32_t)frame, regs);
            bytes = picture_bytes((uint32_t)frame, pic.qp, idr,
                                  (uint64_t)p.width * p.height, scenario % 2);
            if (ybytes) {
                bytes = ybytes;
                ybytes = 0;
            }
            for (i = 0;; i++) {
                checked++;
                if (i >= n || pic.qp != want[i]) {
                    if (failed++ < 10)
                        fprintf(stderr, "%s: scenario %d picture %d step %d: QP %d, OEM %d\n",
                                path, scenario, frame, i, pic.qp, i < n ? want[i] : -1);
                    break;
                }
                memset(slice + 0x1e4, 0, 4);  /* as the oracle: set by a basic-unit run */
                if (EPRC30_FrameEnd(&rc, bytes, regs, cmpx, &pic, 1) != 1)
                    break;
                if (wn < 16)
                    mbrc_words(slice, !idr, rc.bu_tgt, wq[wn++]);
                bytes = bytes / 2u < 20u ? 20u : bytes / 2u;
            }
            if (i + 1 < n && failed++ < 10)
                fprintf(stderr, "%s: scenario %d picture %d: %d re-encodes, OEM %d\n",
                        path, scenario, frame, i, n - 1);
            frame++;
        }
    }
    if (active)
        EPRC30_Free(&rc);
    fclose(f);
    printf("%s: %d scenarios, %ld decisions, %ld differ\n", path, scenario + 1, checked, failed);
    return failed ? 1 : 0;
}
