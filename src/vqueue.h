/* Pictures decoded ahead of time, waiting for their moment on screen.
 *
 * The video thread pushes (one writer), the UI thread takes (one reader). Each
 * picture carries its seek generation: after a seek, pictures of the old
 * position are thrown away, and new ones wait until the clock has been started
 * for their generation. */
#ifndef VQUEUE_H
#define VQUEUE_H

#define VQ_MAX_SLOTS 8
#define VQ_RING (VQ_MAX_SLOTS + 1)

typedef struct {
    volatile int busy[VQ_MAX_SLOTS]; /* slot is queued, or just taken and not yet released */
    volatile struct { int slot, ms, gen; } e[VQ_RING]; /* volatile: stay written before tail moves */
    volatile int head, tail;
    int nslots;
    int skipped;                     /* due together: only the newest was shown */
} VQueue;

void vq_reset(VQueue *q, int nslots);
int vq_empty(const VQueue *q);

/* A slot neither on screen, published nor queued; -1 if there is none right now. */
int vq_free_slot(const VQueue *q, int shown, int published);

/* The caller must own a free slot; the ring can't overflow, having more entries than slots. */
void vq_push(VQueue *q, int slot, int ms, int gen);

/* The newest picture due by now_ms, or -1. Stale generations are always dropped;
 * nothing is shown while paused or before the clock starts for generation gen.
 * The returned slot stays busy until vq_release: put it on screen first, so the
 * decoder can't pick it in between. */
int vq_take(VQueue *q, int gen, int clock_gen, int now_ms, int paused, int *ms);
void vq_release(VQueue *q, int slot);

#endif
