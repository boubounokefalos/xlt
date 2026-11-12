/* XLT - thumbnail cache.
 *
 *   thumbs_get()  main thread: LRU hit -> VRAM pointer; miss -> push request
 *   loader thread: newest request first; read <app>/cache/<key>.t16, or
 *                  decode ICON0 from the game and write that file
 *   thumbs_pump() main thread, between frames: copy finished textures into
 *                 the least recently used VRAM slot (the GPU is idle then)
 *
 * The .t16 files hold the texture exactly as the GPU reads it (swizzled
 * 144x80 ABGR1555), so a cached thumbnail is one read and one memcpy.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include "thumbs.h"
#include "thumbconv.h"
#include "gfx.h"
#include "bg.h"
#include "umd.h"

#define SLOTS 30
#define QUEUE 8
#define STAGING 3
#define FAILED 64
#define DONE_MAX 16
#define T16_MAGIC 0x36315458u /* "XT16" */

typedef struct {
    uint32_t key;
    char path[200];
    uint8_t is_disc, is_picture, is_music, is_video, is_umd;
    uint32_t icon_off, icon_size;
} Req;

typedef struct {
    uint32_t key;
    int staging; /* index into s_stage, or -1 if the load failed */
} Done;

/* Main thread only. */
static struct { uint32_t key; uint32_t used; } s_slot[SLOTS];
static uint32_t s_frame;
static uint32_t s_failed[FAILED];
static int s_failed_pos;

/* Shared, guarded by s_lock. */
static Req s_q[QUEUE];
static int s_qn;
static uint32_t s_inflight;
static Done s_done[DONE_MAX];
static int s_dn;
static int s_stage_busy[STAGING];
static uint8_t *s_stage[STAGING];

static SceUID s_lock = -1, s_work = -1, s_thread = -1;
static volatile int s_stop;
static char s_cache_dir[256];

static void lock(void) { sceKernelWaitSema(s_lock, 1, NULL); }
static void unlock(void) { sceKernelSignalSema(s_lock, 1); }

static uint8_t *slot_vram(int i, int uncached)
{
    u32 base = (u32)sceGeEdramGetAddr() + GFX_VRAM_FREE + (u32)i * THUMB_BYTES;
    return (uint8_t *)(uncached ? (base | 0x40000000u) : base);
}

/* ------------------------------------------------------------ loader side */

static int load_cached(uint32_t key, uint8_t *out)
{
    char path[280];
    snprintf(path, sizeof(path), "%s/%08X.t16", s_cache_dir, (unsigned)key);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    uint32_t h[2];
    int ok = sceIoRead(fd, h, 8) == 8 && h[0] == T16_MAGIC && h[1] == key &&
             sceIoRead(fd, out, THUMB_BYTES) == THUMB_BYTES;
    sceIoClose(fd);
    return ok ? 0 : -1;
}

static void store_cached(uint32_t key, const uint8_t *tex)
{
    char path[280];
    snprintf(path, sizeof(path), "%s/%08X.t16", s_cache_dir, (unsigned)key);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    uint32_t h[2] = {T16_MAGIC, key};
    sceIoWrite(fd, h, 8);
    sceIoWrite(fd, tex, THUMB_BYTES);
    sceIoClose(fd);
}

static int loader_thread(SceSize args, void *argp)
{
    while (!s_stop) {
        sceKernelWaitSema(s_work, 1, NULL);
        if (s_stop)
            break;

        /* Wait for a free staging buffer, then take the newest request. */
        int st = -1;
        Req r;
        for (;;) {
            lock();
            if (s_qn == 0) {
                unlock();
                break;
            }
            for (int i = 0; i < STAGING && s_dn < DONE_MAX; i++)
                if (!s_stage_busy[i]) { st = i; break; }
            if (st >= 0) {
                r = s_q[--s_qn];
                s_stage_busy[st] = 1;
                s_inflight = r.key;
                unlock();
                break;
            }
            unlock();
            sceKernelDelayThread(10000);
            if (s_stop)
                return 0;
        }
        if (st < 0)
            continue;

        int ok = load_cached(r.key, s_stage[st]) == 0;
        /* a video's thumbnail is a decoded frame, made by the video player (thumbs_put) */
        if (!ok && !r.is_video && (r.is_music     ? thumb_from_music(r.path, r.icon_off, s_stage[st])
                    : r.is_umd ? thumb_from_picture(UMD_ICON0, s_stage[st]) /* a plain PNG on the disc */
                    : r.is_picture ? thumb_from_picture(r.path, s_stage[st])
                                   : thumb_from_entry(r.path, r.is_disc, r.icon_off, r.icon_size, s_stage[st])) == 0) {
            store_cached(r.key, s_stage[st]);
            ok = 1;
        }

        lock();
        if (!ok)
            s_stage_busy[st] = 0;
        s_done[s_dn].key = r.key;
        s_done[s_dn].staging = ok ? st : -1;
        s_dn++; /* < DONE_MAX: checked before the request was taken */
        s_inflight = 0;
        unlock();
    }
    return 0;
}

/* -------------------------------------------------------------- main side */

int thumbs_pump(void)
{
    s_frame++;
    if (s_lock < 0)
        return 0;
    int changed = 0;
    lock();
    for (int d = 0; d < s_dn; d++) {
        Done *dn = &s_done[d];
        if (dn->staging < 0) {
            s_failed[s_failed_pos] = dn->key;
            s_failed_pos = (s_failed_pos + 1) % FAILED;
            changed = 1; /* redraw: a video without a cached frame then asks for one */
            continue;
        }
        int victim = -1;
        for (int i = 0; i < SLOTS && victim < 0; i++)
            if (s_slot[i].key == 0)
                victim = i;
        if (victim < 0) {
            victim = 0;
            for (int i = 1; i < SLOTS; i++)
                if (s_slot[i].used < s_slot[victim].used)
                    victim = i;
        }
        memcpy(slot_vram(victim, 1), s_stage[dn->staging], THUMB_BYTES);
        s_slot[victim].key = dn->key;
        s_slot[victim].used = s_frame;
        s_stage_busy[dn->staging] = 0;
        changed = 1;
    }
    int had_done = s_dn > 0;
    s_dn = 0;
    int pending = s_qn > 0;
    unlock();
    if (had_done && pending)
        sceKernelSignalSema(s_work, 1); /* staging freed: let the loader continue */
    return changed;
}

const void *thumbs_get(const Entry *e)
{
    if ((!e->icon_size && !kind_is_video(e->kind)) || s_lock < 0)
        return NULL;
    for (int i = 0; i < SLOTS; i++)
        if (s_slot[i].key == e->key) {
            s_slot[i].used = s_frame;
            return slot_vram(i, 0);
        }
    for (int i = 0; i < FAILED; i++)
        if (s_failed[i] == e->key)
            return NULL;

    lock();
    int known = s_inflight == e->key;
    for (int i = 0; i < s_qn && !known; i++)
        known = s_q[i].key == e->key;
    for (int i = 0; i < s_dn && !known; i++)
        known = s_done[i].key == e->key;
    if (!known) {
        if (s_qn == QUEUE) { /* drop the oldest; it has probably scrolled away */
            memmove(&s_q[0], &s_q[1], sizeof(Req) * (QUEUE - 1));
            s_qn--;
        }
        Req *r = &s_q[s_qn++];
        r->key = e->key;
        memcpy(r->path, e->path, sizeof(r->path));
        r->is_disc = kind_is_disc(e->kind);
        r->is_picture = kind_is_picture(e->kind);
        r->is_music = kind_is_music(e->kind);
        r->is_video = kind_is_video(e->kind);
        r->is_umd = kind_is_umd(e->kind);
        r->icon_off = e->icon_off;
        r->icon_size = e->icon_size;
    }
    unlock();
    if (!known)
        sceKernelSignalSema(s_work, 1);
    return NULL;
}

int thumbs_missing(const Entry *e)
{
    for (int i = 0; i < FAILED; i++)
        if (s_failed[i] == e->key)
            return 1;
    return 0;
}

typedef struct { uint32_t key; uint8_t *tex; char dir[256]; } PutArg;

static void job_store(void *arg)
{
    const PutArg *a = arg;
    char path[280];
    snprintf(path, sizeof(path), "%s/%08X.t16", a->dir, (unsigned)a->key);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        uint32_t h[2] = {T16_MAGIC, a->key};
        sceIoWrite(fd, h, 8);
        sceIoWrite(fd, a->tex, THUMB_BYTES);
        sceIoClose(fd);
    }
    free(a->tex);
}

void thumbs_put(uint32_t key, const uint8_t *tex)
{
    for (int i = 0; i < FAILED; i++) /* it exists now */
        if (s_failed[i] == key)
            s_failed[i] = 0;
    int victim = -1;
    for (int i = 0; i < SLOTS && victim < 0; i++)
        if (s_slot[i].key == key || s_slot[i].key == 0)
            victim = i;
    if (victim < 0) {
        victim = 0;
        for (int i = 1; i < SLOTS; i++)
            if (s_slot[i].used < s_slot[victim].used)
                victim = i;
    }
    memcpy(slot_vram(victim, 1), tex, THUMB_BYTES); /* between frames: the GPU is idle */
    s_slot[victim].key = key;
    s_slot[victim].used = s_frame;
    PutArg a = {key, malloc(THUMB_BYTES), ""};
    if (!a.tex)
        return;
    memcpy(a.tex, tex, THUMB_BYTES);
    snprintf(a.dir, sizeof(a.dir), "%s", s_cache_dir);
    bg_post(job_store, &a, sizeof(a));
}

void thumbs_init(const char *app_dir)
{
    s_stop = 0;
    s_qn = s_dn = 0;
    s_inflight = 0;
    memset(s_stage_busy, 0, sizeof(s_stage_busy));
    snprintf(s_cache_dir, sizeof(s_cache_dir), "%scache", app_dir);
    sceIoMkdir(s_cache_dir, 0777);
    for (int i = 0; i < STAGING; i++) {
        s_stage[i] = memalign(16, THUMB_BYTES);
        if (!s_stage[i])
            return;
    }
    s_lock = sceKernelCreateSema("xl_tlock", 0, 1, 1, NULL);
    s_work = sceKernelCreateSema("xl_twork", 0, 0, 1000, NULL);
    /* Lower priority than the UI (larger number) so loading never stalls a frame. */
    s_thread = sceKernelCreateThread("xl_thumbs", loader_thread, 0x38, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (s_lock < 0 || s_work < 0 || s_thread < 0 || sceKernelStartThread(s_thread, 0, NULL) < 0)
        s_lock = -1;
}

void thumbs_shutdown(void)
{
    if (s_thread >= 0) {
        s_stop = 1;
        sceKernelSignalSema(s_work, 1);
        SceUInt timeout = 2000000;
        sceKernelWaitThreadEnd(s_thread, &timeout);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_lock >= 0) sceKernelDeleteSema(s_lock);
    if (s_work >= 0) sceKernelDeleteSema(s_work);
    s_lock = s_work = -1;
    for (int i = 0; i < STAGING; i++) {
        free(s_stage[i]);
        s_stage[i] = NULL;
    }
    /* VRAM slots stay valid and are reused after thumbs_init(). */
}
