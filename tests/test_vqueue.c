/* Host test: the queue of decoded pictures: order, timing, seeks, pausing,
 * and (run as a simulation) that no slot is ever lost or overwritten on screen. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/vqueue.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("OK   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)

static int count_free(const VQueue *q, int shown)
{
    int n = 0;
    for (int k = 0; k < q->nslots; k++)
        if (k != shown && !q->busy[k]) n++;
    return n;
}

int main(void)
{
    VQueue q;
    int ms = -1, s;
    vq_reset(&q, 5);
    CHECK(vq_empty(&q) && vq_free_slot(&q, -1, -1) == 0, "starts empty");

    vq_push(&q, 0, 1000, 1);
    vq_push(&q, 1, 1040, 1);
    CHECK(vq_free_slot(&q, -1, -1) == 2, "queued slots aren't free");
    CHECK(vq_take(&q, 1, 0, 5000, 0, &ms) < 0 && !vq_empty(&q), "nothing shows before the clock starts");
    CHECK(vq_take(&q, 1, 1, 999, 0, &ms) < 0, "not before its time");
    CHECK(vq_take(&q, 1, 1, 1000, 1, &ms) < 0 && !vq_empty(&q), "nothing shows while paused");
    s = vq_take(&q, 1, 1, 1000, 0, &ms);
    CHECK(s == 0 && ms == 1000, "due: taken");
    CHECK(q.busy[0] && vq_free_slot(&q, -1, -1) != 0, "taken but not released: still not free");
    vq_release(&q, s);
    CHECK(vq_free_slot(&q, 0, -1) != 0, "the one on screen isn't free");

    vq_push(&q, 2, 1080, 1);
    s = vq_take(&q, 1, 1, 2000, 0, &ms);
    CHECK(s == 2 && ms == 1080 && q.skipped == 1 && !q.busy[1], "several due: the newest, the others freed");
    vq_release(&q, s);

    vq_push(&q, 0, 1120, 1);
    vq_push(&q, 1, 1160, 1);
    vq_push(&q, 3, 900, 2);
    s = vq_take(&q, 2, 1, 5000, 0, &ms);
    CHECK(s < 0 && !q.busy[0] && !q.busy[1] && q.busy[3], "seek: old pictures go, the new one waits for its clock");
    s = vq_take(&q, 2, 2, 900, 0, &ms);
    CHECK(s == 3 && ms == 900, "after a backward seek the new picture shows");
    vq_release(&q, s);
    vq_push(&q, 0, 1, 2);
    vq_take(&q, 3, 2, 0, 1, &ms);
    CHECK(vq_empty(&q) && !q.busy[0], "stale pictures are dropped even while paused");

    /* decoder and screen running at odd rates with seeks and pauses */
    vq_reset(&q, 5);
    srand(11);
    int shown = -1, gen = 1, clock_gen = -1, out = 0, now = 0, last = -1, bad = 0, overwritten = 0, pending = 0;
    for (int tick = 0; tick < 2000000; tick++) {
        int paused = rand() % 500 == 0;
        if (rand() % 3) {
            int k = vq_free_slot(&q, shown, -1);
            if (k >= 0) {
                if (k == shown) overwritten++;
                vq_push(&q, k, out, gen);
                out += 40;
                if (++pending >= 3) clock_gen = gen;
            }
        }
        now += rand() % 30;
        s = vq_take(&q, gen, clock_gen, now, paused, &ms);
        if (s >= 0) {
            if (ms <= last) bad++;
            last = ms;
            shown = s;
            vq_release(&q, s);
        }
        if (now > out + 500) now = out;
        if (rand() % 4000 == 0) { gen++; clock_gen = -1; pending = 0; last = -1; now = out = rand() % 100000; }
        int queued = (q.tail - q.head + VQ_RING) % VQ_RING;
        if (queued + count_free(&q, shown) + (shown >= 0) < q.nslots) bad++;
    }
    CHECK(!bad, "simulation: pictures always in order, no slot ever lost");
    CHECK(!overwritten, "simulation: the picture on screen is never written to");
    printf(fails ? "FAILED\n" : "all good\n");
    return fails != 0;
}
