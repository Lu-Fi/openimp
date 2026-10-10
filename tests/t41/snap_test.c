/* T41 SnapFrame: frame lend state machine and the NV12 de-pitch copy. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "t40/t41_snap.h"

static void lend(void)
{
    struct t41_snap_state s = {0, 0, 0, 0};

    /* Nobody waiting: dequeues and releases are untouched. */
    assert(!t41_snap_on_dequeue(&s, 1));
    assert(!t41_snap_on_release(&s, 1));
    /* Encoder dequeues buffer 2 while a snapshot waits: lent, and the
     * encoder's release is deferred, not refused and not queued. */
    s.waiting = 1;
    assert(t41_snap_on_dequeue(&s, 2) == 1);
    assert(!t41_snap_on_dequeue(&s, 3));          /* one frame only */
    assert(!t41_snap_on_release(&s, 3));          /* other buffers normal */
    assert(t41_snap_on_release(&s, 2) == 1);
    assert(t41_snap_on_release(&s, 2) == -1);     /* double release */
    assert(t41_snap_finish(&s) == 1);             /* snapshot requeues it */
    assert(!s.waiting && !s.ready && !s.deferred);
    /* Copy done before the encoder releases: the encoder requeues. */
    s.waiting = 1;
    assert(t41_snap_on_dequeue(&s, 0) == 1);
    assert(t41_snap_finish(&s) == 0);
    assert(!t41_snap_on_release(&s, 0));
    /* Timed out without a frame. */
    s.waiting = 1;
    assert(t41_snap_finish(&s) == 0);
}

static void copy(void)
{
    const uint32_t w = 20, h = 10, pitch = 32, lines = 16;
    uint32_t size = pitch * lines + pitch * (h / 2), x, y;
    uint8_t *src = malloc(size), *dst = malloc(w * h * 3 / 2 + 1);

    assert(src && dst);
    memset(src, 0xee, size);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            src[y * pitch + x] = (uint8_t)(y * 16 + x);
    for (y = 0; y < h / 2; ++y)
        for (x = 0; x < w; ++x)
            src[pitch * lines + y * pitch + x] = (uint8_t)(0x80 + y * 16 + x);
    dst[w * h * 3 / 2] = 0x5a;
    assert(t41_snap_nv12_bytes(w, h) == w * h * 3 / 2);
    assert(!t41_snap_copy_nv12(dst, src, size, w, h));
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            assert(dst[y * w + x] == (uint8_t)(y * 16 + x));
    for (y = 0; y < h / 2; ++y)
        for (x = 0; x < w; ++x)
            assert(dst[w * h + y * w + x] == (uint8_t)(0x80 + y * 16 + x));
    assert(dst[w * h * 3 / 2] == 0x5a);           /* no overrun */
    /* A buffer too small for the pitched layout, odd sizes: refused. */
    assert(t41_snap_copy_nv12(dst, src, size - 1, w, h) == -1);
    assert(t41_snap_copy_nv12(dst, src, size, w, h - 1) == -1);
    assert(t41_snap_copy_nv12(dst, src, size, 0, h) == -1);
    assert(t41_snap_copy_nv12(NULL, src, size, w, h) == -1);
    /* 1920x1080: 1088 luma lines, UV at 1920 * 1088. */
    assert(t41_snap_align16(1080) == 1088 && t41_snap_align16(1920) == 1920);
    free(src);
    free(dst);
}

int main(void)
{
    lend();
    copy();
    puts("T41 SnapFrame tests passed");
    return 0;
}
