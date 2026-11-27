/*
 * Host side of the T30 NCU oracle comparison (Lu-Fi/openimp #701, #703): runs the OpenIMP NCU
 * engine through the same call sequence as tools/t30_ncu/ncu_oracle.c and prints every register
 * write in the same format, for diffing against the vendor trace on arbitrary bins (fuzzing).
 * Build: cc -I src/t30 -o ncu_host_trace tools/t30_ncu/ncu_host_trace.c src/t30/t30_ncu.c \
 *            src/t30/t30_ncu_par104.c src/t30/t30_ncu_par105.c -lpthread
 * Run:   ncu_host_trace <dir with ncu_<sensor>.bin> <sensor> <w> <h> <mode>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t30_ncu.h"
#include "ncu_oracle_seq.h"

static uint32_t shadow[0x2000 / 4];

static void fake_wr(uint32_t a, uint32_t v)
{
    printf("W %08x %08x\n", a, v);
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

int main(int argc, char **argv)
{
    const struct ncu_seq_ops ops = { s_hal_init, s_buf_size, s_buf_init, s_bypass, s_mode, s_update };

    if (argc < 6)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    t30_ncu_set_reg_ops(fake_wr, fake_rd);
    t30_ncu_set_bin_dir(argv[1]);
    return ncu_oracle_seq(&ops, argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
}
