/* XLT - installs the in-game screenshot plugin (plugin/xltshot). See shotplug.h. */
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shotplug.h"
#include "plugins.h"
#include "plugtext.h"
#include "ark.h"
#include "bg.h"
#include "library.h"
#include "xltshot_data.h" /* the plugin, built by plugin/xltshot/Makefile */

#define NAME "xltshot.prx"

static volatile int s_msg_ready;
static char s_msg[96];

/* 1 if the file at path already holds exactly the plugin XLT carries */
static int same_file(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    int same = sceIoLseek32(fd, 0, PSP_SEEK_END) == (int)sizeof(xltshot_prx);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    static unsigned char buf[4096];
    for (int at = 0; same && at < (int)sizeof(xltshot_prx); at += sizeof(buf)) {
        int n = sizeof(xltshot_prx) - at < sizeof(buf) ? (int)sizeof(xltshot_prx) - at : (int)sizeof(buf);
        same = sceIoRead(fd, buf, n) == n && !memcmp(buf, xltshot_prx + at, n);
    }
    sceIoClose(fd);
    return same;
}

static void job_install(void *arg)
{
    (void)arg;
    if (!ark_version())
        return;
    /* where it goes: the Memory Stick, or a PSP Go's internal storage without one */
    const char *dev = "ms0:";
    SceUID d = sceIoDopen("ms0:/");
    if (d >= 0) sceIoDclose(d);
    else dev = "ef0:";
    char dir[32], prx[64], note[260];
    snprintf(dir, sizeof(dir), "%s/SEPLUGINS", dev);
    snprintf(prx, sizeof(prx), "%s/" NAME, dir);
    sceIoMkdir(dir, 0777);

    /* what the PSP lets XLT open, for the log (a fresh start once found nothing it could read) */
    {
        SceUID f1 = sceIoOpen("ms0:/SEPLUGINS/PLUGINS.TXT", PSP_O_RDONLY, 0);
        if (f1 >= 0) sceIoClose(f1);
        SceUID d1 = sceIoDopen("ms0:/MUSIC");
        int first = -1;
        char song[200] = "";
        if (d1 >= 0) {
            SceIoDirent e;
            memset(&e, 0, sizeof(e));
            while (sceIoDread(d1, &e) > 0) {
                if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
                    snprintf(song, sizeof(song), "ms0:/MUSIC/%.180s", e.d_name);
                    SceUID f2 = sceIoOpen(song, PSP_O_RDONLY, 0);
                    first = f2;
                    if (f2 >= 0) sceIoClose(f2);
                    break;
                }
                memset(&e, 0, sizeof(e));
            }
            sceIoDclose(d1);
        }
        snprintf(note, sizeof(note), "io check: open SEPLUGINS/PLUGINS.TXT %08x, dopen MUSIC %08x, open first song %08x (%.60s)",
                 (unsigned)f1, (unsigned)d1, (unsigned)first, song[0] ? song + 11 : "none");
        library_note(note);
    }

    /* the file: written when missing or not the one this XLT carries (an update) */
    int updated = 0;
    if (!same_file(prx)) {
        SceUID fd = sceIoOpen(prx, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        int ok = fd >= 0 && sceIoWrite(fd, xltshot_prx, sizeof(xltshot_prx)) == (int)sizeof(xltshot_prx);
        if (fd >= 0) sceIoClose(fd);
        if (!ok) {
            sceIoRemove(prx);
            snprintf(note, sizeof(note), "screenshot plugin: can't write %s", prx);
            library_note(note);
            return;
        }
        updated = 1;
    }

    /* the lists: once a line for it exists (on or off), it is the user's: left alone.
     * A list that is there but can't be read leaves them all alone: it might mention it,
     * and writing one we couldn't read would lose what is in it. */
    char paths[3][160];
    int n = plugins_file_paths(paths), listed = 0;
    for (int i = 0; i < n && !listed; i++) {
        int missing;
        char *t = plugins_read_file(paths[i], &missing);
        if (t) listed = plugtext_mentions(t, NAME);
        else if (!missing) {
            snprintf(note, sizeof(note), "screenshot plugin: can't read %.160s, so the lists are left alone", paths[i]);
            library_note(note);
            return;
        }
        free(t);
    }
    if (listed) {
        if (updated) {
            snprintf(note, sizeof(note), "screenshot plugin: %s updated", prx);
            library_note(note);
        }
        return;
    }

    /* first time: other screenshot plugins off (in games), then ours on */
    char off[160] = "";
    for (int i = 0; i < n; i++) {
        char *t = plugins_read_text(paths[i]);
        if (!t) continue;
        int cap = (int)strlen(t) + 64;
        char *o = malloc(cap), names[120];
        int k = o ? plugtext_off_screenshots(t, o, cap, names, sizeof(names)) : -1;
        if (k > 0 && plugins_write_text(paths[i], o, (int)strlen(o)) == 0) {
            int used = (int)strlen(off);
            snprintf(off + used, sizeof(off) - used, "%s%s", used ? ", " : "", names);
        }
        free(o);
        free(t);
    }
    char list[64], line[96];
    snprintf(list, sizeof(list), "%s/PLUGINS.TXT", dir);
    snprintf(line, sizeof(line), "game, %s, on", prx);
    int missing;
    char *t = plugins_read_file(list, &missing);
    if (!t && !missing) { /* checked above already, but it may have gone wrong since */
        snprintf(note, sizeof(note), "screenshot plugin: can't read %s, left alone", list);
        library_note(note);
        return;
    }
    const char *old = t ? t : "";
    int cap = (int)strlen(old) + (int)strlen(line) + 8;
    char *o = malloc(cap);
    int len = o ? plugtext_append(old, line, o, cap) : -1;
    int ok = len >= 0 && plugins_write_text(list, o, len) == 0;
    free(o);
    free(t);
    snprintf(note, sizeof(note), "screenshot plugin: %s %s%s%s", ok ? "added to" : "COULDN'T add to", list,
             off[0] ? "; turned off " : "", off);
    library_note(note);
    if (ok) {
        snprintf(s_msg, sizeof(s_msg), off[0] ? "Screenshots in games: L+R+START (%.40s turned off)"
                                              : "Screenshots in games: L+R+START%.0s", off);
        s_msg_ready = 1;
    }
}

void shotplug_install(void) { bg_post(job_install, NULL, 0); }

int shotplug_take_message(char *out, int n)
{
    if (!s_msg_ready)
        return 0;
    s_msg_ready = 0;
    snprintf(out, n, "%s", s_msg);
    return 1;
}
