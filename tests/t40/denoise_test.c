#include <stdio.h>
#include "t40/p2_denoise.h"

static int fails;
#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    P2DenoiseCache c = {0, 0, 0};

    /* created without enable: type forced to 0, QPs kept, no range check */
    EXPECT(p2_denoise_apply(&c, false, true, 1, 2, 3) == 0);
    EXPECT(c.type == 0 && c.iqp == 2 && c.pqp == 3);
    EXPECT(p2_denoise_apply(&c, false, true, 7, 4, 5) == 0);
    EXPECT(c.type == 0 && c.iqp == 4 && c.pqp == 5);
    /* created with enable */
    EXPECT(p2_denoise_apply(&c, true, true, 2, 20, 30) == 0);
    EXPECT(c.type == 2 && c.iqp == 20 && c.pqp == 30);
    /* dnType >= 3 or < 0 refused, nothing stored */
    EXPECT(p2_denoise_apply(&c, true, true, 3, 1, 1) == -1);
    EXPECT(p2_denoise_apply(&c, true, true, -1, 1, 1) == -1);
    EXPECT(c.type == 2 && c.iqp == 20 && c.pqp == 30);
    /* T21: hardware path cleared by the vendor encoder; -1 check still runs */
    EXPECT(p2_denoise_apply(&c, true, false, 1, 7, 8) == 0);
    EXPECT(c.type == 0 && c.iqp == 7 && c.pqp == 8);
    EXPECT(p2_denoise_apply(&c, true, false, 5, 1, 1) == -1);
    EXPECT(c.type == 0 && c.iqp == 7 && c.pqp == 8);
    if (fails) return 1;
    puts("denoise_test OK");
    return 0;
}
