/*
 * video_drop.c - monitor thread behind IMP_ISP_Tuning_SetVideoDrop
 * (see video_drop.h).  The thread exists only while a callback is
 * registered; it wakes once per tick, so it costs nothing per frame.
 */
#include <pthread.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <time.h>

#include "imp_log_int.h"
#include "video_drop.h"

#ifndef OPENIMP_VIDEO_DROP_TICK_MS
#define OPENIMP_VIDEO_DROP_TICK_MS 1000
#endif

volatile uint32_t openimp_video_drop_frames;

static pthread_mutex_t video_drop_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*video_drop_cb)(void);
static int video_drop_running;

static void *video_drop_thread(void *arg)
{
    OpenIMPVideoDropState state = { 0, 0, 0 };

    (void)arg;
    prctl(PR_SET_NAME, "videodrop");
    state.last_frames = __atomic_load_n(&openimp_video_drop_frames,
                                        __ATOMIC_RELAXED);
    for (;;) {
        struct timespec tick = {
            OPENIMP_VIDEO_DROP_TICK_MS / 1000,
            (OPENIMP_VIDEO_DROP_TICK_MS % 1000) * 1000000L
        };
        void (*cb)(void);
        uint32_t frames;

        while (nanosleep(&tick, &tick) != 0)
            ;
        pthread_mutex_lock(&video_drop_lock);
        cb = video_drop_cb;
        if (!cb)
            video_drop_running = 0;
        pthread_mutex_unlock(&video_drop_lock);
        if (!cb)
            break;
        frames = __atomic_load_n(&openimp_video_drop_frames,
                                 __ATOMIC_RELAXED);
        if (openimp_video_drop_step(&state, frames,
                                    openimp_fs_video_demand())) {
            IMP_LOG_ERR("ISP", "video drop: no frame for %u s (report %u)",
                        2u * state.reports, state.reports);
            cb();
        }
    }
    return NULL;
}

int openimp_video_drop_set(void (*cb)(void))
{
    int result = 0;

    pthread_mutex_lock(&video_drop_lock);
    video_drop_cb = cb;
    if (cb && !video_drop_running) {
        pthread_attr_t attr;
        pthread_t thread;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, video_drop_thread, NULL) == 0) {
            video_drop_running = 1;
        } else {
            video_drop_cb = NULL;
            result = -1;
        }
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&video_drop_lock);
    return result;
}
