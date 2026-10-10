/*
 * Low-address fake allocator for host tests.
 *
 * The encoder, IVS and ISP ABIs keep addresses in 32-bit fields, as on
 * MIPS.  On an x86-64 host the fakes must therefore hand out memory below
 * 4 GiB.  MAP_32BIT is not enough: with ASLR the kernel places MAP_32BIT
 * mappings bottom-up from about 1 GiB, and the brk heap of a non-PIE binary
 * anywhere in the first GiB above its data.  When a mapping lands directly
 * above the heap top, the next heap growth fails, glibc falls back to mmap
 * above 4 GiB and a truncated address crashes the test (about 2 % of runs,
 * see tests/t30 helix_encoder_test).
 *
 * fake_rmem_map() carves from one fixed window (default 256 MiB at
 * 1.5 GiB, above the heap's reach) mapped with MAP_FIXED_NOREPLACE, first
 * fit.  fake_rmem_unmap() DONTNEEDs the range and makes it PROT_NONE, so
 * freed memory still faults and comes back zeroed, as after munmap.
 *
 * Header only, state is per translation unit; a second TU (or a second
 * user in the same process) simply takes the next free window.
 * Override FAKE_RMEM_SIZE / FAKE_RMEM_SLOTS before including if needed.
 */
#ifndef OPENIMP_TESTS_FAKE_RMEM_H
#define OPENIMP_TESTS_FAKE_RMEM_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef FAKE_RMEM_BASE
#define FAKE_RMEM_BASE 0x60000000u
#endif
#ifndef FAKE_RMEM_SIZE
#define FAKE_RMEM_SIZE (256u << 20)
#endif
#ifndef FAKE_RMEM_SLOTS
#define FAKE_RMEM_SLOTS 64u
#endif
#define FAKE_RMEM_PAGE 4096u

static uint8_t *fake_rmem_window;
static struct { uintptr_t start, end; } fake_rmem_live[FAKE_RMEM_SLOTS];

/* Abort with a message: the asserts below must also hold under NDEBUG. */
static inline void fake_rmem_check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "fake_rmem: %s\n", what);
        abort();
    }
}

static inline void *fake_rmem_map(size_t size)
{
    uintptr_t start, end;
    unsigned int i;
    int moved;

    if (!fake_rmem_window) {
        uintptr_t base;

        /* the next free window below 4 GiB, normally the first */
        for (base = FAKE_RMEM_BASE;
             base + (uintptr_t)FAKE_RMEM_SIZE <= 0xffffffffu;
             base += FAKE_RMEM_SIZE) {
            void *p = mmap((void *)base, FAKE_RMEM_SIZE, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                           MAP_FIXED_NOREPLACE, -1, 0);

            if (p == (void *)base) {
                fake_rmem_window = p;
                break;
            }
            if (p != MAP_FAILED) /* old kernel: a hint only */
                munmap(p, FAKE_RMEM_SIZE);
        }
        fake_rmem_check(fake_rmem_window != NULL, "no free low window");
    }
    size = (size + FAKE_RMEM_PAGE - 1u) & ~(size_t)(FAKE_RMEM_PAGE - 1u);
    start = (uintptr_t)fake_rmem_window;
    do {
        moved = 0;
        end = start + size;
        for (i = 0; i < FAKE_RMEM_SLOTS; i++) {
            if (fake_rmem_live[i].end && start < fake_rmem_live[i].end &&
                fake_rmem_live[i].start < end) {
                start = fake_rmem_live[i].end;
                moved = 1;
                break;
            }
        }
    } while (moved);
    fake_rmem_check(end <= (uintptr_t)fake_rmem_window + FAKE_RMEM_SIZE,
                    "window exhausted");
    for (i = 0; i < FAKE_RMEM_SLOTS && fake_rmem_live[i].end; i++)
        ;
    fake_rmem_check(i < FAKE_RMEM_SLOTS, "out of slots");
    fake_rmem_live[i].start = start;
    fake_rmem_live[i].end = end;
    fake_rmem_check(mprotect((void *)start, size,
                             PROT_READ | PROT_WRITE) == 0, "mprotect");
    return (void *)start;
}

/* freed memory faults again, as after munmap, and comes back zeroed */
static inline void fake_rmem_unmap(void *mapping, size_t size)
{
    uintptr_t start = (uintptr_t)mapping;
    unsigned int i;

    size = (size + FAKE_RMEM_PAGE - 1u) & ~(size_t)(FAKE_RMEM_PAGE - 1u);
    for (i = 0; i < FAKE_RMEM_SLOTS; i++)
        if (fake_rmem_live[i].start == start && fake_rmem_live[i].end)
            break;
    fake_rmem_check(i < FAKE_RMEM_SLOTS &&
                    fake_rmem_live[i].end == start + size,
                    "unmap of an unknown range");
    fake_rmem_check(madvise(mapping, size, MADV_DONTNEED) == 0, "madvise");
    fake_rmem_check(mprotect(mapping, size, PROT_NONE) == 0, "mprotect");
    fake_rmem_live[i].start = fake_rmem_live[i].end = 0;
}

#endif
