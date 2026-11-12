/* XLT - previews. See preview.h.
 *
 * Games: ICON1.PMF (the XMB's animated icon) is read from the disc image
 * (PSP_GAME/ICON1.PMF; CSO and ZSO through disc.c) or from EBOOT.PBP's third
 * section, on the background worker, then played from memory by the video
 * player's PMF path, looping. Games without one keep their ICON0; they are
 * remembered so their file isn't opened again.
 * Videos: the player's own MP4 / PMF path, muted, from 10% in. An MP4 needs
 * the ME booted the player's way, which would stop music, so MP4s aren't
 * previewed while music plays (PMFs are).
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "preview.h"
#include "bg.h"
#include "disc.h"
#include "music.h"
#include "thumbconv.h"
#include "thumbs.h"
#include "video.h"
#include "umd.h"

#define REST_FRAMES 36 /* about 0.6 s, close to the XMB's delay */
#define GRAB_WAIT 180  /* at most ~3 s of still frames for other tiles before the live preview */
#define ICON1_MAX (3 << 20)
#define NO_ICON1 64
#define GRAB_Q 8
#define GRAB_FAILED 32

static struct {
    int active;
    char path[200];
    uint32_t key;
    int kind;
    int rest, requested;
    int waited; /* frames the live preview has waited for still frames */
    int gen;
    uint32_t none[NO_ICON1]; /* keys of games without an ICON1 */
    int none_n, none_w;
} T;

/* Videos waiting for a still frame: the one drawn last is grabbed first. */
static struct {
    struct { uint32_t key; char path[200]; } q[GRAB_Q];
    int n;
    int busy;
    uint32_t busy_key;
    uint32_t failed[GRAB_FAILED];
    int failed_w;
    uint8_t tex[THUMB_BYTES] __attribute__((aligned(16)));
} G;

static int is_pmf(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".pmf");
}

/* An MP4 needs the ME booted the player's way, which music can't share. */
static int can_decode(const char *path) { return is_pmf(path) || music_state() == MUSIC_STOPPED; }

void preview_want_thumb(const Entry *e)
{
    if (!e || !kind_is_video(e->kind) || !thumbs_missing(e) || (G.busy && G.busy_key == e->key))
        return;
    for (int i = 0; i < GRAB_FAILED; i++)
        if (G.failed[i] == e->key)
            return;
    for (int i = 0; i < G.n; i++)
        if (G.q[i].key == e->key)
            return;
    if (G.n == GRAB_Q) { /* the oldest has probably scrolled away */
        memmove(&G.q[0], &G.q[1], sizeof(G.q[0]) * (GRAB_Q - 1));
        G.n--;
    }
    G.q[G.n].key = e->key;
    snprintf(G.q[G.n].path, sizeof(G.q[G.n].path), "%s", e->path);
    G.n++;
}

/* The newest queued video that can be decoded now, or -1. */
static int grab_ready(void)
{
    for (int i = G.n - 1; i >= 0; i--)
        if (can_decode(G.q[i].path))
            return i;
    return -1;
}

static int grab_step(void)
{
    if (G.busy) {
        int r = video_grab_take(G.tex);
        if (r == 1) {
            thumbs_put(G.busy_key, G.tex);
            G.busy = 0;
            return 1;
        }
        if (r < 0 || video_preview_idle()) { /* failed, or stopped for music or the player */
            if (r < 0)
                G.failed[G.failed_w++ % GRAB_FAILED] = G.busy_key;
            G.busy = 0;
        }
        return 0;
    }
    int live = T.active && T.requested && video_preview_path();
    int i = grab_ready();
    if (live || i < 0 || !video_preview_idle())
        return 0;
    char path[200];
    uint32_t key = G.q[i].key;
    snprintf(path, sizeof(path), "%s", G.q[i].path);
    memmove(&G.q[i], &G.q[i + 1], sizeof(G.q[0]) * (G.n - i - 1));
    G.n--;
    if (video_grab(path) == 0) {
        G.busy = 1;
        G.busy_key = key;
    }
    return 0;
}

typedef struct {
    int gen, pbp;
    char path[200];
} Icon1Arg;

static volatile int s_ready;
static int s_gen;
static uint8_t *s_data;
static uint32_t s_len;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void job_icon1(void *arg)
{
    const Icon1Arg *a = arg;
    uint8_t *data = NULL;
    uint32_t len = 0;
    if (a->pbp == 2) { /* the UMD: a plain file on the disc */
        SceUID fd = sceIoOpen(UMD_ICON1, PSP_O_RDONLY, 0);
        if (fd >= 0) {
            int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
            if (n >= 2048 && n <= ICON1_MAX && (data = malloc(n)) != NULL) {
                len = n;
                if (sceIoLseek32(fd, 0, PSP_SEEK_SET) != 0 || sceIoRead(fd, data, n) != n) {
                    free(data);
                    data = NULL;
                }
            }
            sceIoClose(fd);
        }
    } else if (a->pbp) {
        SceUID fd = sceIoOpen(a->path, PSP_O_RDONLY, 0);
        uint8_t h[40];
        if (fd >= 0) {
            if (sceIoRead(fd, h, 40) == 40 && !memcmp(h, "\0PBP", 4)) {
                uint32_t icon1 = rd32(h + 16), pic0 = rd32(h + 20);
                if (pic0 > icon1 && pic0 - icon1 >= 2048 && pic0 - icon1 <= ICON1_MAX) {
                    len = pic0 - icon1;
                    data = malloc(len);
                    if (!data || sceIoLseek32(fd, icon1, PSP_SEEK_SET) != (int)icon1 ||
                        sceIoRead(fd, data, len) != (int)len) {
                        free(data);
                        data = NULL;
                    }
                }
            }
            sceIoClose(fd);
        }
    } else {
        Disc d;
        if (disc_open(&d, a->path) == 0) {
            uint32_t off, size;
            if (disc_find(&d, "PSP_GAME/ICON1.PMF", &off, &size) == 0 && size >= 2048 && size <= ICON1_MAX) {
                data = malloc(size);
                len = size;
                if (data && disc_read(&d, data, size, off) != 0) {
                    free(data);
                    data = NULL;
                }
            }
            disc_close(&d);
        }
    }
    if (data && memcmp(data, "PSMF", 4)) { /* not a PMF after all */
        free(data);
        data = NULL;
    }
    while (s_ready) /* the UI takes a result every frame */
        sceKernelDelayThread(5000);
    s_data = data;
    s_len = data ? len : 0;
    s_gen = a->gen;
    __sync_synchronize();
    s_ready = 1;
}

static int has_none(uint32_t key)
{
    for (int i = 0; i < T.none_n; i++)
        if (T.none[i] == key)
            return 1;
    return 0;
}

void preview_target(const Entry *e)
{
    int same = e && T.active && T.key == e->key && !strcmp(T.path, e->path);
    if (same || (!e && !T.active))
        return;
    if (T.active && T.requested)
        video_preview_stop(0);
    T.active = e != NULL;
    T.requested = 0;
    T.gen++;
    if (!e)
        return;
    snprintf(T.path, sizeof(T.path), "%s", e->path);
    T.key = e->key;
    T.kind = e->kind;
    T.rest = REST_FRAMES;
    T.waited = 0;
}

/* Test switch: with cache/nopreview on the stick, no live previews (and so no Media Engine
 * boot or MPEG modules): to see whether they are what stops Wi-Fi from starting. */
static int s_nopreview = -1;

static void start(void)
{
    if (s_nopreview < 0) {
        SceUID fd = sceIoOpen("ms0:/PSP/GAME/XLT/cache/nopreview", PSP_O_RDONLY, 0);
        s_nopreview = fd >= 0;
        if (fd >= 0) sceIoClose(fd);
        if (s_nopreview) video_note("previews off: cache/nopreview exists");
    }
    T.requested = 1;
    if (s_nopreview)
        return;
    if (kind_is_video(T.kind)) {
        if (!can_decode(T.path)) {
            video_note("preview skipped: music is playing");
            return;
        }
        video_preview_file(T.path);
        return;
    }
    if (kind_is_picture(T.kind) || kind_is_music(T.kind) || has_none(T.key))
        return;
    static Icon1Arg a;
    a.gen = T.gen;
    a.pbp = kind_is_umd(T.kind) ? 2 : !kind_is_disc(T.kind);
    snprintf(a.path, sizeof(a.path), "%s", T.path);
    bg_post(job_icon1, &a, sizeof(a));
}

int preview_step(void)
{
    int dirty = grab_step();
    if (T.active && !T.requested && T.rest > 0 && --T.rest == 0) {
        /* The still frames on screen first: they're quick, and the decoder does one thing at
         * a time. A grid shows many videos, so that wait is capped; a grab already running
         * always finishes (stopping it would mark its video as failed). */
        if (G.busy || (grab_ready() >= 0 && T.waited++ < GRAB_WAIT)) {
            T.rest = 1;
        } else {
            if (T.waited > GRAB_WAIT)
                video_note("preview: stopped waiting for still frames");
            start();
        }
    }
    if (s_ready && !G.busy) { /* an icon waits for a running grab */
        uint8_t *data = s_data;
        uint32_t len = s_len;
        int gen = s_gen;
        __sync_synchronize();
        s_ready = 0;
        if (gen == T.gen && T.active) {
            if (data) {
                video_preview_pmf(T.path, data, len); /* the player owns it now */
                data = NULL;
            } else {
                T.none[T.none_w++ % NO_ICON1] = T.key; /* full: the oldest goes */
                if (T.none_n < NO_ICON1) T.none_n++;
            }
        }
        free(data);
    }
    return video_preview_step() | dirty;
}

int preview_draw(const Entry *e, int x, int y, int w, int h)
{
    const char *p = video_preview_path();
    if (!e || !p || !T.active || strcmp(p, e->path) || strcmp(T.path, e->path))
        return 0;
    int ok = video_preview_draw(x, y, w, h);
    static int logged_gen = -1;
    if (ok && logged_gen != T.gen) { /* once per preview: the screen really shows it */
        logged_gen = T.gen;
        video_note("preview drawn in a %dx%d box", w, h);
    }
    return ok;
}
