/* XLT - control center.
 *
 * Toggles: Wi-Fi (shows the hardware switch), USB, Capture, Sleep, Power.
 * Now playing: a card with the cover, title, artist / album and progress;
 * ✕ pauses, ←/→ change track, L/R seek 10 s. With nothing playing it is a
 * slim row and ✕ opens the Music list. Then the system volume (0-30, the
 * one the volume buttons change; ✕ mutes), brightness (0-100 % in 5 %
 * steps). Free space is in Settings > About (measuring it stops the PSP for seconds). Volume set here
 * goes through osd_sync(), so the volume OSD doesn't pop up for it. Focus moves in
 * reading order with the d-pad; ○ or SELECT closes. The hints follow the
 * focused row. USB and brightness are polled only while the sheet is open.
 * (The games' default CPU clock moved out: it belongs on the Settings tab.)
 *
 * Nothing slow runs on the UI thread: free space (sceIoDevctl walks the FAT
 * on big cards) is read, and xlt.ini written, by the shared low-priority
 * worker (bg.c). The sheet opens at once with the last known values
 * and updates when fresh ones arrive.
 */
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include "sfx.h"
#include "cc.h"
#include "ui_internal.h"
#include "anim.h"
#include "gamecfg.h"
#include "library.h"
#include "sysctl.h"
#include "thumbs.h"
#include "bg.h"
#include "music.h"
#include "nowplaying.h"
#include "fm.h"
#include "reader.h"
#include "video.h"
#include "osd.h"
#include "web.h"
#include "recorder.h"

#define CARD_H 60
#define SLIM_H 30
#define ROW_H 22
#define ROW_STEP 26 /* row height plus the gap */
#define MUSIC_Y 90
#define TILE_W 80
#define TILE_H 48
#define TILE_GAP 8
#define TILES_X ((SCREEN_W - 5 * TILE_W - 4 * TILE_GAP) / 2)
#define PANEL_W (SCREEN_W - 2 * TILES_X)
#define LABEL_X (TILES_X + 8)
#define CTRL_X 150
#define CTRL_R (SCREEN_W - TILES_X - 8) /* right edge of the row controls */

enum { T_WIFI, T_USB, T_CAPTURE, T_SLEEP, T_POWER, T_COUNT };
enum { ROW_TILES, ROW_MUSIC, ROW_VOLUME, ROW_LIGHT, ROW_COUNT };
enum { P_SHUTDOWN, P_RESTART, P_EXIT, P_CANCEL, P_COUNT };

/* The power dialog: grows out of the Power tile to this, centred on the screen. */
#define PD_W 420
#define PD_H 216
#define PD_X ((SCREEN_W - PD_W) / 2)
#define PD_Y ((SCREEN_H - PD_H) / 2)
#define PD_CARD 68      /* its cards, as on the home screen: the focused one grows */
#define PD_CARD_BIG 84
#define PD_GAP 14

static struct {
    int open;
    Spring t;
    int row, col;
    int power_menu, power_col;
    Spring power_t;       /* the dialog: 0 = the Power tile .. FX(256) = open (fine steps: no snap at the end) */
    Spring pcard[P_COUNT];
    int light, light0;    /* brightness 0..100 %, -1 if unavailable */
    int vol, muted;       /* system volume 0..VOLUME_MAX, -1 if unavailable */
    int avls;             /* the limiter is on: the level stops at AVLS_LEVEL */
    int reassert;         /* frames left in the boot window, see cc_step */
    volatile int was_asleep;
    int ini_dirty;        /* brightness changed while USB was on: save later */
    int usb_connected;
    Spring usb_t;         /* the USB mode screen: 0 hidden .. FX(1) shown */
    int usb_frames;
    int frames;
    int shown_sec;        /* elapsed second last drawn in the now-playing row */
    int capture;          /* 1 = pending, taken once the sheet is closed */
    int sleep;            /* sleep once the sheet is closed */
    char dev[8];
    char app_dir[200];
} C;

/* ------------------------------------------------- background jobs (bg.c) */

static void job_save_ini(void *arg)
{
    globalcfg_save();
}

void cc_init(const char *app_dir)
{
    snprintf(C.dev, sizeof(C.dev), "%.4s", app_dir);
    snprintf(C.app_dir, sizeof(C.app_dir), "%s", app_dir);
    if (g_cfg.brightness >= 0) {
        sysctl_brightness_set(g_cfg.brightness);
        C.reassert = 10 * 60; /* the firmware may still dim the panel while we load */
    }
}

static void open_sheet(void)
{
    C.open = 1;
    C.row = ROW_TILES;
    C.col = T_USB;
    C.power_menu = 0;
    spring_to(&C.t, FX(1));
    C.light = -1;
    if (sysctl_brightness_available()) {
        int hw = sysctl_brightness_hw();
        if (hw >= 0 && g_cfg.brightness >= 0 && hw == brightness_to_hw(g_cfg.brightness))
            C.light = g_cfg.brightness; /* exact: 74 panel levels can't round-trip every % */
        else if (hw >= 0)
            C.light = (brightness_from_hw(hw) + BRIGHTNESS_STEP / 2) / BRIGHTNESS_STEP * BRIGHTNESS_STEP;
    }
    C.light0 = C.light;
    C.vol = sysctl_volume_get(&C.muted);
    C.avls = sysctl_avls() > 0;
    C.usb_connected = sysctl_usb_connected();
}

static void close_sheet(void)
{
    if (C.light >= 0 && C.light != C.light0 && sysctl_brightness_available()) {
        g_cfg.brightness = C.light;
        C.ini_dirty = 1;
    }
    /* With USB on the PC owns the stick: keep the ini change for later. */
    if (C.ini_dirty && !sysctl_usb_active()) {
        C.ini_dirty = 0;
        bg_post(job_save_ini, NULL, 0);
    }
    C.open = 0;
    spring_to(&C.t, 0);
}

void cc_toggle(void)
{
    if (C.open) { close_sheet(); sfx_play(SFX_CLOSE); }
    else { open_sheet(); sfx_play(SFX_OPEN); }
}

int cc_is_open(void) { return C.open || C.t.x > 0; }

void cc_on_resume(void) { C.was_asleep = 1; }

int cc_take_capture(void)
{
    if (!C.capture || cc_is_open())
        return 0;
    C.capture = 0;
    return 1;
}

static void usb_toggle(void)
{
    if (!sysctl_usb_available()) {
        ui_toast("USB needs ARK");
        return;
    }
    if (sysctl_usb_active()) {
        sysctl_usb_stop();
        thumbs_init(C.app_dir);
        library_rescan(); /* the PC may have added or removed games */
        if (C.ini_dirty) {
            C.ini_dirty = 0;
            bg_post(job_save_ini, NULL, 0);
        }
        spring_to(&C.usb_t, 0);
        ui_toast("USB off" DOT "rescanning");
    } else {
        if (fm_busy()) {
            ui_toast("Wait for the file job to finish first");
            return;
        }
        reader_shutdown(); /* an open PDF is a file handle on the stick */
        /* Nothing may touch the Memory Stick while the PC has it. */
        if (music_state() != MUSIC_STOPPED)
            ui_toast("Music stopped for USB");
        music_shutdown();
        video_shutdown();
        rec_stop(); /* a recording is saved before the PC takes the stick */
        bg_flush();
        library_shutdown();
        thumbs_shutdown();
        if (sysctl_usb_start() == 0) {
            close_sheet(); /* the USB mode screen takes over until it is turned off */
            spring_to(&C.usb_t, FX(1));
        } else {
            thumbs_init(C.app_dir);
            library_rescan();
            ui_toast("Couldn't start USB");
        }
    }
    C.usb_connected = sysctl_usb_connected();
}

static void activate_tile(int t)
{
    switch (t) {
    case T_WIFI:
        ui_toast("Wi-Fi follows the switch on the PSP");
        break;
    case T_USB:
        usb_toggle();
        break;
    case T_CAPTURE:
        C.capture = 1;
        close_sheet();
        break;
    case T_SLEEP:
        if (sysctl_usb_active()) {
            ui_toast("Turn USB off first");
            break;
        }
        C.sleep = 1;
        close_sheet();
        break;
    case T_POWER: /* the tile grows into the power dialog, Cancel focused */
        C.power_menu = 1;
        C.power_col = P_CANCEL;
        spring_to(&C.power_t, FX(256));
        for (int i = 0; i < P_COUNT; i++)
            spring_snap(&C.pcard[i], FX(i == P_CANCEL ? PD_CARD_BIG : PD_CARD));
        break;
    }
}

static void power_close(void)
{
    C.power_menu = 0;
    spring_to(&C.power_t, 0); /* back into the tile */
}

static void power_focus(int i)
{
    if (i < 0 || i >= P_COUNT || i == C.power_col)
        return;
    sfx_play(SFX_MOVE);
    C.power_col = i;
    for (int k = 0; k < P_COUNT; k++)
        spring_to(&C.pcard[k], FX(k == i ? PD_CARD_BIG : PD_CARD));
}

static void power_action(int a)
{
    if (a == P_CANCEL) {
        power_close();
        return;
    }
    C.power_menu = 0;
    if (sysctl_usb_active())
        sysctl_usb_stop();
    bg_flush(); /* queued settings writes land before the power goes */
    music_shutdown();
    video_shutdown();
    if (a == P_SHUTDOWN) {
        library_shutdown();
        thumbs_shutdown();
        sysctl_shutdown();
    } else if (a == P_RESTART) {
        library_shutdown();
        thumbs_shutdown();
        sysctl_restart();
    } else {
        close_sheet();
        ui_request_exit();
    }
}

int cc_input(unsigned int p)
{
    if (!C.open)
        return 0;
    if (C.power_menu) {
        if (p & PSP_CTRL_LEFT) power_focus(C.power_col - 1);
        else if (p & PSP_CTRL_RIGHT) power_focus(C.power_col + 1);
        else if (p & PSP_CTRL_CROSS) power_action(C.power_col);
        else if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) power_close();
        return 1;
    }
    if (C.power_t.x > 0)
        return 1; /* still shrinking back into the tile */
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) {
        close_sheet();
        return 1;
    }
    if (p & PSP_CTRL_UP && C.row > 0) { C.row--; sfx_play(SFX_MOVE); }
    else if (p & PSP_CTRL_DOWN && C.row < ROW_COUNT - 1) { C.row++; sfx_play(SFX_MOVE); }
    else if (C.row == ROW_MUSIC) {
        if (video_is_open()) { /* the player has the decoder and the audio: no music until it closes */
            if (p & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)) sfx_play(SFX_ERROR);
        } else if (p & PSP_CTRL_TRIANGLE) { /* the whole screen of it */
            close_sheet();
            np_open();
        } else if (music_state() == MUSIC_STOPPED) {
            if (p & PSP_CTRL_CROSS && music_can_resume(NULL, NULL)) {
                if (music_resume() != 0) ui_toast("The songs from last time aren't there any more");
            } else if (p & PSP_CTRL_CROSS) {
                close_sheet();
                ui_open_music();
            }
        } else if (p & PSP_CTRL_SQUARE) {
            int m = music_sleep_cycle();
            char msg[48];
            snprintf(msg, sizeof(msg), m ? "Music stops in %d minutes" : "Sleep timer off", m);
            ui_toast(msg);
        } else if (p & PSP_CTRL_CROSS) music_toggle();
        else if (p & PSP_CTRL_LEFT) music_prev();
        else if (p & PSP_CTRL_RIGHT) music_next();
        else if (p & PSP_CTRL_LTRIGGER) music_seek(-10000);
        else if (p & PSP_CTRL_RTRIGGER) music_seek(10000);
    }
    else if (C.row == ROW_TILES) {
        if (p & PSP_CTRL_LEFT && C.col > 0) { C.col--; sfx_play(SFX_MOVE); }
        else if (p & PSP_CTRL_RIGHT && C.col < T_COUNT - 1) { C.col++; sfx_play(SFX_MOVE); }
        else if (p & PSP_CTRL_CROSS) activate_tile(C.col);
    } else {
        int dir = (p & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER)) ? -1
                : (p & (PSP_CTRL_RIGHT | PSP_CTRL_RTRIGGER | PSP_CTRL_CROSS)) ? 1 : 0;
        if (C.row == ROW_VOLUME && C.vol >= 0 && sysctl_volume_settable()) {
            int v = C.vol, m = C.muted;
            if (p & PSP_CTRL_CROSS) {
                m = !m;
            } else if (dir) {
                v += dir;
                m = 0; /* moving the slider unmutes, as the volume buttons do */
            }
            if (v < 0) v = 0;
            if (v > VOLUME_MAX) v = VOLUME_MAX;
            if (C.avls && v > AVLS_LEVEL && dir > 0) { /* as far as the volume buttons go */
                v = C.vol > AVLS_LEVEL ? C.vol : AVLS_LEVEL;
                if (C.vol >= AVLS_LEVEL)
                    ui_toast(AVLS_MESSAGE);
            }
            if (v != C.vol || m != C.muted) {
                sysctl_volume_set(v, m);
                osd_sync(); /* our change: no OSD */
                C.vol = sysctl_volume_get(&C.muted); /* what the PSP took (AVLS may cap it) */
            }
        } else if (C.row == ROW_VOLUME && (p & (PSP_CTRL_CROSS | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
            ui_toast(C.vol < 0 ? "Volume needs ARK" : "Use the volume buttons");
        } else if (dir && C.row == ROW_LIGHT && C.light >= 0) {
            int s = (C.light / BRIGHTNESS_STEP + dir) * BRIGHTNESS_STEP; /* snap to 5 % */
            if (p & PSP_CTRL_CROSS && C.light >= 100) s = 0;           /* ✕ cycles round */
            if (s < 0) s = 0;
            if (s > 100) s = 100;
            if (s != C.light && sysctl_brightness_set(s) == 0)
                C.light = s;
        }
    }
    return 1;
}

static int usb_step(void);

int cc_step(void)
{
    int old = C.t.x;
    spring_step(&C.t);
    int dirty = C.t.x != old;
    old = C.power_t.x;
    spring_step_kc(&C.power_t, 30, 100); /* softer than the rest: the growing is meant to be seen */
    dirty |= C.power_t.x != old;
    for (int i = 0; i < P_COUNT; i++) {
        old = C.pcard[i].x;
        spring_step(&C.pcard[i]);
        dirty |= C.pcard[i].x != old;
    }
    if (C.sleep && !cc_is_open()) {
        C.sleep = 0;
        bg_flush();
        sysctl_sleep();
    }
    if (C.was_asleep) { /* set by the power callback when the PSP wakes */
        C.was_asleep = 0;
        if (g_cfg.brightness >= 0)
            C.reassert = 3 * 60 + 1; /* the firmware restores its own level for a moment */
    }
    /* The firmware puts back its own stock level while an app loads and when
     * the PSP wakes; re-apply the user's level (as BetterBright does). */
    if (g_cfg.brightness >= 0 && C.reassert > 0) {
        if (--C.reassert % 30 == 0) { /* twice a second, no redraw needed */
            int want = brightness_to_hw(g_cfg.brightness), now = sysctl_brightness_hw();
            if (now >= 0 && now != want)
                sysctl_brightness_set(g_cfg.brightness);
        }
    }
    MusicNow now;
    if (C.open && music_state() == MUSIC_PLAYING && music_now(&now) == 0 && now.elapsed_ms / 1000 != C.shown_sec) {
        C.shown_sec = now.elapsed_ms / 1000;
        dirty = 1;
    }
    /* While open, the slider follows the volume buttons (the OSD stays away). */
    static int tick;
    if (C.open && (++tick & 1) == 0) {
        int m = 0, v = sysctl_volume_get(&m);
        C.avls = sysctl_avls() > 0;
        if (v != C.vol || (v >= 0 && m != C.muted)) {
            C.vol = v;
            C.muted = m;
            dirty = 1;
        }
    }
    dirty |= usb_step();
    /* While open: poll the USB connection once a second. */
    if (C.open && sysctl_usb_active() && ++C.frames >= 60) {
        C.frames = 0;
        int c = sysctl_usb_connected();
        if (c != C.usb_connected) {
            C.usb_connected = c;
            dirty = 1;
        }
    }
    return dirty;
}

/* ------------------------------------------------------------ USB mode */

/* Like the XMB: while the PC has the Memory Stick, one full screen says so
 * and nothing that reads the stick is drawn or started underneath. */
int cc_usb_screen(void) { return sysctl_usb_active() || C.usb_t.x > 0; }
int cc_usb_covers(void) { return sysctl_usb_active() && C.usb_t.x >= FX(1); }

int cc_usb_input(unsigned int p)
{
    if (!sysctl_usb_active())
        return 0;
    if (p & PSP_CTRL_CIRCLE)
        usb_toggle(); /* off: back to the launcher, which rescans */
    else if (p & PSP_CTRL_SELECT)
        ui_toast("Leave USB mode first (O)");
    return 1; /* every other button does nothing here */
}

static int usb_step(void)
{
    int old = C.usb_t.x, dirty = 0;
    spring_step(&C.usb_t);
    dirty = C.usb_t.x != old;
    if (sysctl_usb_active() && ++C.usb_frames >= 30) { /* the cable and the PC, twice a second */
        C.usb_frames = 0;
        int c = sysctl_usb_connected();
        if (c != C.usb_connected) {
            C.usb_connected = c;
            dirty = 1;
        }
    }
    return dirty;
}

void cc_usb_draw(const SysStatus *st)
{
    int t = C.usb_t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_draw_status_bar("USB mode", st);

    int on = C.usb_connected, cx = SCREEN_W / 2;
    const int s = 76, y = 54;
    gfx_rrect(cx - s / 2, y, s, s, s * 22 / 100, on ? C_ACCENT : C_RAISED);
    web_draw_icon(WEB_ICON_USB, cx - 24, y + s / 2 - 24, 48, 48, on ? C_WHITE : C_MUTED); /* drawn at this size: sharp */
    if (on) /* connected: the same "on" dot as the control center tile */
        gfx_rrect(cx + s / 2 - 14, y + 7, 8, 8, 4, C_WHITE);

    int ty = y + s + 16;
    gfx_text_center(FONT_BOLD, cx, ty, on ? "USB connected" : "USB mode", C_TEXT);
    ty += gfx_line_h(FONT_BOLD) + 2;
    gfx_text_center(FONT_BODY, cx, ty, on ? "The computer is using the Memory Stick."
                                          : "Connect the USB cable to a computer.", C_MUTED);
    ty += gfx_line_h(FONT_BODY) + 4;
    gfx_text_center(FONT_SMALL, cx, ty, on ? "Eject it on the computer before you leave USB mode."
                                           : "Waiting for the computer...", C_MUTED);

    gfx_rect(0, 248, SCREEN_W, 1, C_LINE);
    static const Hint hints[] = {{ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Exit USB mode"}};
    ui_draw_hints(hints, COUNT(hints), cx, 253);
    gfx_set_opacity(256);
}

/* ------------------------------------------------------------------ draw */

static void draw_tile(int i, int x, int y, int on, const char *label, int icon)
{
    int focused = C.open && !C.power_menu && C.row == ROW_TILES && C.col == i;
    gfx_rrect(x, y, TILE_W, TILE_H, 8, on ? C_ACCENT : C_RAISED);
    gfx_icon(icon, x + (TILE_W - 18) / 2, y + 7, 18, 18, on ? C_WHITE : C_MUTED);
    gfx_text_center(FONT_SMALL, x + TILE_W / 2, y + 28, label, on ? C_WHITE : C_TEXT);
    if (on) /* "on" is the fill plus this dot; focus is only the ring */
        gfx_rrect(x + TILE_W - 11, y + 5, 6, 6, 3, C_WHITE);
    if (focused)
        gfx_ring(x - 3, y - 3, TILE_W + 6, TILE_H + 6, C_FOCUS);
}

/* A play wedge, h px tall, as stacked 1 px lines (no triangle primitive). */
static void draw_play(int x, int y, int h, u32 c)
{
    for (int i = 0; i < h; i++) {
        int d = i <= h / 2 ? i : h - 1 - i; /* 0 at the tips, widest in the middle */
        gfx_rect(x, y + i, 1 + d * 7 / 4, 1, c);
    }
}

static void time_str(char *b, int n, int sec)
{
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%d:%02d", sec / 60, sec % 60);
}

static int music_card_h(void) { return music_state() != MUSIC_STOPPED ? CARD_H : SLIM_H; }

/* Now playing: cover with a play/pause badge, title, artist · album, progress. */
static void draw_music(int my)
{
    int focused = C.open && !C.power_menu && C.row == ROW_MUSIC;
    int fh = gfx_line_h(FONT_SMALL);
    MusicNow now;
    int playing = music_state() != MUSIC_STOPPED && music_now(&now) == 0;
    int h = playing ? CARD_H : SLIM_H;
    gfx_rrect(TILES_X, my, PANEL_W, h, 8, C_RAISED);
    if (focused)
        gfx_ring(TILES_X - 3, my - 3, PANEL_W + 6, h + 6, C_FOCUS);
    if (!playing) {
        gfx_icon(ICON_MUSIC, LABEL_X + 1, my + (h - 14) / 2, 14, 14, C_MUTED);
        const char *rt;
        char line[160];
        if (video_is_open())
            snprintf(line, sizeof(line), "Music is off while a video plays");
        else if (music_can_resume(&rt, NULL))
            snprintf(line, sizeof(line), "Resume: %s", rt[0] ? rt : "your music");
        else
            snprintf(line, sizeof(line), "Nothing playing");
        gfx_text_fit(FONT_SMALL, LABEL_X + 24, my + (h - fh) / 2, PANEL_W - (LABEL_X - TILES_X) - 34, line, C_MUTED);
        return;
    }
    int paused = music_state() == MUSIC_PAUSED;

    /* cover */
    const int cs = 42, cx = TILES_X + 8, cy = my + (h - cs) / 2;
    int idx = library_find(VIEW_MUSIC, now.path);
    const Entry *e = idx >= 0 ? library_get(VIEW_MUSIC, idx) : NULL;
    if (e) {
        ui_draw_cover(e, cx, cy, cs, 6, C_RAISED);
    } else {
        gfx_rrect(cx, cy, cs, cs, 6, C_SURFACE);
        gfx_icon(ICON_MUSIC, cx + 12, cy + 12, 18, 18, C_MUTED);
    }
    /* state badge on the cover's corner */
    const int bs = 18, bx = cx + cs - bs + 4, by = cy + cs - bs + 4;
    gfx_rrect(bx - 2, by - 2, bs + 4, bs + 4, (bs + 4) / 2, C_RAISED);
    gfx_rrect(bx, by, bs, bs, bs / 2, paused ? C_MUTED : C_ACCENT);
    if (paused) {
        draw_play(bx + 7, by + 5, 9, C_WHITE);
    } else {
        gfx_rect(bx + 6, by + 5, 2, 8, C_WHITE);
        gfx_rect(bx + 10, by + 5, 2, 8, C_WHITE);
    }

    /* text */
    int tx = cx + cs + 12, tw = TILES_X + PANEL_W - 10 - tx;
    const char *title = e ? e->title : now.title;
    const char *artist = e ? e->artist : now.artist;
    const char *album = e ? e->album : "";
    char sub[120];
    if (artist[0] && album[0]) snprintf(sub, sizeof(sub), "%s" DOT "%s", artist, album);
    else snprintf(sub, sizeof(sub), "%s", artist[0] ? artist : album[0] ? album : "Unknown artist");
    int sl = music_sleep_left();
    if (sl) { /* the sleep timer, in the card's corner */
        char z[24];
        snprintf(z, sizeof(z), "Sleep %d min", sl);
        int zw = gfx_text_width(FONT_SMALL, z);
        gfx_text(FONT_SMALL, tx + tw - zw, my + 24, z, C_FOCUS);
        gfx_text_fit(FONT_BOLD, tx, my + 4, tw, title, C_TEXT);
        gfx_text_fit(FONT_SMALL, tx, my + 24, tw - zw - 8, sub, C_MUTED);
    } else {
        gfx_text_fit(FONT_BOLD, tx, my + 4, tw, title, C_TEXT);
        gfx_text_fit(FONT_SMALL, tx, my + 24, tw, sub, C_MUTED);
    }

    /* progress: fixed slots for the times, so the bar never moves */
    int el = now.elapsed_ms / 1000, d = now.duration_ms / 1000;
    if (d > 0 && el > d) el = d;
    char t1[16], t2[16];
    time_str(t1, sizeof(t1), el);
    if (d > 0) time_str(t2, sizeof(t2), d);
    else snprintf(t2, sizeof(t2), "--:--");
    int slot = gfx_text_width(FONT_SMALL, d >= 3600 ? "0:00:00" : "00:00");
    int py = my + h - fh - 2;
    int bar_x = tx + slot + 8, bar_w = tx + tw - slot - 8 - bar_x;
    gfx_text(FONT_SMALL, tx, py, t1, C_MUTED); /* left edge in line with the title */
    gfx_rrect(bar_x, py + fh / 2 - 2, bar_w, 4, 2, C_SURFACE);
    int fill = d > 0 ? (int)((long long)bar_w * el / d) : 0;
    if (fill >= 4) /* narrower than its rounded ends would draw wrong */
        gfx_rrect(bar_x, py + fh / 2 - 2, fill, 4, 2, paused ? C_MUTED : C_FOCUS);
    gfx_text_right(FONT_SMALL, tx + tw, py, t2, C_MUTED);
}

/* Sheet height: header and tiles, the music card or slim row, three rows, hints. */
static int sheet_height(void) { return MUSIC_Y + music_card_h() + 8 + 2 * ROW_STEP + 2 + 20; }

static void row_label(int y, int icon, const char *label, int focused)
{
    int fh = gfx_line_h(FONT_SMALL);
    gfx_icon(icon, LABEL_X, y + (ROW_H - 16) / 2, 16, 16, focused ? C_TEXT : C_MUTED);
    gfx_text(FONT_SMALL, LABEL_X + 24, y + (ROW_H - fh) / 2, label, focused ? C_TEXT : C_MUTED);
}

/* A 0..100 slider with a knob and its value on the right. */
static void draw_slider(int y, int pct, int focused, int dimmed, const char *value)
{
    int fh = gfx_line_h(FONT_SMALL);
    const int lw = 44, bw = CTRL_R - CTRL_X - lw;
    int fill = 6 + (bw - 6) * pct / 100; /* 0 still shows a stub, so the knob has a track */
    gfx_rrect(CTRL_X, y + ROW_H / 2 - 3, bw, 6, 3, C_RAISED);
    gfx_rrect(CTRL_X, y + ROW_H / 2 - 3, fill, 6, 3, dimmed ? C_MUTED : focused ? C_ACCENT : C_FOCUS);
    gfx_rrect(CTRL_X + fill - 5, y + ROW_H / 2 - 6, 10, 12, 5, dimmed ? C_MUTED : C_TEXT); /* knob */
    gfx_text_right(FONT_SMALL, CTRL_R, y + (ROW_H - fh) / 2, value, dimmed ? C_MUTED : C_TEXT);
}

/* The power dialog, t = 0 (the Power tile at ty) .. 256 (open, centred). It
 * grows from the tile's rounded square; its contents fade in once it is
 * mostly there. */
static void draw_power(int ty)
{
    int t = C.power_t.x >> 8; /* 0..256, a little over while it bounces */
    if (C.power_t.x <= 0)
        return;
    int tt = t > 256 ? 256 : t < 0 ? 0 : t;
    ui_dim(tt * 150 >> 8); /* the control center steps back */
    const int x0 = TILES_X + 4 * (TILE_W + TILE_GAP), y0 = ty;
    int x = x0 + (PD_X - x0) * t / 256, y = y0 + (PD_Y - y0) * t / 256; /* t may overshoot a little: a bounce */
    int w = TILE_W + (PD_W - TILE_W) * t / 256, h = TILE_H + (PD_H - TILE_H) * t / 256;
    int r = 8 + 6 * tt / 256;
    gfx_rrect(x - 1, y - 1, w + 2, h + 2, r + 1, WITH_ALPHA(C_LINE, tt * 255 / 256));
    gfx_rrect(x, y, w, h, r, color_lerp(C_RAISED, C_SURFACE, tt));
    if (tt < 200) { /* the tile's own icon, going */
        int a = 256 - tt * 256 / 200, is = 18 + 14 * tt / 256;
        web_draw_icon(WEB_ICON_POWER, x + (w - is) / 2, y + (h - is) / 2, is, is, WITH_ALPHA(C_MUTED, a * 255 / 256));
    }
    int a = (tt - 214) * 256 / 42; /* the contents: only once the dialog is nearly its full size */
    if (a <= 0)
        return;
    if (a > 256) a = 256;
    gfx_set_opacity(a);
    gfx_icon(ICON_POWER, PD_X + 18, PD_Y + 14, 16, 16, C_FOCUS);
    gfx_text(FONT_BOLD, PD_X + 42, PD_Y + 12, "Power", C_TEXT);

    static const int icons[P_COUNT] = {WEB_ICON_POWER, WEB_ICON_RESTART, WEB_ICON_EXIT, WEB_ICON_CANCEL};
    static const char *const labels[P_COUNT] = {"Shut down", "Restart", "Exit to XMB", "Cancel"};
    static const char *const about[P_COUNT] = {
        "Turns the PSP off.", "Starts the PSP again.",
        "Closes XLT and goes back to the PSP's home menu.", "Back to the control center."};
    /* the row of cards, centred from the sizes they are heading to */
    int total = -PD_GAP, cy = PD_Y + 92;
    for (int i = 0; i < P_COUNT; i++)
        total += spring_int(&C.pcard[i]) + PD_GAP;
    int cx = PD_X + (PD_W - total) / 2;
    for (int i = 0; i < P_COUNT; i++) {
        int s = spring_int(&C.pcard[i]);
        int on = (C.pcard[i].x - FX(PD_CARD)) / (PD_CARD_BIG - PD_CARD); /* Q8: how focused */
        if (on < 0) on = 0;
        if (on > 256) on = 256;
        gfx_rrect(cx, cy - s / 2, s, s, s * 22 / 100, color_lerp(C_RAISED, C_ACCENT, on));
        int is = s * 44 / 100;
        web_draw_icon(icons[i], cx + (s - is) / 2, cy - is / 2, is, is, color_lerp(C_MUTED, C_WHITE, on));
        gfx_text_center(i == C.power_col ? FONT_BOLD : FONT_SMALL, cx + s / 2, cy + PD_CARD_BIG / 2 + 8, labels[i],
                        i == C.power_col ? C_TEXT : C_MUTED);
        cx += s + PD_GAP;
    }
    gfx_text_center(FONT_SMALL, SCREEN_W / 2, cy + PD_CARD_BIG / 2 + 30, about[C.power_col], C_MUTED);
    gfx_rect(PD_X + 14, PD_Y + PD_H - 30, PD_W - 28, 1, C_LINE);
    static const Hint hints[] = {
        {-1, "<-/->", 0, "Choose"},
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "OK"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Cancel"},
    };
    ui_draw_hints(hints, COUNT(hints), SCREEN_W / 2, PD_Y + PD_H - 24);
    gfx_set_opacity(256);
}

void cc_draw(const SysStatus *st)
{
    int t = C.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    ui_dim(t);
    const int sheet_h = sheet_height();
    const int y0 = -sheet_h + ((sheet_h * t) >> 8);
    /* A strip this thin under the sheet would only show cut-off text from the
     * screen behind it: run the sheet to the bottom edge instead. */
    const int bg_h = SCREEN_H - sheet_h < 24 ? SCREEN_H : sheet_h;
    gfx_rrect(0, y0 - 12, SCREEN_W, bg_h + 12, 12, C_SURFACE);
    int fh = gfx_line_h(FONT_SMALL);

    /* header */
    char buf[40];
    gfx_text(FONT_BOLD, TILES_X, y0 + 8, "Control center", C_TEXT);
    format_clock(buf, sizeof(buf), st->hour, st->minute);
    int x = SCREEN_W - TILES_X - gfx_text_width(FONT_SMALL, buf);
    gfx_text(FONT_SMALL, x, y0 + 10, buf, C_TEXT);
    if (st->battery >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", st->battery);
        x -= 10 + gfx_text_width(FONT_SMALL, buf);
        gfx_text(FONT_SMALL, x, y0 + 10, buf, C_MUTED);
        x -= 28;
        ui_draw_battery(x, y0 + 14, st);
    }

    /* toggles (the power dialog grows out of the last one, drawn over everything) */
    int ty = y0 + 34;
    {
        int usb_on = sysctl_usb_active();
        const char *usb_label = !usb_on ? "USB" : C.usb_connected ? "Connected" : "USB on";
        draw_tile(T_WIFI, TILES_X, ty, st->wifi_on, st->wifi_on ? "Wi-Fi on" : "Wi-Fi off", ICON_WIFI);
        draw_tile(T_USB, TILES_X + 1 * (TILE_W + TILE_GAP), ty, usb_on, usb_label, ICON_USB);
        draw_tile(T_CAPTURE, TILES_X + 2 * (TILE_W + TILE_GAP), ty, 0, "Capture", ICON_CAMERA);
        draw_tile(T_SLEEP, TILES_X + 3 * (TILE_W + TILE_GAP), ty, 0, "Sleep", ICON_MOON);
        draw_tile(T_POWER, TILES_X + 4 * (TILE_W + TILE_GAP), ty, 0, "Power", ICON_POWER);
    }

    /* now playing */
    int my = y0 + MUSIC_Y;
    draw_music(my);

    int vy = my + music_card_h() + 8, ry = vy + ROW_STEP;

    /* volume */
    int focus_vol = C.open && !C.power_menu && C.row == ROW_VOLUME;
    row_label(vy, C.muted ? ICON_MUTE : ICON_VOLUME, "Volume", focus_vol);
    if (C.vol >= 0) {
        char num[12];
        snprintf(num, sizeof(num), "%d", C.vol);
        draw_slider(vy, C.muted ? 0 : C.vol * 100 / VOLUME_MAX, focus_vol, C.muted, C.muted ? "Muted" : num);
    } else {
        gfx_text(FONT_SMALL, CTRL_X, vy + (ROW_H - fh) / 2, "Needs ARK", C_MUTED);
    }
    if (focus_vol)
        gfx_ring(TILES_X - 3, vy - 3, PANEL_W + 6, ROW_H + 6, C_FOCUS);

    /* brightness */
    int focus_light = C.open && !C.power_menu && C.row == ROW_LIGHT;
    row_label(ry, ICON_SUN, "Brightness", focus_light);
    if (C.light >= 0) {
        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", C.light);
        draw_slider(ry, C.light, focus_light, 0, pct);
    } else {
        gfx_text(FONT_SMALL, CTRL_X, ry + (ROW_H - fh) / 2, "Needs ARK", C_MUTED);
    }
    if (focus_light)
        gfx_ring(TILES_X - 3, ry - 3, PANEL_W + 6, ROW_H + 6, C_FOCUS);

    /* hints for the focused row */
    static const Hint power_hints[] = {
        {-1, "<-/->", 0, "Choose"},
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "OK"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    static const Hint tile_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Select"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint play_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Pause"},
        {-1, "<-/->", 0, "Track"},
        {-1, "L/R", 0, "Seek"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Full"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Sleep"},
    };
    static const Hint resume_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Play"},
        {-1, "<-/->", 0, "Track"},
        {-1, "L/R", 0, "Seek"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Full"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Sleep"},
    };
    static const Hint stopped_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open Music"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Full"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint carry_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Resume"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Full"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint vol_hints[] = {
        {-1, "<-/->", 0, "Adjust"},
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Mute"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint unmute_hints[] = {
        {-1, "<-/->", 0, "Adjust"},
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Unmute"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint light_hints[] = {
        {-1, "<-/->", 0, "Adjust"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    static const Hint close_hints[] = {
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
    };
    const Hint *h = tile_hints;
    int n = COUNT(tile_hints);
    if (C.power_menu) { h = power_hints; n = COUNT(power_hints); }
    else if (C.row == ROW_MUSIC) {
        int ms = music_state();
        if (video_is_open()) { h = close_hints; n = COUNT(close_hints); }
        else if (ms == MUSIC_PLAYING) { h = play_hints; n = COUNT(play_hints); }
        else if (ms == MUSIC_PAUSED) { h = resume_hints; n = COUNT(resume_hints); }
        else if (music_can_resume(NULL, NULL)) { h = carry_hints; n = COUNT(carry_hints); }
        else { h = stopped_hints; n = COUNT(stopped_hints); }
    } else if (C.row == ROW_VOLUME && C.vol >= 0) {
        if (C.muted) { h = unmute_hints; n = COUNT(unmute_hints); }
        else { h = vol_hints; n = COUNT(vol_hints); }
    } else if (C.row == ROW_LIGHT || C.row == ROW_VOLUME) { h = light_hints; n = COUNT(light_hints); }
    if (C.power_t.x <= 0)
        ui_draw_hints(h, n, SCREEN_W / 2, y0 + sheet_h - 22);
    draw_power(ty);
}
