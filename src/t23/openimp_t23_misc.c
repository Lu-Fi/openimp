/* T23 1.3.0 FrameSource/System odds and ends. */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <imp/imp_framesource.h>

#include "dma_alloc.h"
#include "imp_log_int.h"

#define T23_FS_CHANNELS 9

static pthread_mutex_t misc_lock = PTHREAD_MUTEX_INITIALIZER;
static int direct_threshold[T23_FS_CHANNELS];

static int fs_channel_created(int chn)
{
    IMPFSChnAttr attr;

    memset(&attr, 0, sizeof(attr));
    return chn >= 0 && chn < T23_FS_CHANNELS &&
           IMP_FrameSource_GetChnAttr(chn, &attr) == 0;
}

/* OEM: the cache threshold that decides between encoding and dropping in
 * the dual-sensor IVDC direct mode; it is only stored on the created
 * channel (and read back), which is all a single-sensor stack needs. */
int IMP_FrameSource_SetDirectModeAttr(int chn, int data_threshold)
{
    if (!fs_channel_created(chn))
        return -1;
    pthread_mutex_lock(&misc_lock);
    direct_threshold[chn] = data_threshold;
    pthread_mutex_unlock(&misc_lock);
    return 0;
}

int IMP_FrameSource_GetDirectModeAttr(int chn, int *data_threshold)
{
    if (!data_threshold || !fs_channel_created(chn))
        return -1;
    pthread_mutex_lock(&misc_lock);
    *data_threshold = direct_threshold[chn];
    pthread_mutex_unlock(&misc_lock);
    return 0;
}

/* OEM: IMP_System_MemPoolRequest is IMP_MemPool_InitPool(poolId, size,
 * name): one block of rmem, zeroed, managed in 256-byte units, from which
 * the channels bound with IMP_FrameSource_SetPool take their buffers
 * (src/dma_alloc.c).  T23 1.3.0 has no IMP_System_MemPoolFree. */
int IMP_System_MemPoolRequest(int poolId, size_t size, char *name)
{
    return IMP_MemPool_InitPool(poolId, size, name);
}
