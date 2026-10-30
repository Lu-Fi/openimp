#ifndef OPENIMP_MONO_COND_H
#define OPENIMP_MONO_COND_H

/* Condition variables for the frame path wait on CLOCK_MONOTONIC: a step of
 * the system clock (manual set, NTP step) must not stretch or cut a timed
 * wait that runs once per frame. */
#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* (Re)initialise `cond` with the monotonic clock; only before any use. */
static inline int openimp_cond_init_mono(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    int rc;

    if (pthread_condattr_init(&attr) != 0)
        return pthread_cond_init(cond, NULL);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

/* now (CLOCK_MONOTONIC) + after_us, for pthread_cond_timedwait on a cond
 * made by openimp_cond_init_mono. */
static inline void openimp_deadline_mono(struct timespec *ts, uint64_t after_us)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(after_us / 1000000u);
    ts->tv_nsec += (long)(after_us % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

#endif /* OPENIMP_MONO_COND_H */
