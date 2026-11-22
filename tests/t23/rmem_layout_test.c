/* Host simulation of the T23 reserved-memory arena with the native Helix
 * encoder (Galayou Y4: rmem=22M, 1920x1080 main + 640x360 sub, two-buffer
 * FrameSource pools).
 *
 * The arena is OpenIMP's real allocator (src/rmem_arena.h, the same code
 * src/dma_alloc.c runs on the camera); the encoder buffers come from the
 * real OpenIMP_T30_HelixCreate/Destroy against a fake /dev/soc_vpu that only
 * hands out channels.  The other allocations are sized from the device log
 * of the first native run (ISP NCU buffer, pools, OSD).  The test replays
 * timps' order - start, chn0 idle and re-enable, sub channel starting while
 * chn0 is idle, snapshots, encoder restarts - and requires that every
 * FrameSource pool can always be re-created.  It also replays the device
 * log with the first native encoder's layout, which must fail with the
 * same largest free block the camera reported (5554176 bytes). */
#define _GNU_SOURCE
#include <assert.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma_alloc.h"
#include "rmem_arena.h"
#include "t30/t30_helix_encoder.h"
#include "t30/t23_helix_bs.h"
#include "imp/imp_framesource.h"

/* FrameSource channel sizes the encoder's area sizing sees (#768); all 0
 * = no FrameSource, the 2 MiB default */
static uint32_t fs_w[3], fs_h[3];

int IMP_FrameSource_GetChnAttr(int chn, IMPFSChnAttr *attr)
{
    if (chn < 0 || chn >= 3 || !fs_w[chn])
        return -1;
    memset(attr, 0, sizeof(*attr));
    attr->picWidth = (int)fs_w[chn];
    attr->picHeight = (int)fs_h[chn];
    return 0;
}

#ifndef RMEM_SIZE
#define RMEM_SIZE   (22u << 20)
#endif
#define RMEM_PHYS   0x02a00000u
#define FAKE_FD     4243

#ifndef MAIN_W
#define MAIN_W 1920u
#endif
#ifndef MAIN_H
#define MAIN_H 1080u
#endif
#ifndef SUB_W
#define SUB_W  640u
#endif
#ifndef SUB_H
#define SUB_H  360u
#endif
/* VBM frame size: macroblock-aligned NV12 */
#define FRAME(w, h) ((((w) + 15u) & ~15u) * (((h) + 15u) & ~15u) * 3u / 2u)
#define POOL0 (2u * FRAME(MAIN_W, MAIN_H))   /* 6266880, as in the log */
#define POOL1 (2u * FRAME(SUB_W, SUB_H))
#ifndef ISP_NCU
#define ISP_NCU 3133440u
#endif
#define OSD_PIECES 4u
#define OSD_PIECE (68u << 10)

static RmemArena arena;
static int top_allocations;

static int arena_alloc(IMPDMABufferInfo *info, int size, int top)
{
    size_t off;

    if ((top ? rmem_arena_alloc_top(&arena, (size_t)size, &off)
             : rmem_arena_alloc(&arena, (size_t)size, &off)) != 0)
        return -1;
    memset(info, 0, sizeof(*info));
    info->phys_addr = RMEM_PHYS + (uint32_t)off;
    /* never dereferenced for encoder buffers except by memset below */
    info->virt_addr = (uint32_t)(uintptr_t)malloc((size_t)size);
    assert(info->virt_addr);
    info->size = (uint32_t)size;
    return 0;
}

/* encoder buffers keep a heap shadow so memset works; remember it */
static struct { uint32_t phys; void *virt; } shadows[64];

static void remember(const IMPDMABufferInfo *info)
{
    unsigned int i;

    for (i = 0; i < 64u; i++)
        if (!shadows[i].phys) {
            shadows[i].phys = info->phys_addr;
            shadows[i].virt = (void *)(uintptr_t)info->virt_addr;
            return;
        }
    abort();
}

int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    (void)tag;
    if (arena_alloc(info, size, 0) != 0)
        return -1;
    remember(info);
    return 0;
}

int DMA_AllocDescriptorTop(IMPDMABufferInfo *info, int size,
                           const char *tag)
{
    (void)tag;
    if (arena_alloc(info, size, 1) != 0)
        return -1;
    top_allocations++;
    remember(info);
    return 0;
}

int DMA_RmemStats(size_t *used, size_t *size, size_t *largest)
{
    if (used)
        *used = 0;
    if (size)
        *size = 0;
    if (largest)
        *largest = 0;
    return 0;
}

int DMA_FreePhys(uint32_t phys)
{
    unsigned int i;

    assert(rmem_arena_free(&arena, phys - RMEM_PHYS, NULL) == 0);
    for (i = 0; i < 64u; i++)
        if (shadows[i].phys == phys) {
            free(shadows[i].virt);
            shadows[i].phys = 0;
        }
    return 0;
}

uint32_t DMA_VirtToPhys(const void *virt)
{
    return (uint32_t)(uintptr_t)virt;
}

int DMA_RmemFlushCache(void *virt, uint32_t size, int dir)
{
    (void)virt;
    (void)size;
    (void)dir;
    return 0;
}

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/soc_vpu") == 0)
        return FAKE_FD;
    return __real_open(path, flags, 0644);
}

int __wrap_close(int fd)
{
    return fd == FAKE_FD ? 0 : __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    uint32_t *node;

    va_start(args, request);
    node = va_arg(args, uint32_t *);
    va_end(args);
    assert(fd == FAKE_FD);
    if (request == 0xc0586300u) {
        node[0] = 0x8000u;          /* clist */
        return 0;
    }
    assert(request == 0xc0586301u);
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    IMPDMABufferInfo info;
    int live;
} Block;

static Block isp, pool0, pool1, osd[OSD_PIECES], snapshot;
static T30HelixEncoder *main_encoder, *sub_encoder;
static size_t worst_free = (size_t)-1;
static unsigned int pool_enables;

static void block_alloc(Block *b, uint32_t size)
{
    assert(!b->live);
    if (DMA_AllocDescriptor(&b->info, (int)size, "sim") != 0) {
        fprintf(stderr, "allocation of %u failed: used %zu of %zu, "
                "largest free block %zu, %d extents\n", size, arena.used,
                arena.size, rmem_arena_largest_gap(&arena), arena.count);
        abort();
    }
    b->live = 1;
}

static void block_free(Block *b)
{
    if (b->live)
        DMA_FreePhys(b->info.phys_addr);
    b->live = 0;
}

static void pool_enable(Block *pool, uint32_t size)
{
    size_t largest;

    if (pool->live)
        return;
    largest = rmem_arena_largest_gap(&arena);
    assert(largest >= size);
    block_alloc(pool, size);
    pool_enables++;
}

static void encoder_create(T30HelixEncoder **encoder, uint32_t w,
                           uint32_t h)
{
    HWEncoderParams params;

    if (*encoder)
        return;
    memset(&params, 0, sizeof(params));
    params.width = w;
    params.height = h;
    params.fps_num = 25;
    params.fps_den = 1;
    params.gop_length = 50;
    params.rc_mode = HW_RC_MODE_CBR;
    params.bitrate = w > 640u ? 1200000u : 384000u;
    params.qp = 32;
    params.min_qp = 20;
    params.max_qp = 45;
    if (OpenIMP_T30_HelixCreate(encoder, &params) != 0) {
        fprintf(stderr, "encoder %ux%u create failed: used %zu, largest "
                "free block %zu\n", w, h, arena.used,
                rmem_arena_largest_gap(&arena));
        abort();
    }
}

static void encoder_destroy(T30HelixEncoder **encoder)
{
    OpenIMP_T30_HelixDestroy(*encoder);
    *encoder = NULL;
}

static void note_free(void)
{
    if (arena.size - arena.used < worst_free)
        worst_free = arena.size - arena.used;
}

static void start(int sub_first_after_idle)
{
    unsigned int i;

    rmem_arena_init(&arena, RMEM_SIZE);
    block_alloc(&isp, ISP_NCU);
    /* #799: both encoders exist from IMP_Encoder_CreateChn on, in
     * channel order, before the FrameSource pools (vendor order) */
    encoder_create(&main_encoder, MAIN_W, MAIN_H);
    encoder_create(&sub_encoder, SUB_W, SUB_H);
    pool_enable(&pool0, POOL0);
    if (!sub_first_after_idle)
        pool_enable(&pool1, POOL1);
    for (i = 0; i < OSD_PIECES; i++)
        block_alloc(&osd[i], OSD_PIECE);
    encoder_create(&main_encoder, MAIN_W, MAIN_H);
    if (!sub_first_after_idle)
        encoder_create(&sub_encoder, SUB_W, SUB_H);
    note_free();
}

static void stop_all(void)
{
    unsigned int i;

    encoder_destroy(&main_encoder);
    encoder_destroy(&sub_encoder);
    block_free(&snapshot);
    block_free(&pool0);
    block_free(&pool1);
    for (i = 0; i < OSD_PIECES; i++)
        block_free(&osd[i]);
    block_free(&isp);
    assert(arena.used == 0 && arena.count == 0);
}

/* The first device run: the previous native encoder (2 MiB EMC scratch
 * and a 1 MiB + 4 KiB bitstream window per channel, every buffer best-fit
 * from the bottom).  Replaying the log order with it must fail exactly as
 * on the camera, which shows the model matches the device. */
static int legacy_log_order_fails(void)
{
    static const uint32_t main_parts[] = {
        16384u, 2097152u, 1052672u, FRAME(MAIN_W, MAIN_H),
        FRAME(MAIN_W, MAIN_H)
    };
    static const uint32_t sub_parts[] = {
        16384u, 2097152u, 1052672u, FRAME(SUB_W, SUB_H),
        FRAME(SUB_W, SUB_H)
    };
    Block parts[10];
    size_t off;
    unsigned int i;
    int fits;

    memset(parts, 0, sizeof(parts));
    rmem_arena_init(&arena, RMEM_SIZE);
    block_alloc(&isp, ISP_NCU);
    block_alloc(&pool0, POOL0);
    for (i = 0; i < OSD_PIECES; i++)
        block_alloc(&osd[i], OSD_PIECE);
    for (i = 0; i < 5u; i++)
        block_alloc(&parts[i], main_parts[i]);
    block_free(&pool0);
    block_alloc(&pool1, POOL1);
    for (i = 0; i < 5u; i++)
        block_alloc(&parts[5 + i], sub_parts[i]);
    printf("legacy layout: used %zu of %zu, largest free block %zu, main "
           "pool needs %u\n", arena.used, arena.size,
           rmem_arena_largest_gap(&arena), POOL0);
    fits = rmem_arena_alloc(&arena, POOL0, &off) == 0;
    if (fits)
        rmem_arena_free(&arena, off, NULL);
    for (i = 0; i < 10u; i++)
        block_free(&parts[i]);
    block_free(&pool1);
    for (i = 0; i < OSD_PIECES; i++)
        block_free(&osd[i]);
    block_free(&isp);
    return !fits;
}

int main(void)
{
    unsigned int step;
    uint32_t seed = 12345u;
    uint32_t main_size, sub_size;

    /* the encoder keeps addresses in 32-bit words, as on MIPS */
    mallopt(M_MMAP_MAX, 0);
    /* per-channel encoder memory */
    rmem_arena_init(&arena, RMEM_SIZE);
    encoder_create(&main_encoder, MAIN_W, MAIN_H);
    main_size = (uint32_t)arena.used;
    encoder_create(&sub_encoder, SUB_W, SUB_H);
    sub_size = (uint32_t)arena.used - main_size;
    encoder_destroy(&main_encoder);
    encoder_destroy(&sub_encoder);
    assert(arena.used == 0);
    printf("native encoder rmem: %ux%u %u KiB, %ux%u %u KiB\n", MAIN_W,
           MAIN_H, main_size >> 10, SUB_W, SUB_H, sub_size >> 10);

    assert(legacy_log_order_fails());

    /* 1: the device log's order - main starts alone, chn0 idles, the sub
     * channel starts while chn0 is idle, chn0 comes back.  Since #799 the
     * sub encoder already exists from its CreateChn on (start()); the
     * lazy order (sub encoder made after its pool, OPENIMP_HELIX_LAZY_
     * CREATE=1) is no longer the default. */
    start(1);
    block_free(&pool0);                     /* chn0 idle */
    pool_enable(&pool1, POOL1);             /* chn1 streaming */
    note_free();
    pool_enable(&pool0, POOL0);             /* chn0 re-enable */
    note_free();
    stop_all();
    printf("log order: chn0 re-enabled after the sub channel started\n");

    /* 2: long random run of idle/re-enable cycles, snapshots and encoder
     * restarts (a channel's codec is re-created when timps restarts it) */
    start(0);
    for (step = 0; step < 20000u; step++) {
        unsigned int action;

        seed = seed * 1103515245u + 12345u;
        action = (seed >> 16) % 10u;
        switch (action) {
        case 0: block_free(&pool0); break;
        case 1: pool_enable(&pool0, POOL0); break;
        case 2: block_free(&pool1); break;
        case 3: pool_enable(&pool1, POOL1); break;
        case 4:
            /* JPEG snapshot staging while chn0 may be idle */
            if (!snapshot.live)
                block_alloc(&snapshot, 256u << 10);
            break;
        case 5: block_free(&snapshot); break;
        case 6:
            encoder_destroy(&sub_encoder);
            encoder_create(&sub_encoder, SUB_W, SUB_H);
            break;
        case 7:
            if (!pool0.live) {      /* main codec restart while idle */
                encoder_destroy(&main_encoder);
                encoder_create(&main_encoder, MAIN_W, MAIN_H);
            }
            break;
        default:
            pool_enable(&pool0, POOL0);
            pool_enable(&pool1, POOL1);
            break;
        }
        note_free();
        /* a snapshot is transient: pools must fit once it is gone */
        if (!snapshot.live) {
            pool_enable(&pool0, POOL0);
            pool_enable(&pool1, POOL1);
        }
    }
    stop_all();
    printf("random run: %u pool re-creations, all succeeded; least free "
           "%zu KiB (with a 256 KiB snapshot buffer live)\n",
           pool_enables, worst_free >> 10);
    assert(top_allocations > 0);
    /* #768: the shared area follows the largest FrameSource picture */
    {
        const uint32_t w720[2] = { 1280u, 640u }, h720[2] = { 720u, 360u };
        const uint32_t w1080[1] = { 1920u }, h1080[1] = { 1080u };
        uint32_t m720, s720, area;

        assert(t23_bs_window_bytes(1280u, 720u) == 1408u << 10);
        assert(t23_bs_window_bytes(640u, 360u) == 384u << 10);
        assert(t23_bs_window_bytes(1920u, 1080u) == 2u << 20);
        assert(t23_bs_window_bytes(176u, 144u) == 256u << 10);
        assert(t23_bs_area_bytes(w720, h720, 2u, 0u) == (1408u << 10) + 4096u);
        assert(t23_bs_area_bytes(w1080, h1080, 1u, 0u) == (2u << 20) + 4096u);
        assert(t23_bs_area_bytes(w720, h720, 0u, 0u) == (2u << 20) + 4096u);
        assert(t23_bs_area_bytes(w720, h720, 2u, 3u << 20) == 3u << 20);

        fs_w[0] = 1280u; fs_h[0] = 720u; fs_w[1] = 640u; fs_h[1] = 360u;
        rmem_arena_init(&arena, 32u << 20);
        encoder_create(&sub_encoder, 640u, 360u);   /* timps: sub first */
        s720 = (uint32_t)arena.used;
        encoder_create(&main_encoder, 1280u, 720u);
        m720 = (uint32_t)arena.used - s720;
        area = OpenIMP_T23_HelixBs_Size();
        encoder_destroy(&main_encoder);
        encoder_destroy(&sub_encoder);
        assert(arena.used == 0);
        printf("720p sensor: area %u KiB, sub 640x360 %u KiB, main "
               "1280x720 %u KiB\n", area >> 10, s720 >> 10, m720 >> 10);
        assert(area == (1408u << 10) + 4096u);
        memset(fs_w, 0, sizeof(fs_w));
        memset(fs_h, 0, sizeof(fs_h));
    }
    /* #799: O-KAM QC3 / A12 (T23DL, rmem=8M): 1280x720 main over IVDC,
     * 640x360 sub, two JPEG channels.  Every encoder channel holds the
     * shared area from its CreateChn on (stock vpuBs from EncoderInit),
     * the H.264 buffers come at CreateChn in channel order, so neither the
     * JPEG buffer nor the streaming order matters any more. */
    {
        static const unsigned int orders[2][2] = { { 0, 1 }, { 1, 0 } };
        unsigned int o;

        fs_w[0] = 1280u; fs_h[0] = 720u; fs_w[1] = 640u; fs_h[1] = 360u;
        for (o = 0; o < 2u; o++) {
            Block ncu = { 0 }, ivdc = { 0 }, vbm1 = { 0 }, osdb = { 0 };
            T30HelixEncoder **enc[2] = { &main_encoder, &sub_encoder };
            const uint32_t w[2] = { 1280u, 640u }, h[2] = { 720u, 360u };
            unsigned int k, holders = 0;
            size_t enc_bytes;

            rmem_arena_init(&arena, 8u << 20);
            block_alloc(&ncu, 1384448u);        /* ISP NCU */
            for (k = 0; k < 2u; k++) {          /* H.264 CreateChn */
                unsigned int c = orders[o][k];

                assert(OpenIMP_T23_HelixBs_Prepare() == 0);
                holders++;
                encoder_create(enc[c], w[c], h[c]);
            }
            for (k = 0; k < 2u; k++) {          /* JPEG CreateChn */
                assert(OpenIMP_T23_HelixBs_Prepare() == 0);
                holders++;
            }
            /* JPEG 1280x720 needs 1056768 bytes: it fits the area */
            assert(OpenIMP_T23_HelixBs_Size() >= 1056768u);
            enc_bytes = arena.used - 1384448u;
            block_alloc(&osdb, 350u << 10);     /* timps OSD, A12 run */
            block_alloc(&ivdc, 576000u);        /* IVDC ring, 300 lines */
            block_alloc(&vbm1, 706560u);        /* sub FrameSource pool */
            printf("#799 A12 rmem=8M, H.264 CreateChn %s first: encoder "
                   "%zu KiB (area %u KiB), free %zu KiB\n",
                   orders[o][0] ? "sub" : "main", enc_bytes >> 10,
                   OpenIMP_T23_HelixBs_Size() >> 10,
                   (arena.size - arena.used) >> 10);
            block_free(&vbm1);
            block_free(&ivdc);
            block_free(&osdb);
            encoder_destroy(&main_encoder);
            encoder_destroy(&sub_encoder);
            assert(OpenIMP_T23_HelixBs_Size() != 0u);   /* JPEG holds it */
            while (holders--)
                OpenIMP_T23_HelixBs_Unprepare();
            assert(OpenIMP_T23_HelixBs_Size() == 0u);
            block_free(&ncu);
            assert(arena.used == 0);
        }
        /* JPEG first: the area exists without an H.264 channel, the
         * first H.264 channel adopts the spill guard as its scratch */
        rmem_arena_init(&arena, 8u << 20);
        assert(OpenIMP_T23_HelixBs_Prepare() == 0);
        {
            size_t before = arena.used;

            assert(OpenIMP_T23_HelixBs_Prepare() == 0);
            encoder_create(&main_encoder, 1280u, 720u);
            /* adopted guard: no second EMC scratch */
            assert(arena.used - before < (2u << 20));
            encoder_destroy(&main_encoder);
        }
        OpenIMP_T23_HelixBs_Unprepare();
        OpenIMP_T23_HelixBs_Unprepare();
        assert(arena.used == 0);
        memset(fs_w, 0, sizeof(fs_w));
        memset(fs_h, 0, sizeof(fs_h));
    }
    printf("T23 rmem layout simulation passed\n");
    return 0;
}
