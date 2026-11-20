/*
 * T30 NCU engine (src/t30/t30_ncu.c, t30_ncu_par104.c) against the vendor libimp 1.0.4 register
 * trace (#701).  The fixtures were recorded with tools/t30_ncu/ncu_oracle.c running the stock
 * ABUS firmware libimp 1.0.4 (md5 962be4bc) under qemu-mipsel on the synthetic bin of
 * ncu_synth_bin.h (seed 1), mode 1 (Temper by day, NCU at night) and mode 0 (NCU always on).
 * Every register write (address, value, order) must match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "t30_ncu.h"
#include "ncu_synth_bin.h"

static FILE *out;
#define NCU_SEQ_OUT out
#include "../../tools/t30_ncu/ncu_oracle_seq.h"

static uint32_t shadow[0x2000 / 4];

static void fake_wr(uint32_t a, uint32_t v)
{
    fprintf(out, "W %08x %08x\n", a, v);
    if ((a & 0xffffe000u) == 0x13090000u)
        shadow[(a & 0x1fffu) / 4] = v;
}

static uint32_t fake_rd(uint32_t a)
{
    return (a & 0xffffe000u) == 0x13090000u ? shadow[(a & 0x1fffu) / 4] : 0;
}

static int s_hal_init(const char *s, int w, int h) { return t30_ncu_hal_init(s, w, h); }
static int s_buf_size(void) { return (int)t30_ncu_hal_buf_size(); }
static int s_buf_init(uint32_t v, uint32_t p, uint32_t n) { t30_ncu_hal_buf_init(v, p, n); return 0; }
static int s_bypass(int en) { t30_ncu_ctrl_bypass(en); return 0; }
static int s_mode(int m) { t30_ncu_ctrl_update_mode(m); return 0; }
static int s_update(int g) { t30_ncu_ctrl_update(g); return 0; }

static int run_mode(const char *dir, uint32_t mode)
{
    static uint32_t bin[0x1548 / 4];
    const struct ncu_seq_ops ops = { s_hal_init, s_buf_size, s_buf_init, s_bypass, s_mode, s_update };
    char path[256], sensor[16], *got = NULL, *want = NULL;
    size_t got_n = 0, want_n = 0;
    uint32_t probed = 9;
    FILE *f;
    int ok;

    snprintf(sensor, sizeof(sensor), "synth%u", mode);
    snprintf(path, sizeof(path), "%s/ncu_%s.bin", dir, sensor);
    ncu_synth_bin(bin, 1, mode);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(bin, sizeof(bin), 1, f) != 1)
        return 1;
    fclose(f);
    if (t30_ncu_bin_probe(path, &probed) != T30_NCU_BIN_104 || probed != mode) {
        fprintf(stderr, "ncu_test: probe mode %u -> %u\n", mode, probed);
        return 1;
    }
    memset(shadow, 0, sizeof(shadow));
    out = open_memstream(&got, &got_n);
    ncu_oracle_seq(&ops, sensor, 1920, 1080, (int)mode);
    fclose(out);
    unlink(path);

    snprintf(path, sizeof(path), "fixtures/ncu104_synth_mode%u.txt", mode);
    f = fopen(path, "rb");
    if (f == NULL)
        return 1;
    {
        FILE *m = open_memstream(&want, &want_n);
        char line[128];

        while (fgets(line, sizeof(line), f) != NULL)
            fputs(line, m);
        fclose(m);
    }
    fclose(f);
    ok = got_n == want_n && memcmp(got, want, got_n) == 0;
    if (!ok) {
        size_t i, line = 1;

        for (i = 0; i < got_n && i < want_n && got[i] == want[i]; i++)
            line += got[i] == '\n';
        fprintf(stderr, "ncu_test: mode %u differs from the vendor trace at line %zu\n", mode, line);
    }
    free(got);
    free(want);
    return ok ? 0 : 1;
}

int main(void)
{
    char dir[] = "/tmp/ncu_test.XXXXXX";
    int fails = 0;
    uint32_t m;

    if (mkdtemp(dir) == NULL)
        return 1;
    t30_ncu_set_reg_ops(fake_wr, fake_rd);
    t30_ncu_set_bin_dir(dir);
    fails += run_mode(dir, 1);
    fails += run_mode(dir, 0);
    rmdir(dir);
    /* layout probe: wrong sizes are not taken for an NCU bin */
    if (t30_ncu_bin_probe("/nonexistent/ncu_x.bin", &m) != T30_NCU_BIN_NONE)
        fails++;
    printf("ncu_test: %s\n", fails ? "FAIL" : "ok (vendor 1.0.4 trace, modes 1 and 0)");
    return fails ? 1 : 0;
}
