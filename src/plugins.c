/* XLT - PLUGINS.TXT file access for per-game plugins. */
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "plugins.h"
#include "ark.h"

#define FILES 3
#define MAX_FILE (32 * 1024)

/* ARK's processing order: its own folder, then ms0:/SEPLUGINS, then ef0:. */
int plugins_file_paths(char paths[FILES][160])
{
    int n = 0;
    if (ark_dir()[0] && strcasecmp(ark_dir(), "ms0:/SEPLUGINS/"))
        snprintf(paths[n++], 160, "%sPLUGINS.TXT", ark_dir());
    snprintf(paths[n++], 160, "ms0:/SEPLUGINS/PLUGINS.TXT");
    snprintf(paths[n++], 160, "ef0:/SEPLUGINS/PLUGINS.TXT");
    return n;
}

#define ERR_NOT_FOUND 0x80010002 /* SCE_ERROR_ERRNO_ENOENT */

char *plugins_read_file(const char *path, int *missing)
{
    *missing = 0;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        *missing = (unsigned)fd == ERR_NOT_FOUND;
        return NULL;
    }
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = NULL;
    if (size >= 0 && size <= MAX_FILE && (buf = malloc(size + 1))) {
        int n = sceIoRead(fd, buf, size);
        if (n != size) { /* only part of it: as good as unreadable */
            free(buf);
            buf = NULL;
        } else {
            buf[n] = 0;
        }
    }
    sceIoClose(fd);
    return buf;
}

char *plugins_read_text(const char *path)
{
    int missing;
    return plugins_read_file(path, &missing);
}

int plugins_write_text(const char *path, const char *text, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return -1;
    int r = sceIoWrite(fd, text, len) == len ? 0 : -1;
    sceIoClose(fd);
    return r;
}

/* 1 if the plugin's file is there (or where it is can't be told): a line for a
 * file that was deleted would only clutter the lists, so those are left out.
 * Nothing in PLUGINS.TXT changes. */
static int file_there(const char *path)
{
    char full[200];
    if (plugtext_resolve(path, ark_dir(), full, sizeof(full)) != 0)
        return 1;
    SceIoStat st;
    return sceIoGetstat(full, &st) >= 0 && !FIO_S_ISDIR(st.st_mode);
}

int plugins_supported(const Entry *e)
{
    return ark_version() && e->id[0] && !kind_is_homebrew(e->kind);
}

int plugins_for(const Entry *e, PlugState *out, int max)
{
    if (!plugins_supported(e))
        return 0;
    char paths[FILES][160];
    char *texts[FILES] = {0};
    int n = plugins_file_paths(paths);
    for (int i = 0; i < n; i++)
        texts[i] = plugins_read_text(paths[i]);
    int count = plugtext_list((const char *const *)texts, n, e->id, e->kind == KIND_PS1, out, max);
    for (int i = 0; i < n; i++)
        free(texts[i]);
    int k = 0;
    for (int i = 0; i < count; i++)
        if (file_there(out[i].path))
            out[k++] = out[i];
    return k;
}

int plugins_set(const Entry *e, const PlugState *p, int override)
{
    char paths[FILES][160];
    int n = plugins_file_paths(paths);
    if (!plugins_supported(e) || p->file < 0 || p->file >= n)
        return -1;
    char *text = plugins_read_text(paths[p->file]);
    int cap = (text ? strlen(text) : 0) + 512;
    char *out = malloc(cap);
    int r = -1;
    if (out) {
        int len = plugtext_set(text, e->id, p->path, override, out, cap);
        if (len >= 0) {
            SceUID fd = sceIoOpen(paths[p->file], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                r = sceIoWrite(fd, out, len) == len ? 0 : -1;
                sceIoClose(fd);
            }
        }
    }
    free(out);
    free(text);
    return r;
}

/* ------------------------------------------------ global lines (Settings) */

int plugins_global(PlugLine *out, int max)
{
    if (!ark_version())
        return 0;
    char paths[FILES][160];
    int n = plugins_file_paths(paths), count = 0;
    for (int i = 0; i < n && count < max; i++) {
        char *text = plugins_read_text(paths[i]);
        if (text)
            count += plugtext_lines(text, i, out + count, max - count);
        free(text);
    }
    int k = 0;
    for (int i = 0; i < count; i++)
        if (file_there(out[i].path))
            out[k++] = out[i]; /* file and line stay with each entry, so changing one still finds its line */
    return k;
}

int plugins_global_set(const PlugLine *p, const char *runlevel, int on)
{
    char paths[FILES][160];
    int n = plugins_file_paths(paths);
    if (!ark_version() || p->file < 0 || p->file >= n)
        return -1;
    char *text = plugins_read_text(paths[p->file]);
    if (!text)
        return -1;
    /* the file may have changed since it was listed: the line must still be this plugin */
    static PlugLine now[PLUG_MAX];
    int k = plugtext_lines(text, p->file, now, PLUG_MAX), same = 0;
    for (int i = 0; i < k; i++)
        if (now[i].line == p->line && !strcmp(now[i].path, p->path))
            same = 1;
    int cap = strlen(text) + 32, r = -1;
    char *out = same ? malloc(cap) : NULL;
    if (out) {
        int len = plugtext_set_line(text, p->line, runlevel, on, out, cap);
        if (len >= 0) {
            SceUID fd = sceIoOpen(paths[p->file], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                r = sceIoWrite(fd, out, len) == len ? 0 : -1;
                sceIoClose(fd);
            }
        }
    }
    free(out);
    free(text);
    return r;
}
