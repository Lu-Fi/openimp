/*
 * T30 NCU vendor oracle (Lu-Fi/openimp #701, #703): runs the NCU engine of a vendor T30 libimp
 * (1.0.4 stock ABUS firmware, md5 962be4bc, or 1.0.5 T30X library, md5 ad636e05) under qemu-mipsel and prints every NCU
 * register write.  write_reg_32 / read_reg_32 are defined here and preempt the library's
 * own (it reaches them through the GOT), so no /dev/mem is touched.  The vendor library
 * is not part of this repository.
 *
 * Build (thingino mipsel toolchain, vendor root with lib/ and usr/lib/libimp.so):
 *   mipsel-linux-gcc -O1 -o ROOT/ncu_oracle ncu_oracle.c -Wl,--export-dynamic \
 *     -Wl,--dynamic-linker=/lib/ld-uClibc.so.0 -LROOT/usr/lib -LROOT/lib \
 *     -Wl,-rpath-link=ROOT/lib -limp -lstdc++ -Wl,--allow-shlib-undefined
 * Run:  qemu-mipsel -L ROOT ROOT/ncu_oracle <sensor> 1920 1080 1 > trace.txt
 *   (reads ROOT/etc/sensor/ncu_<sensor>.bin; ncu_synth_gen writes the test bin)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ncu_oracle_seq.h"

static uint32_t shadow[0x2000 / 4];

int write_reg_32(uint32_t a, uint32_t v)
{
    printf("W %08x %08x\n", a, v);
    if ((a & 0xffffe000u) == 0x13090000u)
        shadow[(a & 0x1fffu) / 4] = v;
    return 0;
}

int read_reg_32(uint32_t a, uint32_t *v)
{
    *v = (a & 0xffffe000u) == 0x13090000u ? shadow[(a & 0x1fffu) / 4] : 0;
    return 0;
}

int IMP_Log_Get_Option(void) { return 0; }
int isp_get_total_gain(void) { return 0; }   /* 1.0.5 ncu_ctrl_update_param reads it; 1.0.4 does not */
void imp_log_fun(int level, int opt, int cnt, ...) { (void)level; (void)opt; (void)cnt; }

extern int ncu_hal_init(const char *sensor, int w, int h);
extern int ncu_hal_buf_size(void);
extern int ncu_hal_buf_init(uint32_t virt, uint32_t phys, uint32_t size);
extern int ncu_ctrl_bypass(int en);
extern int ncu_ctrl_update_mode(int mode);
extern int ncu_func_update(int gain);

int main(int argc, char **argv)
{
    const char *sensor = argc > 1 ? argv[1] : "synth";
    int w = argc > 2 ? atoi(argv[2]) : 1920;
    int h = argc > 3 ? atoi(argv[3]) : 1080;
    int mode = argc > 4 ? atoi(argv[4]) : 1;
    struct ncu_seq_ops ops = { ncu_hal_init, ncu_hal_buf_size, ncu_hal_buf_init, ncu_ctrl_bypass,
                               ncu_ctrl_update_mode, ncu_func_update };

    setvbuf(stdout, NULL, _IOLBF, 0);
    return ncu_oracle_seq(&ops, sensor, w, h, mode);
}
