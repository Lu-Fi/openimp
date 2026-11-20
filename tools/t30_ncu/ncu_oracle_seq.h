/* The call sequence of the T30 NCU oracle (#701), shared by tools/t30_ncu/ncu_oracle.c (vendor
 * library) and tests/t30/ncu_test.c (OpenIMP port): init as the vendor 1.0.4 AddSensor does for
 * the bin's 3D mode, then night (bypass off, night set) and day (bypass on, day set), each with
 * the 1 s gain update over NCU_SEQ_GAINS. */
#ifndef NCU_ORACLE_SEQ_H
#define NCU_ORACLE_SEQ_H
#include <stdint.h>
#include <stdio.h>

#ifndef NCU_SEQ_OUT
#define NCU_SEQ_OUT stdout
#endif

struct ncu_seq_ops {
    int (*hal_init)(const char *sensor, int w, int h);
    int (*hal_buf_size)(void);
    int (*hal_buf_init)(uint32_t virt, uint32_t phys, uint32_t size);
    int (*bypass)(int en);
    int (*update_mode)(int mode);
    int (*update)(int gain);
};

static const int ncu_seq_gains[] = { 0, 256, 583, 1327, 2300, 4200, 8100, 16000, 47200, 200000 };

static int ncu_oracle_seq(const struct ncu_seq_ops *o, const char *sensor, int w, int h, int mode)
{
    uint32_t size;
    unsigned i;

    fprintf(NCU_SEQ_OUT, "# hal_init %s %dx%d mode %d\n", sensor, w, h, mode);
    if (o->hal_init(sensor, w, h) != 0)
        return 1;
    size = mode == 0 ? (uint32_t)o->hal_buf_size() : (uint32_t)(w * h * 4);
    fprintf(NCU_SEQ_OUT, "# hal_buf_init %u\n", size);
    o->hal_buf_init(0x40000000u, 0x02000000u, size);
    fprintf(NCU_SEQ_OUT, "# bypass %d\n", mode == 1);
    o->bypass(mode == 1);
    fprintf(NCU_SEQ_OUT, "# night\n");
    o->bypass(0);
    o->update_mode(1);
    for (i = 0; i < sizeof(ncu_seq_gains) / sizeof(ncu_seq_gains[0]); i++) {
        fprintf(NCU_SEQ_OUT, "# night gain %d\n", ncu_seq_gains[i]);
        o->update(ncu_seq_gains[i]);
    }
    fprintf(NCU_SEQ_OUT, "# day\n");
    o->bypass(1);
    o->update_mode(0);
    for (i = 0; i < sizeof(ncu_seq_gains) / sizeof(ncu_seq_gains[0]); i++) {
        fprintf(NCU_SEQ_OUT, "# day gain %d\n", ncu_seq_gains[i]);
        o->update(ncu_seq_gains[i]);
    }
    fprintf(NCU_SEQ_OUT, "# end\n");
    return 0;
}
#endif
