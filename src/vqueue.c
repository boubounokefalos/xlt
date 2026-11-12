#include "vqueue.h"

void vq_reset(VQueue *q, int nslots)
{
    for (int i = 0; i < VQ_MAX_SLOTS; i++)
        q->busy[i] = 0;
    q->head = q->tail = 0;
    q->nslots = nslots > VQ_MAX_SLOTS ? VQ_MAX_SLOTS : nslots;
    q->skipped = 0;
}

int vq_empty(const VQueue *q)
{
    return q->head == q->tail;
}

int vq_free_slot(const VQueue *q, int shown, int published)
{
    for (int k = 0; k < q->nslots; k++)
        if (k != shown && k != published && !q->busy[k])
            return k;
    return -1;
}

void vq_push(VQueue *q, int slot, int ms, int gen)
{
    int t = q->tail;
    q->busy[slot] = 1;
    q->e[t].slot = slot;
    q->e[t].ms = ms;
    q->e[t].gen = gen;
    q->tail = (t + 1) % VQ_RING; /* last: the reader sees only a complete entry */
}

int vq_take(VQueue *q, int gen, int clock_gen, int now_ms, int paused, int *ms)
{
    int took = -1;
    while (q->head != q->tail) {
        int h = q->head, slot = q->e[h].slot;
        if (q->e[h].gen == gen) {
            if (clock_gen != gen || paused || q->e[h].ms > now_ms)
                break;
            if (took >= 0) {
                q->busy[took] = 0;
                q->skipped++;
            }
            took = slot;
            *ms = q->e[h].ms;
        } else {
            q->busy[slot] = 0; /* from before a seek */
        }
        q->head = (h + 1) % VQ_RING;
    }
    return took;
}

void vq_release(VQueue *q, int slot)
{
    q->busy[slot] = 0;
}
