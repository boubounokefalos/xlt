/* XLT - one low-priority worker thread for small file jobs (settings reads
 * and writes), so the UI thread never waits on the Memory Stick. */
#ifndef BG_H
#define BG_H

#define BG_ARG_MAX (8 * 1024)

void bg_init(void);

/* Queues fn(copy of arg). Jobs run in order. If the queue is full, waits for
 * room; if there is no worker thread, runs fn right away. */
void bg_post(void (*fn)(void *arg), const void *arg, int size);

/* Waits until every queued job has finished (before a launch or power-off). */
void bg_flush(void);

#endif
