/*
 * T32/T33 encoder channel: the stream slots and the FIFO of coded streams
 * (READY) that GetStream hands out in coding order.
 *
 * Vendor behaviour (libimp T33 2.0.2.1, T32 2.2.0, T31 1.1.6, T30 1.0.5):
 * - a frame that finds no free stream buffer is dropped, the queued streams
 *   stay (T33 get_empty_framestream_noblock = sem_trywait on the free-buffer
 *   semaphore, "the bitstream buf is full", drop counter, the new frame is
 *   released); t32_encq_free() returning -1 is the same.
 * - IMP_Encoder_StopRecvPic, after the frames in work are coded, drains
 *   every stream still queued with PollingStream / GetStream /
 *   ReleaseStream (T33 0xf6520, up to 10 rounds; T31 0x9591c, T30 likewise),
 *   so StartRecvPic never returns a stream coded before the stop (#691).
 *   A stream the application holds (GetStream without ReleaseStream) stays
 *   with it.  t32_encq_stop() is that drain.
 * - IMP_Encoder_FlushStream drops the queued streams the same way.
 */
#ifndef OPENIMP_T32_ENCQ_H
#define OPENIMP_T32_ENCQ_H

#define T32_ENCQ_MAX 8

enum { SLOT_FREE, SLOT_READY, SLOT_USER };

struct t32_encq {
    int state[T32_ENCQ_MAX];            /* SLOT_* of each stream slot */
    int ready[T32_ENCQ_MAX];            /* FIFO of SLOT_READY slots */
    int rhead, rcount;
    int nslots;
};

static inline void t32_encq_init(struct t32_encq *q, int nslots)
{
    int i;

    for (i = 0; i < T32_ENCQ_MAX; i++)
        q->state[i] = SLOT_FREE;
    q->rhead = q->rcount = 0;
    q->nslots = nslots;
}

/* a free slot for the next coded stream, -1 when all are queued or held */
static inline int t32_encq_free(const struct t32_encq *q)
{
    int i;

    for (i = 0; i < q->nslots; i++)
        if (q->state[i] == SLOT_FREE)
            return i;
    return -1;
}

/* slot @si holds a coded stream: queue it behind the others */
static inline void t32_encq_ready(struct t32_encq *q, int si)
{
    q->state[si] = SLOT_READY;
    q->ready[(q->rhead + q->rcount) % T32_ENCQ_MAX] = si;
    q->rcount++;
}

/* the oldest i-th queued slot (0 = next for GetStream) */
static inline int t32_encq_peek(const struct t32_encq *q, int i)
{
    return q->ready[(q->rhead + i) % T32_ENCQ_MAX];
}

/* GetStream: the oldest queued slot goes to the application, -1 when empty */
static inline int t32_encq_take(struct t32_encq *q)
{
    int si;

    if (!q->rcount)
        return -1;
    si = q->ready[q->rhead];
    q->rhead = (q->rhead + 1) % T32_ENCQ_MAX;
    q->rcount--;
    q->state[si] = SLOT_USER;
    return si;
}

/* ReleaseStream: 0 when @si was held by the application */
static inline int t32_encq_release(struct t32_encq *q, int si)
{
    if (si < 0 || si >= q->nslots || q->state[si] != SLOT_USER)
        return -1;
    q->state[si] = SLOT_FREE;
    return 0;
}

/* FlushStream / StopRecvPic: drop every queued stream, return how many */
static inline int t32_encq_drop_ready(struct t32_encq *q)
{
    int n = 0;

    while (q->rcount) {
        q->state[q->ready[q->rhead]] = SLOT_FREE;
        q->rhead = (q->rhead + 1) % T32_ENCQ_MAX;
        q->rcount--;
        n++;
    }
    return n;
}

/* StopRecvPic, after the frames in work are coded (vendor drain, #691) */
static inline int t32_encq_stop(struct t32_encq *q)
{
    return t32_encq_drop_ready(q);
}

/* a slot is queued or held by the application */
static inline int t32_encq_busy(const struct t32_encq *q)
{
    int i;

    for (i = 0; i < q->nslots; i++)
        if (q->state[i] != SLOT_FREE)
            return 1;
    return 0;
}

#endif
