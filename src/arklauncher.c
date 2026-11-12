/* XLT - XLT as ARK's launcher. See arklauncher.h. */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <systemctrl_ark.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "arklauncher.h"
#include "ark.h"
#include "arkcfg.h"
#include "bg.h"
#include "library.h"

static int s_is_launcher;
static char s_dir[256]; /* XLT's data folder, with its EBOOT.PBP: the copy VBOOT.PBP follows */

static int exists(const char *p)
{
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

static int dir_exists(const char *p)
{
    SceUID d = sceIoDopen(p);
    if (d < 0) return 0;
    sceIoDclose(d);
    return 1;
}

static void ark_file(char *out, int n, const char *name) { snprintf(out, n, "%s%s", ark_dir(), name); }

void launcher_early(const char *self, char *dir, int cap)
{
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    if (!ark_version() || !ark_dir()[0] || !self)
        return;
    char vboot[200];
    ark_file(vboot, sizeof(vboot), "VBOOT.PBP");
    /* the path ARK gives may not spell ARK's folder as we do (ARK-5): any VBOOT.PBP is the launcher, as psp5 has it */
    const char *base = strrchr(self, '/');
    s_is_launcher = !strcasecmp(self, vboot) || (base && !strcasecmp(base + 1, "VBOOT.PBP"));
    if (!s_is_launcher)
        return;
    /* where the XLT this copy was made from lives (written when it was installed) */
    char note[200], home[256] = "";
    ark_file(note, sizeof(note), "VBOOT_XLT.TXT");
    SceUID fd = sceIoOpen(note, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, home, sizeof(home) - 1);
        sceIoClose(fd);
        home[n > 0 ? n : 0] = 0;
        home[strcspn(home, "\r\n")] = 0;
    }
    if (!home[0] || !dir_exists(home))
        snprintf(home, sizeof(home), "%.4s/PSP/GAME/XLT/", self);
    if (dir_exists(home)) {
        snprintf(dir, cap, "%s", home);
        snprintf(s_dir, sizeof(s_dir), "%s", home);
    }
}

int launcher_running(void) { return s_is_launcher; }

/* 1 if the PBP at path is XLT (its PARAM.SFO's TITLE) */
static int is_xlt(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    unsigned char h[40], sfo[1024];
    int ok = 0;
    if (sceIoRead(fd, h, 40) == 40 && !memcmp(h, "\0PBP", 4)) {
        unsigned off = h[8] | h[9] << 8 | h[10] << 16 | (unsigned)h[11] << 24;
        unsigned end = h[12] | h[13] << 8 | h[14] << 16 | (unsigned)h[15] << 24;
        int n = end > off && end - off < sizeof(sfo) ? (int)(end - off) : (int)sizeof(sfo);
        if (sceIoLseek32(fd, (int)off, PSP_SEEK_SET) == (int)off && sceIoRead(fd, sfo, n) == n) {
            /* the SFO's entries: the key table at kt, the values at dt */
            unsigned kt = sfo[8] | sfo[9] << 8, dt = sfo[12] | sfo[13] << 8, cnt = sfo[16] | sfo[17] << 8;
            for (unsigned k = 0; k < cnt && 20 + k * 16 + 16 <= (unsigned)n; k++) {
                const unsigned char *e = sfo + 20 + k * 16;
                unsigned ko = e[0] | e[1] << 8, len = e[4] | e[5] << 8, doff = e[12] | e[13] << 8;
                if (kt + ko + 6 <= (unsigned)n && !memcmp(sfo + kt + ko, "TITLE", 6) && dt + doff + len <= (unsigned)n)
                    ok = len >= 3 && !memcmp(sfo + dt + doff, "XLT", 4);
            }
        }
    }
    sceIoClose(fd);
    return ok;
}

/* 1 if two files hold the same bytes */
static int same_files(const char *a, const char *b)
{
    SceUID fa = sceIoOpen(a, PSP_O_RDONLY, 0), fb = sceIoOpen(b, PSP_O_RDONLY, 0);
    int same = fa >= 0 && fb >= 0 && sceIoLseek32(fa, 0, PSP_SEEK_END) == sceIoLseek32(fb, 0, PSP_SEEK_END);
    if (same) {
        sceIoLseek32(fa, 0, PSP_SEEK_SET);
        sceIoLseek32(fb, 0, PSP_SEEK_SET);
        unsigned char *x = malloc(32 * 1024), *y = malloc(32 * 1024);
        same = x && y;
        for (int n; same && (n = sceIoRead(fa, x, 32 * 1024)) > 0;)
            same = sceIoRead(fb, y, n) == n && !memcmp(x, y, n);
        free(x);
        free(y);
    }
    if (fa >= 0) sceIoClose(fa);
    if (fb >= 0) sceIoClose(fb);
    return same;
}

/* copies src to dst through dst.tmp, so a cut-off copy never becomes the launcher */
static int copy_file(const char *src, const char *dst)
{
    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    SceUID in = sceIoOpen(src, PSP_O_RDONLY, 0), out = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    unsigned char *buf = malloc(64 * 1024);
    int ok = in >= 0 && out >= 0 && buf;
    for (int n; ok && (n = sceIoRead(in, buf, 64 * 1024)) > 0;)
        ok = sceIoWrite(out, buf, n) == n;
    free(buf);
    if (in >= 0) sceIoClose(in);
    if (out >= 0) sceIoClose(out);
    if (ok) {
        sceIoRemove(dst);
        ok = sceIoRename(tmp, dst) >= 0;
    }
    if (!ok) sceIoRemove(tmp);
    return ok;
}

/* "size y-m-d h:m:s.us" of a file: what VBOOT_XLT.TXT remembers of the XLT it copied, so a
 * start only looks at two file dates instead of reading both 4 MB files (which, on the
 * background worker during the boot animation, made it stutter) */
static int file_sig(const char *path, char *out, int n)
{
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0)
        return -1;
    const ScePspDateTime *t = &st.sce_st_mtime;
    snprintf(out, n, "%lld %d-%d-%d %d:%d:%d.%u", (long long)st.st_size, t->year, t->month, t->day, t->hour, t->minute,
             t->second, (unsigned)t->microsecond);
    return 0;
}

/* line 2 of VBOOT_XLT.TXT */
static void noted_sig(const char *note, char *out, int n)
{
    out[0] = 0;
    char text[400];
    SceUID fd = sceIoOpen(note, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    int k = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    text[k > 0 ? k : 0] = 0;
    char *nl = strchr(text, '\n');
    if (nl) {
        snprintf(out, n, "%s", nl + 1);
        out[strcspn(out, "\r\n")] = 0;
    }
}

static void paused_path(char *out, int n) { snprintf(out, n, "%scache/launcher.paused", s_dir); }

static int set_launcher(int on)
{
    static ArkCfg c;
    char *extra = malloc(16 * 1024);
    int r = -1;
    if (extra && ark_load_cfg(&c, extra, 16 * 1024) == 0) {
        c.value[AK_LAUNCHER] = on ? 1 : 0;
        r = ark_save_cfg(&c, extra);
    }
    free(extra);
    return r;
}

void launcher_sync_now(void)
{
    if (!ark_version() || !ark_dir()[0])
        return;
    char note[320], paused[300];
    /* back from the XMB after a pause: the launcher on again */
    paused_path(paused, sizeof(paused));
    if (exists(paused) && set_launcher(1) == 0) {
        sceIoRemove(paused);
        library_note("ark launcher: switched back on (it was paused to leave to the XMB)");
    }
    static ArkCfg c;
    char *extra = malloc(16 * 1024);
    int ok = extra && ark_load_cfg(&c, extra, 16 * 1024) == 0, on = ok && c.value[AK_LAUNCHER] > 0;
    free(extra);
    if (!ok)
        return;
    char vboot[200], menu[200], master[300], home[200];
    ark_file(vboot, sizeof(vboot), "VBOOT.PBP");
    ark_file(menu, sizeof(menu), "VBOOT_ARKMENU.PBP");
    ark_file(home, sizeof(home), "VBOOT_XLT.TXT");
    snprintf(master, sizeof(master), "%sEBOOT.PBP", s_dir);
    int have = exists(vboot), mine = have && is_xlt(vboot);
    if (on) {
        if (have && !mine) { /* ARK's menu (or another launcher): kept, not lost */
            const char *keep = exists(menu) ? "VBOOT_OLD.PBP" : "VBOOT_ARKMENU.PBP";
            char to[200];
            ark_file(to, sizeof(to), keep);
            sceIoRemove(exists(menu) ? to : menu);
            if (sceIoRename(vboot, to) < 0) {
                library_note("ark launcher: COULDN'T move ARK's VBOOT.PBP aside, so XLT isn't the launcher");
                return;
            }
            have = mine = 0;
            snprintf(note, sizeof(note), "ark launcher: ARK's menu kept as %s", keep);
            library_note(note);
        }
        char sig[96], was[96], vsig[96];
        if (file_sig(master, sig, sizeof(sig)) != 0)
            return; /* nothing to copy from (an XLT started from elsewhere) */
        noted_sig(home, was, sizeof(was));
        /* the copy is the XLT it was made from, unchanged since, and still the same size: done */
        if (mine && !strcmp(sig, was) && file_sig(vboot, vsig, sizeof(vsig)) == 0 && atoll(vsig) == atoll(sig))
            return;
        if (!mine || !same_files(master, vboot)) {
            int done = copy_file(master, vboot);
            snprintf(note, sizeof(note), "ark launcher: %s %.120s from %.150s", done ? "XLT is now" : "COULDN'T make XLT", vboot, master);
            library_note(note);
            if (!done)
                return;
        }
        SceUID fd = sceIoOpen(home, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            char text[400];
            int n = snprintf(text, sizeof(text), "%s\n%s\n", s_dir, sig);
            sceIoWrite(fd, text, n);
            sceIoClose(fd);
        }
    } else if (mine && exists(menu)) { /* off: ARK's own menu back */
        sceIoRemove(vboot);
        int back = sceIoRename(menu, vboot) >= 0;
        sceIoRemove(home);
        library_note(back ? "ark launcher: ARK's menu put back" : "ark launcher: COULDN'T put ARK's menu back");
    }
}

static void job_sync(void *arg) { (void)arg; launcher_sync_now(); }

void launcher_sync(const char *dir)
{
    if (!s_is_launcher)
        snprintf(s_dir, sizeof(s_dir), "%s", dir);
    bg_post(job_sync, NULL, 0);
}

void launcher_before_exit(void)
{
    if (!s_is_launcher || !ark_version())
        return;
    /* ARK would start the launcher again: off until XLT starts next */
    if (set_launcher(0) == 0) {
        char paused[300];
        paused_path(paused, sizeof(paused));
        SceUID fd = sceIoOpen(paused, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }
    /* ARK also keeps the setting in memory, and that is what it looks at when XLT ends: ARK-4
     * too (it doesn't read SETTINGS.TXT again then). Only touched when the layout is surely
     * right: ARK's magic number, and the launcher byte set, as it must be while XLT is the launcher. */
    SEConfigARK se;
    memset(&se, 0, sizeof(se));
    char note[96];
    int r = sctrlSEGetConfigEx((SEConfig *)&se, sizeof(se));
    if (r >= 0 && se.magic == ARK_CONFIG_MAGIC && se.launcher_mode == 1) {
        se.launcher_mode = 0;
        r = sctrlSESetConfigEx((SEConfig *)&se, sizeof(se));
        snprintf(note, sizeof(note), "ark launcher: paused to leave to the XMB (in memory: %08x)", (unsigned)r);
    } else {
        snprintf(note, sizeof(note), "ark launcher: paused in SETTINGS.TXT only (config %08x, magic %08x, launcher %d)",
                 (unsigned)r, (unsigned)se.magic, se.launcher_mode);
    }
    library_note(note);
}
