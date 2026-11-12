/* XLT - the UMD drive. See umd.h.
 *
 * As psp5's umd.c (tested on hardware) and ARK-5's arkMenu (umd.cpp): poll
 * sceUmdCheckMedium; when a disc appears, sceUmdActivate it as disc0: and wait
 * (in short steps, up to 20 s: some discs spin up slowly) for PSP_UMD_READY,
 * then read PSP_GAME/PARAM.SFO. A UMD Video has UMD_VIDEO/PARAM.SFO instead. */
/* Parts of this file (the disc-watcher thread) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspumd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "umd.h"
#include "sysctl.h"

#define GAME_SFO "disc0:/PSP_GAME/PARAM.SFO"
#define VIDEO_SFO "disc0:/UMD_VIDEO/PARAM.SFO"
#define SFO_MAX (64 * 1024)
#define POLL_US (300 * 1000)
#define READY_US (500 * 1000) /* waits in short steps, so umd_stop never takes long */
#define READY_TRIES 40        /* 20 s */

static SceUID s_thread = -1, s_lock = -1;
static volatile int s_quit;
static volatile int s_gen;   /* bumped on every change */
static int s_seen;           /* s_gen at the last umd_poll */
static int s_state_w;        /* written by the thread, under s_lock */
static Entry s_entry_w;
static int s_state;          /* the UI's copy */
static Entry s_entry;

static int file_size(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return n > 0 ? n : 0;
}

/* PARAM.SFO at path into e; 0 on success. */
static int read_sfo(const char *path, Entry *e, char *category)
{
    int size = file_size(path);
    if (!size || size > SFO_MAX)
        return -1;
    uint8_t *b = malloc(size);
    if (!b)
        return -1;
    int got = -1;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        got = sceIoRead(fd, b, size);
        sceIoClose(fd);
    }
    int r = got == size ? sfo_parse(b, size, e, category) : -1;
    free(b);
    return r;
}

/* The thumbnail and preview caches are named by key: one per disc. */
static uint32_t disc_key(const Entry *e)
{
    uint32_t h = 2166136261u;
    const char *parts[3] = {"UMD", e->id, e->title};
    for (int i = 0; i < 3; i++)
        for (const char *p = parts[i]; *p; p++)
            h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}

static int read_disc(Entry *e)
{
    char cat[4] = "";
    memset(e, 0, sizeof(*e));
    e->kind = KIND_UMD;
    if (read_sfo(GAME_SFO, e, cat) == 0) {
        snprintf(e->path, sizeof(e->path), "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
        e->icon_size = file_size(UMD_ICON0); /* thumbs.c reads it as a whole file */
        e->size = (uint64_t)file_size("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
        e->key = disc_key(e);
        return UMD_GAME;
    }
    memset(e, 0, sizeof(*e));
    e->kind = KIND_UMD;
    if (read_sfo(VIDEO_SFO, e, cat) == 0 || file_size(VIDEO_SFO)) {
        if (!e->title[0])
            snprintf(e->title, sizeof(e->title), "UMD Video");
        snprintf(e->path, sizeof(e->path), "disc0:/UMD_VIDEO");
        e->key = disc_key(e);
        return UMD_VIDEO;
    }
    return UMD_OTHER;
}

static int medium_present(void) { return sceUmdCheckMedium() > 0; }

static int drive_ready(void)
{
    sceUmdActivate(1, "disc0:");
    for (int t = 0; t < READY_TRIES && !s_quit && medium_present(); t++)
        if (sceUmdWaitDriveStatWithTimer(PSP_UMD_READY, READY_US) >= 0)
            break;
    return !s_quit && (sceUmdGetDriveStat() & PSP_UMD_READY);
}

static void publish(int state, const Entry *e)
{
    sceKernelWaitSema(s_lock, 1, NULL);
    s_state_w = state;
    if (e)
        s_entry_w = *e;
    s_gen++;
    sceKernelSignalSema(s_lock, 1);
}

static int umd_thread(SceSize args, void *argp)
{
    int state = UMD_NONE;
    while (!s_quit) {
        int present = medium_present();
        if (present && state == UMD_NONE) {
            publish(UMD_READING, NULL);
            static Entry e;
            state = drive_ready() ? read_disc(&e) : UMD_OTHER;
            if (s_quit)
                break;
            publish(state, &e);
        } else if (!present && state != UMD_NONE) {
            state = UMD_NONE;
            publish(UMD_NONE, NULL);
        }
        sceKernelDelayThread(POLL_US);
    }
    return 0;
}

void umd_start(void)
{
    if (s_thread >= 0 || sysctl_model() == 4)
        return; /* running already, or a PSP Go (no drive) */
    s_quit = 0;
    if (s_lock < 0)
        s_lock = sceKernelCreateSema("xl_umdlock", 0, 1, 1, NULL);
    if (s_lock < 0)
        return;
    s_thread = sceKernelCreateThread("xl_umd", umd_thread, 0x30, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread >= 0 && sceKernelStartThread(s_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
}

void umd_stop(void)
{
    if (s_thread < 0)
        return;
    s_quit = 1;
    SceUInt timeout = 2000000;
    sceKernelWaitThreadEnd(s_thread, &timeout);
    sceKernelDeleteThread(s_thread);
    s_thread = -1;
}

int umd_poll(void)
{
    if (s_lock < 0 || s_gen == s_seen)
        return 0;
    sceKernelWaitSema(s_lock, 1, NULL);
    s_seen = s_gen;
    s_state = s_state_w;
    if (s_state == UMD_GAME || s_state == UMD_VIDEO)
        s_entry = s_entry_w;
    sceKernelSignalSema(s_lock, 1);
    return 1;
}

int umd_state(void) { return s_state; }
const Entry *umd_entry(void) { return &s_entry; }
