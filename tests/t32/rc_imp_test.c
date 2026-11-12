/*
 * T32 IMP -> i264e rate-control parameters (#295) host test: OpenIMP's
 * IMP_Encoder_SetDefaultParam rate-control part (t32_rc_default_attr) and
 * attribute -> i264e parameter mapping (t32_rc_init, t32_rc_reconfig) against vendor
 * 2.2.0 (tools/eprc_t33_imp_oracle.py: SetDefaultParam, then
 * channel_i264e_encoder_init + i264e_validate_parameters):
 *
 *   rc_imp_test [-v] FILE ...
 *
 * 'D' lines: the attribute SetDefaultParam fills; 'A' + 'P' lines: a random
 * attribute and the parameter block the vendor makes of it (through
 * IMP_Encoder_CreateChn).  Every i264e word the T33 controller reads
 * (t32_rc33_param_words) must be equal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc.h"

#define PSIZE 0x360

static int verbose;

static int unhex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;

        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return s[2 * n] == '\0' || s[2 * n] == '\n' ? 0 : -1;
}

static uint32_t w32(const uint8_t *p, uint32_t o)
{
    return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24;
}

static unsigned bad_off[PSIZE / 4];

/* one 'D' line: profile mode initqp kbps ret attr */
static int check_default(char *line, int *bad)
{
    unsigned prof, mode, ret;
    int iqp, kbps, n = 0;
    uint8_t want[0xb8], gotb[0xb8];
    IMPEncoderAttrRcMode got;

    if (sscanf(line, "D %x %u %d %d %x %n", &prof, &mode, &iqp, &kbps, &ret, &n) != 5 || !n ||
        unhex(line + n, want, sizeof(want)) != 0)
        return -1;
    memset(&got, 0xee, sizeof(got));
    got.rcMode = (IMPEncoderRcMode)mode;
    t32_rc_default_attr(&got, (IMPPayloadType)(prof >> 24), iqp, (uint32_t)kbps);
    memcpy(gotb + 0x3c, &got, sizeof(got));
    if (memcmp(gotb + 0x3c, want + 0x3c, sizeof(got)) != 0) {
        (*bad)++;
        if (verbose) {
            printf("D %08x mode %u iqp %d kbps %d\n  vendor ", prof, mode, iqp, kbps);
            for (unsigned i = 0x3c; i < 0x3c + sizeof(IMPEncoderAttrRcMode); i++)
                printf("%02x", want[i]);
            printf("\n  ours   ");
            for (unsigned i = 0x3c; i < 0x3c + sizeof(IMPEncoderAttrRcMode); i++)
                printf("%02x", gotb[i]);
            printf("\n");
        }
    }
    return 0;
}

/* one 'A' + 'P' pair, or with @rline an 'A' + 'R' + 'Q' triple (the 'R'
 * rate control set at run time on the 'A' channel) */
static int check_param(const char *aline, const char *rline, char *pline, int *bad, int *skipped)
{
    unsigned seed, seed2, ret, vret = 0;
    int n = 0;
    uint8_t at[0xb8], want[PSIZE];
    IMPEncoderAttrRcMode m;
    T32Rc rc;
    const uint16_t *words;
    unsigned nw, diff = 0, pw, ph;

    if (sscanf(aline, "A %u %n", &seed, &n) != 1 || !n || unhex(aline + n, at, sizeof(at)) != 0)
        return -1;
    n = 0;
    if (rline) {
        if (sscanf(pline, "Q %u %x %n", &seed2, &ret, &n) != 2 || !n || seed2 != seed)
            return -1;
    } else if (sscanf(pline, "P %u %x %x %n", &seed2, &ret, &vret, &n) != 3 || !n || seed2 != seed) {
        return -1;
    }
    if (ret != 0 || vret != 0 || pline[n] == '-') {
        (*skipped)++;           /* the vendor channel fails to start */
        return 0;
    }
    if (unhex(pline + n, want, sizeof(want)) != 0)
        return -1;
    /* IMPEncoderChnAttr (vendor 2.x, 184 bytes): picWidth/picHeight +0xc/+0x10,
     * outFrmRate +0x30, maxGop +0x38, attrRcMode +0x3c (64 bytes, no pointers) */
    memcpy(&m, at + 0x3c, sizeof(m));
    pw = w32(at, 0xc) & 0xffffu;
    ph = w32(at, 0x10) & 0xffffu;
    /* the channel as the encoder sets it up (rc_setup) */
    memset(&rc, 0, sizeof(rc));
    if (t32_rc_init(&rc, &m, pw, ph, w32(at, 4), w32(at, 0x38), w32(at, 0x30), w32(at, 0x34), 0) != 0)
        return -1;
    if (rline) {
        unsigned s3;
        int k = 0;

        if (sscanf(rline, "R %u %n", &s3, &k) != 1 || !k || s3 != seed || unhex(rline + k, (uint8_t *)&m, sizeof(m)))
            return -1;
        /* SetChnAttrRcMode, taken at the next IDR */
        if (t32_rc_reconfig(&rc, &m, pw, ph, w32(at, 4), w32(at, 0x38), w32(at, 0x30), w32(at, 0x34), 0) != 0)
            return -1;
    }
    if (!rc.t33) {
        t32_rc_free(&rc);
        (*bad)++;
        if (verbose)
            printf("seed %u mode %d: no controller\n", seed, (int)m.rcMode);
        return 0;
    }
    nw = t32_rc33_param_words(&words);
    for (unsigned i = 0; i < nw; i++) {
        uint32_t o = words[i], v = w32(want, o), g = t32_rc33_get(rc.t33, o);

        if (v != g) {
            if (!diff && verbose)
                printf("%sseed %u mode %d:", rline ? "run time " : "", seed, (int)m.rcMode);
            if (verbose)
                printf(" %03x v %d o %d", o, (int)v, (int)g);
            bad_off[o / 4]++;
            diff++;
        }
    }
    if (diff) {
        (*bad)++;
        if (verbose)
            printf("\n");
    }
    t32_rc_free(&rc);
    return 0;
}

int main(int argc, char **argv)
{
    int dbad = 0, dn = 0, pbad = 0, pn = 0, skipped = 0, qbad = 0, qn = 0, qskipped = 0, fail = 0;
    static char line[4096], aline[4096], rline[4096];

    for (int i = 1; i < argc; i++) {
        FILE *f;

        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
            continue;
        }
        f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        aline[0] = '\0';
        while (fgets(line, sizeof(line), f)) {
            int r = 0;

            if (line[0] == 'D') {
                r = check_default(line, &dbad);
                dn++;
            } else if (line[0] == 'A') {
                strcpy(aline, line);
            } else if (line[0] == 'P') {
                r = check_param(aline, NULL, line, &pbad, &skipped);
                pn++;
            } else if (line[0] == 'R') {
                strcpy(rline, line);
            } else if (line[0] == 'Q') {
                r = check_param(aline, rline, line, &qbad, &qskipped);
                qn++;
            }
            if (r) {
                fprintf(stderr, "%s: bad line: %.60s\n", argv[i], line);
                fclose(f);
                return 2;
            }
        }
        fclose(f);
    }
    for (unsigned o = 0; o < PSIZE / 4; o++)
        if (bad_off[o])
            printf("  i264e +0x%03x: %u differences\n", o * 4, bad_off[o]);
    printf("SetDefaultParam rate-control attributes: %d of %d vendor-equal\n", dn - dbad, dn);
    printf("attribute -> i264e parameters: %d of %d vendor-equal (%d the vendor refuses)\n",
           pn - skipped - pbad, pn - skipped, skipped);
    if (qn)
        printf("SetChnAttrRcMode -> i264e parameters at the next IDR: %d of %d vendor-equal (%d refused)\n",
               qn - qskipped - qbad, qn - qskipped, qskipped);
    fail = dbad || pbad || qbad;
    return fail;
}
