/* Host test of the mode-5 path where the platform draws the picture itself (openimp_tp_ispmid_draw, T40/T41):
 * no frame-sized copy, put(NULL) every tick, 1 = draw again, -1 = rebuild (on + draw), mode 0 = off.
 * make -C tests/testpattern check */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "openimp_testpattern.h"
#include "openimp_testpattern_rt.h"

static int on_n, draw_n, put_n, off_n, put_ret, on_fail;
static uint32_t last_logo = 99;
static const uint8_t *last_nv12 = (const uint8_t *)1;

int openimp_tp_ispmid_on(uint32_t *w, uint32_t *h, uint32_t *stride)
{
    on_n++;
    if (on_fail) {
        errno = EIO;
        return -1;
    }
    *w = *stride = 2880;
    *h = 1620;
    return 0;
}

int openimp_tp_ispmid_draw(uint32_t logo)
{
    draw_n++;
    last_logo = logo;
    return 0;
}

int openimp_tp_ispmid_put(const uint8_t *nv12, uint32_t stride, uint32_t h)
{
    (void)stride; (void)h;
    put_n++;
    last_nv12 = nv12;
    return put_ret;
}

void openimp_tp_ispmid_off(void)
{
    off_n++;
}

int main(void)
{
    OpenIMPTestPatternParams p;
    OpenIMPTestPatternState s;

    OpenIMP_TestPattern_ParamsInit(&p);
    assert(OpenIMP_TestPattern_Supported(5) == 1);
    assert(OpenIMP_TestPattern_Set(5, &p) == 0 && openimp_tp_ispmid_active());
    assert(on_n == 1 && draw_n == 1 && last_logo == p.logo && off_n == 0);
    openimp_tp_ispmid_tick();                           /* held: put(NULL) only */
    assert(put_n == 1 && last_nv12 == NULL && draw_n == 1);
    put_ret = 1;                                        /* fields rewritten: draw again */
    openimp_tp_ispmid_tick();
    assert(draw_n == 2 && on_n == 1 && openimp_tp_ispmid_active());
    put_ret = -1;                                       /* reference moved: rebuild */
    openimp_tp_ispmid_tick();
    assert(on_n == 2 && draw_n == 3 && openimp_tp_ispmid_active());
    put_ret = -1; on_fail = 1;                          /* cannot follow: leave the mode */
    openimp_tp_ispmid_tick();
    assert(!openimp_tp_ispmid_active() && off_n >= 1);
    on_fail = 0; put_ret = 0;
    assert(OpenIMP_TestPattern_Set(5, &p) == 0 && openimp_tp_ispmid_active());
    openimp_tp_ispmid_tick();
    s.size = sizeof s;
    assert(OpenIMP_TestPattern_Get(&s) == 0 && s.mode == 5 && s.frames == 1);
    off_n = 0;
    assert(OpenIMP_TestPattern_Set(0, NULL) == 0 && !openimp_tp_ispmid_active() && off_n == 1);
    puts("tp_inplace: ok");
    return 0;
}
