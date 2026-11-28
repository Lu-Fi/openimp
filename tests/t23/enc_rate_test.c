/* #986: T23 encoder rate = min(sensor, output), GOP kept in whole seconds
 * (vendor T23 1.3.0 do_channel_process 0x4a248..0x4a300, 0x4b234, 0x4b508). */
#include <assert.h>
#include <stdio.h>

#include "t23/t23_enc_rate.h"

static void expect(T23EncRate *r, uint32_t sn, uint32_t sd, uint32_t on,
                   uint32_t od, int changed, uint32_t num, uint32_t den,
                   uint32_t gop)
{
    int c = t23_enc_rate_step(r, sn, sd, on, od);

    if (c != changed || r->num != num || r->den != den || r->gop != gop) {
        fprintf(stderr, "src %u/%u out %u/%u: got %d %u/%u gop %u, want "
                "%d %u/%u gop %u\n", sn, sd, on, od, c, r->num, r->den,
                r->gop, changed, num, den, gop);
        assert(0);
    }
}

int main(void)
{
    T23EncRate r;

    /* light box: 25 fps channel on the 15 fps sensor */
    t23_enc_rate_init(&r, 25, 1, 25);
    expect(&r, 15, 1, 25, 1, 1, 15, 1, 15);
    expect(&r, 15, 1, 25, 1, 0, 15, 1, 15);     /* steady */
    t23_enc_rate_init(&r, 25, 1, 50);
    expect(&r, 15, 1, 25, 1, 1, 15, 1, 30);
    /* sensor back to 25: output rate, GOP from 2 s */
    expect(&r, 25, 1, 25, 1, 1, 25, 1, 50);
    expect(&r, 25, 1, 25, 1, 0, 25, 1, 50);
    /* sensor at the output rate: the first picture rounds the GOP down to
     * whole seconds like the vendor (30 at 25 fps -> 25) */
    t23_enc_rate_init(&r, 25, 1, 30);
    expect(&r, 25, 1, 25, 1, 1, 25, 1, 25);
    /* output below the sensor rate: encoder keeps the output rate */
    t23_enc_rate_init(&r, 15, 1, 30);
    expect(&r, 25, 1, 15, 1, 1, 15, 1, 30);
    /* GOP under one second: kept (vendor would pass 0) */
    t23_enc_rate_init(&r, 25, 1, 10);
    expect(&r, 15, 1, 25, 1, 1, 15, 1, 10);
    /* fractional sensor rate: ceil(seconds * rate) */
    t23_enc_rate_init(&r, 25, 1, 50);
    expect(&r, 30000, 2002, 25, 1, 1, 15000, 1001, 30);
    /* unknown sensor rate: nothing */
    t23_enc_rate_init(&r, 25, 1, 50);
    expect(&r, 0, 1, 25, 1, 0, 25, 1, 50);
    puts("t23 enc rate: ok");
    return 0;
}
