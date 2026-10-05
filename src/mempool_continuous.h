#ifndef OPENIMP_MEMPOOL_CONTINUOUS_H
#define OPENIMP_MEMPOOL_CONTINUOUS_H

/*
 * Continuous block manager of the OEM memory pools (libimp
 * core/mempool/Jz_mempool_continuous.c: mempool_continuous_init/alloc/free,
 * identical in T23 1.3.0 and T31 1.1.6).
 *
 * One region (a rmem block reserved by IMP_System_MemPoolRequest, or the ISP
 * OSD picture pool) is cut into blocks kept in address order:
 *   - init zeroes the whole region and starts with one free block; the
 *     region must be at least 0x100 bytes;
 *   - alloc rounds the size up to 256 bytes, takes the first free block
 *     (lowest address) that fits, splits off the remainder and counts the
 *     block as used;
 *   - free looks the address up among the used blocks, marks it free and
 *     merges it with a free successor and then a free predecessor.
 * Pure bookkeeping, no locking: the caller serializes.
 *
 * Differences from the OEM, on purpose: a size above INT_MAX - 255 is
 * refused instead of wrapping the rounding, and blocks are plain malloc
 * nodes without the 0x40-byte owner-name copies.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MPC_ALIGN     256u
#define MPC_MIN_POOL  0x100u

typedef struct MpcBlock {
    struct MpcBlock *prev;
    struct MpcBlock *next;
    size_t off;
    size_t len;
    int used;
} MpcBlock;

typedef struct {
    uint8_t *base;
    size_t size;
    MpcBlock *head;             /* lowest block, circular list */
    int used_blocks;
} MpcPool;

static inline MpcPool *mpc_init(void *base, size_t size)
{
    MpcPool *p;
    MpcBlock *b;

    if (!base || size < MPC_MIN_POOL)
        return NULL;
    p = (MpcPool *)calloc(1, sizeof(*p));
    b = (MpcBlock *)calloc(1, sizeof(*b));
    if (!p || !b) {
        free(p);
        free(b);
        return NULL;
    }
    memset(base, 0, size);                      /* OEM clears the region */
    b->prev = b->next = b;
    b->len = size;
    p->base = (uint8_t *)base;
    p->size = size;
    p->head = b;
    return p;
}

/* Returns the block address, or NULL when nothing fits (OEM returns 0). */
static inline void *mpc_alloc(MpcPool *p, int size)
{
    MpcBlock *b;
    size_t need;

    if (!p || size <= 0 || (unsigned int)size > 0x7fffff00u)
        return NULL;
    need = ((size_t)size + (MPC_ALIGN - 1u)) & ~(size_t)(MPC_ALIGN - 1u);
    b = p->head;
    do {
        if (!b->used && b->len >= need) {
            if (b->len > need) {
                MpcBlock *rest = (MpcBlock *)calloc(1, sizeof(*rest));

                if (!rest)
                    return NULL;
                rest->off = b->off + need;
                rest->len = b->len - need;
                rest->prev = b;
                rest->next = b->next;
                b->next->prev = rest;
                b->next = rest;
                b->len = need;
            }
            b->used = 1;
            p->used_blocks++;
            return p->base + b->off;
        }
        b = b->next;
    } while (b != p->head);
    return NULL;
}

/* 0 on success, -1 when no used block starts at ptr (OEM logs and goes on). */
static inline int mpc_free(MpcPool *p, void *ptr)
{
    MpcBlock *b;

    if (!p || !ptr)
        return -1;
    b = p->head;
    do {
        if (b->used && p->base + b->off == (uint8_t *)ptr)
            break;
        b = b->next;
    } while (b != p->head);
    if (b == p->head && !(b->used && p->base + b->off == (uint8_t *)ptr))
        return -1;
    b->used = 0;
    if (b->next != p->head && !b->next->used) {
        MpcBlock *n = b->next;

        b->len += n->len;
        b->next = n->next;
        n->next->prev = b;
        free(n);
    }
    if (b != p->head && !b->prev->used) {
        MpcBlock *q = b->prev;

        q->len += b->len;
        q->next = b->next;
        b->next->prev = q;
        free(b);
    }
    p->used_blocks--;
    return 0;
}

static inline void mpc_deinit(MpcPool *p)
{
    MpcBlock *b, *n;

    if (!p)
        return;
    b = p->head;
    do {
        n = b->next;
        free(b);
        b = n;
    } while (b != p->head);
    free(p);
}

#endif
