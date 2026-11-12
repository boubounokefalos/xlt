/* XLT - background job queue. A ring of SLOTS jobs with inline argument
 * copies; the counters are guarded by a semaphore used as a mutex. */
#include <pspkernel.h>
#include <string.h>
#include "bg.h"

#define SLOTS 4

typedef struct {
    void (*fn)(void *);
    int size;
    unsigned char arg[BG_ARG_MAX] __attribute__((aligned(8)));
} Job;

static Job s_jobs[SLOTS];
static int s_head, s_count;   /* guarded by s_lock */
static volatile int s_running; /* a job is executing right now */
static SceUID s_lock = -1, s_work = -1, s_thread = -1;

static void lock(void) { sceKernelWaitSema(s_lock, 1, NULL); }
static void unlock(void) { sceKernelSignalSema(s_lock, 1); }

static int worker(SceSize args, void *argp)
{
    for (;;) {
        sceKernelWaitSema(s_work, 1, NULL);
        lock();
        if (s_count == 0) {
            unlock();
            continue;
        }
        Job *j = &s_jobs[s_head];
        s_running = 1;
        unlock();

        j->fn(j->arg); /* the slot stays reserved until it is released below */

        lock();
        s_head = (s_head + 1) % SLOTS;
        s_count--;
        s_running = 0;
        unlock();
    }
    return 0;
}

void bg_init(void)
{
    s_lock = sceKernelCreateSema("xl_bglock", 0, 1, 1, NULL);
    s_work = sceKernelCreateSema("xl_bgwork", 0, 0, 1000, NULL);
    /* Below the UI (0x20) and the scanner (0x30): settings I/O can wait a frame. */
    s_thread = sceKernelCreateThread("xl_bg", worker, 0x32, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (s_lock < 0 || s_work < 0 || s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0)
        s_thread = -1;
}

void bg_post(void (*fn)(void *), const void *arg, int size)
{
    if (size < 0 || size > BG_ARG_MAX)
        return;
    if (s_thread < 0) {
        static unsigned char tmp[BG_ARG_MAX] __attribute__((aligned(8)));
        if (size) memcpy(tmp, arg, size);
        fn(tmp);
        return;
    }
    for (;;) {
        lock();
        if (s_count < SLOTS) {
            Job *j = &s_jobs[(s_head + s_count) % SLOTS];
            j->fn = fn;
            j->size = size;
            if (size) memcpy(j->arg, arg, size);
            s_count++;
            unlock();
            sceKernelSignalSema(s_work, 1);
            return;
        }
        unlock();
        sceKernelDelayThread(2000); /* full: rare, the jobs are small */
    }
}

void bg_flush(void)
{
    if (s_thread < 0)
        return;
    for (;;) {
        lock();
        int busy = s_count > 0 || s_running;
        unlock();
        if (!busy)
            return;
        sceKernelDelayThread(2000);
    }
}
