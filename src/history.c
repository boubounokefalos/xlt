/* XLT - play history. At most HISTORY_MAX entries, oldest dropped first. */
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include "history.h"
#include "entry.h"

#define HISTORY_MAX 128
#define HISTORY_MAGIC 0x31484C58u /* "XLH1" */

typedef struct {
    char path[sizeof(((Entry *)0)->path)];
    uint64_t played;
    uint32_t count;
    uint32_t pad;
} Record;

static Record s_rec[HISTORY_MAX];
static int s_n;
static char s_file[256];

void history_init(const char *app_dir)
{
    snprintf(s_file, sizeof(s_file), "%.200scache/history.bin", app_dir);
    s_n = 0;
    SceUID fd = sceIoOpen(s_file, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    uint32_t h[3];
    if (sceIoRead(fd, h, sizeof(h)) == sizeof(h) && h[0] == HISTORY_MAGIC && h[1] == sizeof(Record) &&
        h[2] <= HISTORY_MAX) {
        int bytes = (int)(h[2] * sizeof(Record));
        if (sceIoRead(fd, s_rec, bytes) == bytes) {
            s_n = (int)h[2];
            for (int i = 0; i < s_n; i++)
                s_rec[i].path[sizeof(s_rec[i].path) - 1] = 0;
        }
    }
    sceIoClose(fd);
}

void history_clear(void)
{
    s_n = 0;
    sceIoRemove(s_file);
}

int history_count(void) { return s_n; }

static Record *find(const char *path)
{
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_rec[i].path, path))
            return &s_rec[i];
    return NULL;
}

uint64_t history_played(const char *path)
{
    Record *r = find(path);
    return r ? r->played : 0;
}

void history_record(const char *path)
{
    u64 now;
    sceRtcGetCurrentTick(&now);
    Record *r = find(path);
    if (!r) {
        if (s_n == HISTORY_MAX) { /* drop the least recently played */
            int old = 0;
            for (int i = 1; i < s_n; i++)
                if (s_rec[i].played < s_rec[old].played)
                    old = i;
            s_rec[old] = s_rec[--s_n];
        }
        r = &s_rec[s_n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->path, sizeof(r->path), "%s", path);
    }
    r->played = now;
    r->count++;

    SceUID fd = sceIoOpen(s_file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return;
    uint32_t h[3] = {HISTORY_MAGIC, sizeof(Record), (uint32_t)s_n};
    sceIoWrite(fd, h, sizeof(h));
    sceIoWrite(fd, s_rec, s_n * sizeof(Record));
    sceIoClose(fd);
}

void history_describe(const char *path, char *out, int n)
{
    uint64_t t = history_played(path);
    if (!t) {
        snprintf(out, n, "Never played");
        return;
    }
    u64 now;
    sceRtcGetCurrentTick(&now);
    uint32_t days = now > t ? (uint32_t)((now - t) / (86400ull * 1000000ull)) : 0;
    if (days == 0) snprintf(out, n, "Played today");
    else if (days == 1) snprintf(out, n, "Played yesterday");
    else if (days < 14) snprintf(out, n, "Played %u days ago", (unsigned)days);
    else if (days < 60) snprintf(out, n, "Played %u weeks ago", (unsigned)(days / 7));
    else snprintf(out, n, "Played %u months ago", (unsigned)(days / 30));
}
