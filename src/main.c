/* XLT - a light, XMB-logic launcher for ARK custom firmware.
 *
 * Main loop: poll the pad every vblank; draw only when ui_update() or the
 * status bar says something changed. While idle the GPU does nothing and the
 * CPU just waits for the next vblank.
 */
/* Parts of this file (the exit/power callbacks) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include "gfx.h"
#include "sfx.h"
#include "theme.h"
#include "input.h"
#include "osd.h"
#include "psreg.h"
#include "system.h"
#include "ui.h"
#include "library.h"
#include "thumbs.h"
#include "history.h"
#include "launch.h"
#include "ark.h"
#include "cc.h"
#include "gamecfg.h"
#include "sysctl.h"
#include "bg.h"
#include "music.h"
#include "viewer.h"
#include "video.h"
#include "fm.h"
#include "fileops.h"
#include "reader.h"
#include "web.h"
#include "webfetch.h"
#include "net.h"
#include "saver.h"
#include "arkcfg.h"
#include "boot.h"
#include "umd.h"
#include "safestart.h"
#include "recorder.h"
#include "shotplug.h"
#include "arklauncher.h"
#include "version.h"
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("XLT", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
/* A fixed heap, as PSPDX has. "All but N" (negative sizes, or a threshold) takes the low
 * memory on a PSP with the large memory (2000 and later; PARAM.SFO MEMSIZE=1, see the
 * Makefile), and the firmware's network modules then load above the old 32 MB line, where
 * loading NET_INET hangs the PSP. With only the 24 MB for applications (a PSP-1000, or ARK's
 * high memory off: ARK then ignores MEMSIZE=1) XLT (~7 MB) and 18 MB don't fit, so main()
 * makes the heap smaller before the first allocation (the SDK makes it then, and tries again
 * after a failure): see fit_heap(). */
#define HEAP_KB (18 * 1024)
#define HEAP_RESERVE_KB (3 * 1024) /* outside the heap: thread stacks, the network and video modules */
PSP_HEAP_SIZE_KB(HEAP_KB);
extern int sce_newlib_heap_kb_size;
static int s_heap_free_kb; /* the largest free block before the heap, for the log */

static void fit_heap(void)
{
    s_heap_free_kb = (int)(sceKernelMaxFreeMemSize() / 1024);
    if (s_heap_free_kb - HEAP_RESERVE_KB < HEAP_KB)
        sce_newlib_heap_kb_size = s_heap_free_kb - HEAP_RESERVE_KB;
}

static volatile int s_running = 1;

static int exit_callback(int arg1, int arg2, void *common)
{
    s_running = 0;
    return 0;
}

static int power_callback(int unknown, int flags, void *common)
{
    if (flags & PSP_POWER_CB_SUSPENDING) {
        rec_on_suspend(); /* saved while the Memory Stick is still there */
        video_on_suspend();
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        cc_on_resume();
        saver_on_resume();
        video_on_resume();
    }
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cb = sceKernelCreateCallback("exit_cb", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    int pcb = sceKernelCreateCallback("power_cb", power_callback, NULL);
    if (pcb >= 0)
        scePowerRegisterCallback(-1, pcb);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int th = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* Safe start (safestart.h). cache/startguard.txt counts the starts that never settled:
 * one more now, back to 0 once this one has run for a while. Rewritten in place, because
 * a file created just before a freeze is lost (the FAT is never written). */
#define GUARD_FRAMES 900 /* 15 s: past the boot screen, the scan and the first previews */
static int s_guard;      /* frames left, 0 = nothing to clear */

static void guard_write(const char *dir, int n)
{
    char p[300], text[2] = {(char)('0' + n), '\n'};
    snprintf(p, sizeof(p), "%scache/startguard.txt", dir);
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, sizeof(text));
        sceIoClose(fd);
    }
}

/* This start got far enough (or is ending on purpose). */
static void guard_done(const char *dir)
{
    if (s_guard) {
        s_guard = 0;
        guard_write(dir, 0);
    }
}

static void clear_files(const char *path)
{
    SceUID d = sceIoDopen(path);
    if (d < 0)
        return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", path, e.d_name);
            sceIoRemove(p);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

/* Decides how this start runs. For a safe start, dir becomes the empty folder. */
static void safe_start_check(char *dir, int cap)
{
    int held = 0;
    for (int i = 0; i < 3 && !held; i++) { /* the pad needs a moment after input_init */
        SceCtrlData pad;
        sceKernelDelayThread(16000);
        sceCtrlPeekBufferPositive(&pad, 1);
        held = (pad.Buttons & PSP_CTRL_LTRIGGER) != 0;
    }
    char p[300], text[4];
    snprintf(p, sizeof(p), "%scache", dir);
    sceIoMkdir(p, 0777);
    int failed = 0;
    snprintf(p, sizeof(p), "%scache/startguard.txt", dir);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        failed = safestart_parse(text, sceIoRead(fd, text, sizeof(text)));
        sceIoClose(fd);
    }
    int mode = safestart_decide(failed, held);
    guard_write(dir, safestart_next(failed, mode));
    if (mode == SAFE_NO) {
        s_guard = GUARD_FRAMES;
        return;
    }
    g_safe_start = mode;
    strncat(dir, "safe/", cap - strlen(dir) - 1);
    snprintf(p, sizeof(p), "%.260s", dir);
    p[strlen(p) - 1] = 0; /* no trailing slash */
    sceIoMkdir(p, 0777);
    strncat(p, "/cache", sizeof(p) - strlen(p) - 1);
    sceIoMkdir(p, 0777);
    clear_files(p); /* what the last safe start left: this one starts from nothing */
    strncat(p, "/games", sizeof(p) - strlen(p) - 1);
    clear_files(p);
}

/* "ms0:/PSP/GAME/XLT/EBOOT.PBP" -> "ms0:/PSP/GAME/XLT/" */
static void app_dir(char *out, int n, const char *self)
{
    const char *slash = strrchr(self, '/');
    if (!slash) {
        snprintf(out, n, "ms0:/PSP/GAME/XLT/");
        return;
    }
    int len = (int)(slash - self) + 1;
    snprintf(out, n, "%.*s", len, self);
}

/* cache/launched: written right before XLT hands the PSP to a game, removed at
 * the next start. With it there and ARK set to return to its launcher (XLT)
 * after a game, nothing on the stick changed in between: the scan is skipped. */
static void marker_path(char *out, int n, const char *dir) { snprintf(out, n, "%scache/launched", dir); }

static void marker_write(const char *dir, const char *what)
{
    char p[300];
    marker_path(p, sizeof(p), dir);
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, what, strlen(what));
        sceIoClose(fd);
    }
}

static void marker_remove(const char *dir)
{
    char p[300];
    marker_path(p, sizeof(p), dir);
    sceIoRemove(p);
}

/* 1 if this start is the return from a game XLT launched, straight back (no XMB in between). */
static int back_from_game(const char *dir)
{
    char p[300];
    SceIoStat st;
    marker_path(p, sizeof(p), dir);
    if (sceIoGetstat(p, &st) < 0)
        return 0;
    marker_remove(dir);
    if (!ark_version())
        return 0;
    static ArkCfg c;
    char *extra = malloc(16 * 1024);
    int ok = extra && ark_load_cfg(&c, extra, 16 * 1024) == 0;
    free(extra);
    return ok && c.value[AK_LAUNCHER] > 0; /* off: the game ended in the XMB, where USB may have run */
}

/* Stops everything that touches the Memory Stick, records the play and hands
 * the PSP to ARK. Returns only if that failed. */
static void start(const Entry *e, const char *dir)
{
    if (!launch_supported()) {
        ui_toast("ARK not detected: games can't be started here");
        return;
    }
    if (fm_busy()) {
        ui_toast("Wait for the file job to finish (or stop it in Files)");
        return;
    }
    rec_stop();        /* a recording is saved before the game has the PSP */
    reader_shutdown(); /* closes an open PDF */
    web_shutdown();    /* drops the Wi-Fi connection */
    bg_flush(); /* settings just changed in a sheet are written before the game boots */
    music_shutdown(); /* frees the decoder and the audio channel for the game */
    video_shutdown();
    GameCfg cfg;
    gamecfg_load(e, &cfg); /* per-game ISO driver, default NP9660 */
    if (sysctl_usb_active())
        sysctl_usb_stop();
    library_shutdown();
    thumbs_shutdown();
    history_record(e->path);
    power_restore(); /* the game (or ARK's per-game setting) picks its own clock */
    sfx_drain(800); /* the launch notes finish before the PSP reboots into the game */
    sfx_shutdown();
    guard_done(dir); /* a game starting is a start that worked */
    marker_write(dir, e->path);
    umd_stop(); /* the drive's watcher thread: nothing may talk to it while the PSP reboots */
    launch_entry(e, cfg.driver);
    marker_remove(dir); /* still here: the launch failed */
    umd_start();
    sfx_init();

    power_idle();
    library_resort();
    thumbs_init(dir);
    library_rescan();
    ui_toast("Couldn't start this game");
}

int main(int argc, char *argv[])
{
    fit_heap(); /* before anything allocates */
    SceInt64 t_start = sceKernelGetSystemTimeWide();
    const char *self = argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/XLT/EBOOT.PBP";
    char dir[256];
    app_dir(dir, sizeof(dir), self);
    ark_init();
    launcher_early(self, dir, sizeof(dir)); /* as ARK's VBOOT.PBP: the data stays in XLT's own folder */
    char lib_self[300];
    snprintf(lib_self, sizeof(lib_self), "%s", self);
    if (launcher_running()) /* the library skips XLT itself: here, the copy in its folder */
        snprintf(lib_self, sizeof(lib_self), "%sEBOOT.PBP", dir);

    setup_callbacks();
    input_init();
    safe_start_check(dir, sizeof(dir)); /* before anything reads a saved file */
    gfx_init();
    power_idle();
    bg_init();
    sysctl_init();
    osd_init();              /* after sysctl: reads the volume once */
    psreg_init();            /* registry functions for the Settings tab */
    system_read_formats();   /* the PSP's time and date formats, on the worker */
    gamecfg_init(dir);       /* favorites and xlt.ini; before the library */
    music_init(dir);         /* where the music was last time (cache/music.txt) */
    viewer_init(dir);        /* pictures' rotations (cache/rotate.txt) */
    sysctl_storage_load(dir); /* the free space measured last time */
    theme_set(g_cfg.theme);   /* before anything is drawn, the boot screen too */
    history_init(dir);       /* before the library: it orders the Recent row */
    int scan = !g_safe_start && !back_from_game(dir);
    library_init(dir, lib_self, scan); /* cached index now, rescan in the background (unless back from a game) */
    thumbs_init(dir);
    cc_init(dir);            /* restores the saved brightness */
    shotplug_install();      /* the in-game screenshot plugin, on the background worker */
    int launcher_pending = 1; /* "Start XLT at boot" is seen to once the boot screen is gone (main loop) */
    video_init(dir);
    fm_init(dir);
    reader_init(dir);
    web_init(dir);
    ui_init();
    saver_init();
    sfx_init();              /* after xlt.ini: Interface sounds may be off */
    {
        char note[128];
        snprintf(note, sizeof(note), "heap %d KB (%d KB was free; %d KB wanted)", sce_newlib_heap_kb_size,
                 s_heap_free_kb, HEAP_KB);
        library_note(note);
    }
    void *probe = malloc(512 * 1024); /* the heap is there at all */
    if (!probe) {
        library_note("NOT ENOUGH MEMORY: XLT's heap couldn't be made, so nothing can be loaded");
        ui_toast("Not enough memory for XLT");
    }
    free(probe);
    if (g_safe_start) {
        ui_toast(safestart_message(g_safe_start));
    } else {
        boot_init();
        umd_start();         /* watches the UMD drive (not on a PSP Go) */
    }
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "startup %d ms before the first frame",
                 (int)((sceKernelGetSystemTimeWide() - t_start) / 1000));
        library_note(msg);
    }

    SysStatus st = {0};
    Input in;
    system_tick(&st);
    int dirty = 1;

    while (s_running) {
        input_poll(&in);
        if (boot_active()) { /* the boot screen takes every press (any button skips it) */
            dirty |= boot_update(in.pressed);
            in.pressed = 0;
        }
        int saver_dirty = saver_update(&in); /* may eat the press that wakes it */
        /* L+R+START: screenshot, L+R+SELECT: start or stop recording, from anywhere
         * in XLT. START and SELECT are swallowed so they don't also pause music or
         * open the control center; a trigger pressed while the other is held
         * doesn't switch tabs. */
        int shot = 0, rec = 0;
        if ((in.held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
            shot = (in.pressed & PSP_CTRL_START) != 0;
            rec = (in.pressed & PSP_CTRL_SELECT) != 0;
            in.pressed &= ~(PSP_CTRL_START | PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
        }
        dirty |= ui_update(&in, system_tick(&st));
        if (saver_covers())
            dirty = saver_dirty; /* 30 fps; nothing underneath is drawn */
        else
            dirty |= saver_dirty;
        int drew = dirty;
        if (dirty) {
            gfx_begin(g_th->bg); /* the theme's background */
            ui_draw(&st);
            gfx_end(); /* waits for vblank */
            dirty = 0;
        } else {
            sceDisplayWaitVblankStartCB();
        }
        dirty |= rec_frame(drew); /* the new picture, if any, goes to the recording */
        if (launcher_pending && !boot_active()) { /* after the animation: a copy can't make it stutter */
            launcher_pending = 0;
            launcher_sync(dir); /* XLT as ARK's VBOOT.PBP, or ARK's menu back */
            {
                char note[360];
                snprintf(note, sizeof(note), "XLT " XLT_VERSION " started as %.200s (%s, ARK-%d)", self,
                         launcher_running() ? "ARK's launcher" : "an app", ark_version());
                library_note(note);
            }
        }
        char rec_msg[96];
        if (rec_take_message(rec_msg, sizeof(rec_msg)) || shotplug_take_message(rec_msg, sizeof(rec_msg))) {
            ui_toast(rec_msg);
            dirty = 1;
        }
        if (rec) {
            dirty = 1;
            if (rec_active()) {
                rec_stop();
                sfx_now(SFX_OFF);
            } else if (sysctl_usb_active()) {
                ui_toast("No recording in USB mode");
            } else if (rec_start(dir) == 0) {
                ui_toast("Recording" " \xC2\xB7 " "L+R+SELECT stops");
                sfx_now(SFX_ON);
            } else {
                ui_toast("Couldn't start recording");
                sfx_now(SFX_ERROR);
            }
        }

        Entry e;
        if (ui_take_launch(&e)) {
            start(&e, dir);
            dirty = 1; /* only reached if the launch failed */
        }
        if (shot && sysctl_usb_active()) { /* the PC owns the stick: writing a PNG now could corrupt it */
            ui_toast("No screenshots in USB mode");
            shot = 0;
            dirty = 1;
        }
        if (cc_take_capture() || shot) {
            char name[48];
            power_boost(); /* PNG compression */
            int saved = sysctl_screenshot(dir, name, sizeof(name)) == 0;
            if (!library_scanning())
                power_idle();
            ui_toast(saved ? "Screenshot saved to Pictures" : "Couldn't save the screenshot");
            sfx_now(saved ? SFX_SHUTTER : SFX_ERROR);
            if (saved)
                library_rescan(); /* so it shows up in Pictures */
            dirty = 1;
        }
        if (ui_take_exit())
            s_running = 0;
        if (s_guard && !sysctl_usb_active() && --s_guard == 0) /* never while the PC has the stick */
            guard_write(dir, 0);
    }

    if (fm_busy()) { /* don't leave a half-copied file behind: stop the job, then wait for it */
        fop_cancel();
        for (int i = 0; i < 300 && fm_busy(); i++)
            sceKernelDelayThread(10000);
    }
    rec_stop();
    reader_shutdown();
    web_shutdown();
    bg_flush();
    music_shutdown();
    video_shutdown();
    if (sysctl_usb_active())
        sysctl_usb_stop();
    guard_done(dir);
    launcher_before_exit(); /* as ARK's launcher, the XMB would only start XLT again (logs, so before the library goes) */
    library_shutdown();
    thumbs_shutdown();
    sfx_shutdown();
    power_restore();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
