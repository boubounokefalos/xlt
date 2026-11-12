/* XLT - options sheet for one game.
 *
 * Slides in from the right over the dimmed screen, so the selected game
 * stays in view. Play is first and focused: △ then ✕ starts the game.
 * ↑/↓ move, ←/→ change the focused value, ✕ activates, ○ closes.
 * Driver and CPU changes are written when the sheet closes (or on Play),
 * not on every press.
 *
 * All file access (the game's .cfg, SETTINGS.TXT, PLUGINS.TXT) runs on the
 * background worker: the sheet slides in on the first frame and those three
 * rows show "..." until their values arrive. Saving is queued the same way;
 * main.c flushes the queue before a launch, so Play always sees the change.
 *
 * Games and homebrew have "Delete from library": ✕ twice deletes the disc
 * image, or the whole folder that holds the EBOOT.PBP (delpath.c decides
 * what may go), on the worker. Pictures, music and video keep "Remove from
 * library", which only hides them.
 */
#include <stdio.h>
#include <string.h>
#include "sfx.h"
#include "opts.h"
#include "ui_internal.h"
#include "anim.h"
#include "ark.h"
#include "gamecfg.h"
#include "launch.h"
#include "library.h"
#include "plugins.h"
#include "bg.h"
#include "sysctl.h"
#include "imgdecode.h"
#include "delpath.h"
#include "fsdel.h"
#include "groups.h"
#include "musicsort.h"

#define SHEET_W 236
#define ROW_Y 56
#define ROW_H 26
#define PAD 14

enum { R_PLAY, R_SORT, R_DRIVER, R_CPU, R_PLUGINS, R_FAV, R_ADD_PL, R_PL_REMOVE, R_PL_MOVE, R_MOVE, R_DETAILS, R_REMOVE, R_DELETE, R_MAX };
#define PLUG_ROWS 7

static struct {
    int open;
    Spring t;          /* 0 closed .. FX(1) open */
    Entry e;           /* a copy: the library may be replaced while the sheet is open */
    int rows[R_MAX], nrows, focus;
    int details;
    int plugins_open;      /* the plugin panel is shown instead of the rows */
    PlugState plug[PLUG_MAX];
    int plug_new[PLUG_MAX]; /* pending override per plugin */
    int nplug, plug_sel, plug_top;
    int confirm_remove;    /* ✕ once on Remove/Delete: the next ✕ confirms */
    char del_target[200];  /* what Delete removes */
    int del_is_dir;        /* -1: this entry can't be deleted */
    int hidden;            /* the entry is in the Removed view */
    int in_pl;             /* a song opened from an open playlist */
    int sort_view;         /* the list "Sort ... by" orders: VIEW_MUSIC / GAMES / HOMEBREW, -1 none */
    GameCfg cfg, cfg0;
    int clock, clock0, has_clock;
    int loaded;            /* values below came back from the worker */
    int gen;               /* which open() a load result belongs to */
} O;

/* ------------------------------------------------------- worker jobs */

typedef struct {
    int gen;
    Entry e;
    int has_clock;
} LoadArg;

/* Written by the worker; `ready` is cleared first and set last. */
static struct {
    volatile int ready;
    int gen;
    GameCfg cfg;
    int clock;
    PlugState plug[PLUG_MAX];
    int nplug;
} L;

static void job_load(void *arg)
{
    const LoadArg *a = arg;
    L.ready = 0;
    L.gen = a->gen;
    gamecfg_load(&a->e, &L.cfg);
    L.clock = a->has_clock ? ark_get_clock(a->e.id) : CLOCK_AUTO;
    L.nplug = plugins_for(&a->e, L.plug, PLUG_MAX);
    L.ready = 1;
}

typedef struct {
    Entry e;
    int save_driver;
    GameCfg cfg;
    int save_clock, clock;
    int nchg;
    struct { PlugState p; int override; } chg[PLUG_MAX];
} CommitArg;
_Static_assert(sizeof(CommitArg) <= BG_ARG_MAX, "commit job too big for bg.c");

static volatile int s_plug_err, s_clock_err;

static void job_commit(void *arg)
{
    const CommitArg *c = arg;
    if (c->save_driver)
        gamecfg_save(&c->e, &c->cfg);
    for (int i = 0; i < c->nchg; i++)
        if (plugins_set(&c->e, &c->chg[i].p, c->chg[i].override) != 0)
            s_plug_err = 1;
    if (c->save_clock && ark_set_clock(c->e.id, c->clock) != 0)
        s_clock_err = 1;
}

typedef struct {
    Entry e;
    char target[200];
    int is_dir;
} DelArg;

/* s_del_busy: set by the UI when posting, cleared by the UI when it sees done. */
static int s_del_busy;
static volatile int s_del_done, s_del_err;
static char s_del_path[200];

static void job_delete(void *arg)
{
    const DelArg *d = arg;
    int r = fs_delete(d->target, d->is_dir);
    if (r == 0)
        gamecfg_forget(&d->e);
    snprintf(s_del_path, sizeof(s_del_path), "%s", d->e.path);
    s_del_err = r != 0;
    s_del_done = 1;
}

static const int s_drivers[] = {DRIVER_NP9660, DRIVER_INFERNO, DRIVER_MARCH33};

static const char *driver_label(int d)
{
    return d == DRIVER_INFERNO ? "Inferno" : d == DRIVER_MARCH33 ? "March33" : "NP9660";
}

void opts_open(const Entry *e)
{
    if (sysctl_usb_active()) {
        ui_toast("Turn USB off to change game settings");
        return;
    }
    O.e = *e;
    O.open = 1;
    O.details = 0;
    O.plugins_open = 0;
    O.confirm_remove = 0;
    O.focus = 0;
    O.hidden = hidden_get(e->path);
    O.nplug = 0;
    O.plug_sel = O.plug_top = 0;
    O.has_clock = ark_version() && ark_dir()[0] && e->id[0];
    O.loaded = 0;
    spring_to(&O.t, FX(1));

    static LoadArg a; /* static: too big for the stack frame of a UI call */
    a.gen = ++O.gen;
    a.e = *e;
    a.has_clock = O.has_clock;
    bg_post(job_load, &a, sizeof(a));

    O.nrows = 0;
    O.in_pl = kind_is_music(e->kind) && ui_music_in_playlist();
    if (!O.hidden)
        O.rows[O.nrows++] = R_PLAY;
    O.sort_view = -1;
    if (kind_is_music(e->kind) && !O.in_pl)
        O.sort_view = VIEW_MUSIC; /* the Music list's order (a playlist keeps its own) */
    else if (!kind_is_picture(e->kind) && !kind_is_music(e->kind) && !kind_is_video(e->kind) && !kind_is_umd(e->kind))
        O.sort_view = groups_tab(e->path, kind_is_homebrew(e->kind) ? VIEW_HOMEBREW : VIEW_GAMES); /* the tab it's in */
    if (O.sort_view >= 0 && !O.hidden)
        O.rows[O.nrows++] = R_SORT;
    if (kind_is_disc(e->kind))
        O.rows[O.nrows++] = R_DRIVER;
    if (O.has_clock)
        O.rows[O.nrows++] = R_CPU;
    int media = kind_is_picture(e->kind) || kind_is_music(e->kind) || kind_is_video(e->kind);
    int umd = kind_is_umd(e->kind); /* the disc in the drive: nothing to favour, move or delete */
    if (!O.hidden && !umd)
        O.rows[O.nrows++] = R_FAV;
    if (kind_is_music(e->kind) && !O.hidden)
        O.rows[O.nrows++] = R_ADD_PL;
    if (O.in_pl) {
        O.rows[O.nrows++] = R_PL_REMOVE;
        O.rows[O.nrows++] = R_PL_MOVE;
    }
    if (!O.hidden && !media && !umd)
        O.rows[O.nrows++] = R_MOVE; /* to the other tab or a folder (XLT only) */
    O.rows[O.nrows++] = R_DETAILS;
    O.del_is_dir = media || umd ? -1 : del_target(e->path, !kind_is_disc(e->kind), library_self(), O.del_target, sizeof(O.del_target));
    if (!umd && (O.hidden || O.del_is_dir < 0))
        O.rows[O.nrows++] = R_REMOVE; /* Restore (games) or Remove/Restore (media) */
    if (O.del_is_dir >= 0)
        O.rows[O.nrows++] = R_DELETE;
}

int opts_is_open(void) { return O.open || O.t.x > 0; }

/* Writes what changed. Called on close and right before Play. */
static void commit(void)
{
    if (!O.loaded)
        return; /* nothing could have been changed yet */
    static CommitArg c;
    memset(&c, 0, sizeof(c));
    c.e = O.e;
    c.save_driver = O.cfg.driver != O.cfg0.driver;
    c.cfg = O.cfg;
    c.save_clock = O.has_clock && O.clock != O.clock0;
    c.clock = O.clock;
    for (int i = 0; i < O.nplug; i++)
        if (O.plug_new[i] != O.plug[i].override) {
            c.chg[c.nchg].p = O.plug[i];
            c.chg[c.nchg].override = O.plug_new[i];
            c.nchg++;
            O.plug[i].override = O.plug_new[i];
        }
    if (!c.save_driver && !c.save_clock && !c.nchg)
        return;
    O.cfg0 = O.cfg;
    O.clock0 = O.clock;
    bg_post(job_commit, &c, sizeof(c));
}

static void close_sheet(void)
{
    commit();
    O.open = 0;
    spring_to(&O.t, 0);
}

void opts_close(void)
{
    if (O.open)
        close_sheet();
}

static void change(int row, int dir)
{
    if (row == R_SORT) { /* needs nothing from the worker */
        if (O.sort_view == VIEW_MUSIC) ui_music_sort(dir);
        else ui_game_sort(O.sort_view, dir);
        return;
    }
    if (!O.loaded)
        return;
    if (row == R_DRIVER) {
        int i = 0;
        while (i < COUNT(s_drivers) && s_drivers[i] != O.cfg.driver)
            i++;
        i = (i + dir + COUNT(s_drivers)) % COUNT(s_drivers);
        O.cfg.driver = s_drivers[i];
    } else if (row == R_CPU) {
        O.clock = (O.clock + dir + CLOCK_COUNT) % CLOCK_COUNT;
    }
}

static void delete_entry(void)
{
    if (s_del_busy) {
        ui_toast("Still deleting the last one");
        return;
    }
    static DelArg d; /* static: an Entry is too big for this frame */
    d.e = O.e;
    snprintf(d.target, sizeof(d.target), "%s", O.del_target);
    d.is_dir = O.del_is_dir;
    s_del_busy = 1;
    close_sheet();
    ui_toast("Deleting...");
    bg_post(job_delete, &d, sizeof(d));
}

static int plug_cycle(int v, int dir)
{
    /* Global -> On -> Off -> Global */
    static const int order[3] = {PLUG_GLOBAL, PLUG_ON, PLUG_OFF};
    int i = v == PLUG_GLOBAL ? 0 : v == PLUG_ON ? 1 : 2;
    return order[(i + dir + 3) % 3];
}

static void plug_select(int i)
{
    if (i < 0) i = 0;
    if (i >= O.nplug) i = O.nplug - 1;
    O.plug_sel = i < 0 ? 0 : i;
    if (O.plug_sel < O.plug_top) O.plug_top = O.plug_sel;
    if (O.plug_sel >= O.plug_top + PLUG_ROWS) O.plug_top = O.plug_sel - PLUG_ROWS + 1;
}

static void remove_or_restore(void)
{
    if (O.hidden) {
        hidden_set(O.e.path, 0);
        ui_toast("Restored to the library");
    } else if (hidden_set(O.e.path, 1) == 0) {
        favorite_set(O.e.path, 0);
        ui_toast("Removed. Restore it from Settings (the file is kept)");
    } else {
        ui_toast("The removed list is full");
        return;
    }
    close_sheet();
    library_resort();
    ui_library_changed();
}

int opts_input(unsigned int p)
{
    if (!O.open)
        return 0;
    if (O.plugins_open) {
        if (p & PSP_CTRL_CIRCLE) O.plugins_open = 0;
        else if (p & PSP_CTRL_TRIANGLE) close_sheet();
        else if (p & PSP_CTRL_UP) plug_select(O.plug_sel - 1);
        else if (p & PSP_CTRL_DOWN) plug_select(O.plug_sel + 1);
        else if (O.nplug && p & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER))
            O.plug_new[O.plug_sel] = plug_cycle(O.plug_new[O.plug_sel], -1);
        else if (O.nplug && p & (PSP_CTRL_RIGHT | PSP_CTRL_RTRIGGER | PSP_CTRL_CROSS))
            O.plug_new[O.plug_sel] = plug_cycle(O.plug_new[O.plug_sel], 1);
        return 1;
    }
    if (O.details) {
        if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))
            O.details = 0;
        else if (p & PSP_CTRL_TRIANGLE)
            close_sheet();
        return 1;
    }
    int row = O.rows[O.focus];
    if (!(p & PSP_CTRL_CROSS))
        O.confirm_remove = 0; /* anything but a second ✕ cancels */
    if (p & PSP_CTRL_UP && O.focus > 0) { O.focus--; sfx_play(SFX_MOVE); }
    else if (p & PSP_CTRL_DOWN && O.focus < O.nrows - 1) { O.focus++; sfx_play(SFX_MOVE); }
    else if (p & (PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER)) change(row, -1);
    else if (p & (PSP_CTRL_RIGHT | PSP_CTRL_RTRIGGER)) change(row, 1);
    else if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) close_sheet();
    else if (p & PSP_CTRL_CROSS) {
        switch (row) {
        case R_PLAY:
            close_sheet();
            ui_request_launch(&O.e);
            break;
        case R_SORT:
        case R_DRIVER:
        case R_CPU:
            change(row, 1);
            break;
        case R_FAV:
            favorite_set(O.e.path, !favorite_get(O.e.path));
            library_resort();
            ui_library_changed();
            break;
        case R_DETAILS:
            O.details = 1;
            break;
        case R_MOVE:
            ui_open_move(&O.e); /* the menu opens over the sheet */
            break;
        case R_ADD_PL:
            ui_open_add_to_playlist(&O.e); /* the same way */
            break;
        case R_PL_REMOVE:
            close_sheet();
            ui_playlist_remove(&O.e);
            break;
        case R_PL_MOVE:
            close_sheet();
            ui_playlist_move(&O.e);
            break;
        case R_PLUGINS:
            if (O.loaded) {
                O.plugins_open = 1;
                plug_select(0);
            }
            break;
        case R_REMOVE:
            if (O.hidden || O.confirm_remove)
                remove_or_restore();
            else
                O.confirm_remove = 1;
            break;
        case R_DELETE:
            if (O.confirm_remove)
                delete_entry();
            else
                O.confirm_remove = 1;
            break;
        }
    }
    return 1;
}

int opts_step(void)
{
    int old = O.t.x;
    spring_step(&O.t);
    int dirty = O.t.x != old;
    if (L.ready && L.gen == O.gen && !O.loaded && O.open) {
        O.cfg = O.cfg0 = L.cfg;
        O.clock = O.clock0 = L.clock;
        O.nplug = L.nplug;
        memcpy(O.plug, L.plug, sizeof(PlugState) * L.nplug);
        for (int i = 0; i < O.nplug; i++)
            O.plug_new[i] = O.plug[i].override;
        O.loaded = 1;
        dirty = 1;
    }
    if (s_del_done) {
        s_del_done = 0;
        s_del_busy = 0;
        if (!s_del_err) {
            favorite_set(s_del_path, 0);
            hidden_set(s_del_path, 0);
        }
        library_forget(s_del_path); /* on an error the rescan brings back what is left */
        ui_library_changed();
        ui_toast(s_del_err ? "Couldn't delete all of it" : "Deleted from the Memory Stick");
        dirty = 1;
    }
    if (s_plug_err) {
        s_plug_err = 0;
        ui_toast("Couldn't write PLUGINS.TXT");
        dirty = 1;
    }
    if (s_clock_err) {
        s_clock_err = 0;
        ui_toast("Couldn't write ARK's SETTINGS.TXT");
        dirty = 1;
    }
    return dirty;
}

/* ------------------------------------------------------------------ draw */

static void row_value(int row, char *out, int n)
{
    out[0] = 0;
    if (!O.loaded && (row == R_DRIVER || row == R_CPU || row == R_PLUGINS)) {
        snprintf(out, n, "...");
        return;
    }
    if (row == R_SORT)
        snprintf(out, n, "%s", O.sort_view == VIEW_MUSIC ? msort_label(g_cfg.music_sort)
                               : gsort_label(g_cfg.game_sort[O.sort_view == VIEW_HOMEBREW]));
    else if (row == R_DRIVER)
        snprintf(out, n, "%s", driver_label(O.cfg.driver));
    else if (row == R_CPU)
        snprintf(out, n, "%s", clock_label(O.clock));
    else if (row == R_MOVE) {
        int def = kind_is_homebrew(O.e.kind) ? VIEW_HOMEBREW : VIEW_GAMES, t = groups_tab(O.e.path, def);
        const char *f = groups_folder(O.e.path);
        snprintf(out, n, f[0] ? "%s" : "%s", f[0] ? f : t == VIEW_HOMEBREW ? "Homebrew" : "Games");
    } else if (row == R_PLUGINS) {
        int custom = 0;
        for (int i = 0; i < O.nplug; i++)
            custom += O.plug_new[i] != PLUG_GLOBAL;
        if (O.nplug == 0) snprintf(out, n, "None found");
        else if (custom == 0) snprintf(out, n, "Global");
        else snprintf(out, n, "%d custom", custom);
    }
}

static const char *row_label(int row)
{
    switch (row) {
    case R_PLAY: return kind_is_picture(O.e.kind) ? "View" : "Play";
    case R_DRIVER: return "ISO driver";
    case R_CPU: return "CPU speed";
    case R_FAV: return favorite_get(O.e.path) ? "Remove from favorites" : "Add to favorites";
    case R_PLUGINS: return "Plugins";
    case R_MOVE: return "Move to...";
    case R_SORT: return O.sort_view == VIEW_MUSIC ? "Sort songs by" : O.sort_view == VIEW_HOMEBREW ? "Sort apps by" : "Sort games by";
    case R_ADD_PL: return "Add to playlist...";
    case R_PL_REMOVE: return "Remove from playlist";
    case R_PL_MOVE: return "Move in playlist";
    case R_REMOVE:
        return O.hidden ? "Restore to library" : O.confirm_remove ? "Press X again to remove" : "Remove from library";
    case R_DELETE:
        return O.confirm_remove ? "Press X again to delete" : "Delete from library";
    default: return "Details";
    }
}

/* Draws text wrapped to width w, breaking anywhere; returns the y after it. */
static int wrap(int font, int x, int y, int w, const char *s, u32 c, int max_lines)
{
    char line[128];
    for (int l = 0; *s && l < max_lines; l++) {
        int n = 0;
        while (s[n] && n < (int)sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (gfx_text_width(font, line) > w) break;
            n++;
        }
        if (n == 0) n = 1;
        snprintf(line, sizeof(line), "%.*s", n, s);
        gfx_text(font, x, y, line, c);
        s += n;
        y += gfx_line_h(font);
    }
    return y;
}

static void draw_details(int x, int w)
{
    char size[16], buf[64];
    int y = ROW_Y;
    const int lh = gfx_line_h(FONT_SMALL) + 3;
    struct { const char *k; const char *v; } rows[10];
    int n = 0;
    format_size(size, sizeof(size), O.e.size);
    char date[24];
    rows[n].k = "Format"; rows[n++].v = kind_is_picture(O.e.kind) ? img_format_name(O.e.path) : kind_name(O.e.kind);
    if (kind_is_picture(O.e.kind)) {
        format_date(date, sizeof(date), O.e.mtime);
        rows[n].k = "Date"; rows[n++].v = date;
    }
    if (kind_is_video(O.e.kind)) {
        if (O.e.id[0]) { rows[n].k = "Resolution"; rows[n++].v = O.e.id; }
        if (O.e.sysver[0]) { rows[n].k = "Length"; rows[n++].v = O.e.sysver; }
    }
    char trk[8];
    if (kind_is_music(O.e.kind)) {
        if (O.e.artist[0]) { rows[n].k = "Artist"; rows[n++].v = O.e.artist; }
        if (O.e.album[0]) { rows[n].k = "Album"; rows[n++].v = O.e.album; }
        if (O.e.track) {
            snprintf(trk, sizeof(trk), "%d", O.e.track);
            rows[n].k = "Track"; rows[n++].v = trk;
        }
    }
    rows[n].k = "Size"; rows[n++].v = size;
    if (O.e.id[0] && !kind_is_video(O.e.kind)) { rows[n].k = "Game ID"; rows[n++].v = O.e.id; }
    const char *reg = region_name(O.e.id);
    if (reg) { rows[n].k = "Region"; rows[n++].v = reg; }
    if (O.e.sysver[0] && !kind_is_video(O.e.kind)) {
        snprintf(buf, sizeof(buf), "%s or later", O.e.sysver);
        rows[n].k = "Firmware"; rows[n++].v = buf;
    }
    for (int i = 0; i < n; i++, y += lh) {
        gfx_text(FONT_SMALL, x + PAD, y, rows[i].k, C_MUTED);
        gfx_text_right(FONT_SMALL, x + w - PAD, y, rows[i].v, C_TEXT);
    }
    y += 4;
    gfx_text(FONT_SMALL, x + PAD, y, "File", C_MUTED);
    wrap(FONT_SMALL, x + PAD, y + lh, w - 2 * PAD, O.e.path, C_TEXT, 4);
}

static void draw_plugins(int x, int w)
{
    const int y0 = ROW_Y, rh = 22;
    if (O.nplug == 0) {
        gfx_text_fit(FONT_SMALL, x + PAD, y0, w - 2 * PAD, "No plugins found in PLUGINS.TXT", C_MUTED);
        return;
    }
    for (int i = O.plug_top; i < O.nplug && i < O.plug_top + PLUG_ROWS; i++) {
        int y = y0 + (i - O.plug_top) * rh, focus = i == O.plug_sel;
        if (focus)
            gfx_rrect(x + 8, y, w - 16, rh - 2, 5, C_ACCENT);
        const char *name = strrchr(O.plug[i].path, '/');
        name = name ? name + 1 : O.plug[i].path;
        int v = O.plug_new[i];
        int on = v == PLUG_GLOBAL ? O.plug[i].global_on : v == PLUG_ON;
        char val[24];
        snprintf(val, sizeof(val), v == PLUG_GLOBAL ? "%s (global)" : "%s", on ? "On" : "Off");
        int vw = gfx_text_width(FONT_SMALL, val);
        int ty = y + (rh - 2 - gfx_line_h(FONT_BODY)) / 2;
        gfx_text_fit(FONT_BODY, x + PAD + 2, ty, w - 2 * PAD - vw - 10, name, focus ? C_WHITE : C_TEXT);
        gfx_text_right(FONT_SMALL, x + w - PAD - 2, ty + 1, val,
                       focus ? C_WHITE : v == PLUG_GLOBAL ? C_MUTED : on ? C_FOCUS : C_TEXT);
    }
    gfx_text_fit(FONT_SMALL, x + PAD, y0 + PLUG_ROWS * rh + 6, w - 2 * PAD,
                 "Applies the next time the game starts", C_MUTED);
}

void opts_draw(void)
{
    int t = O.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    ui_dim(t);
    const int w = SHEET_W, x = SCREEN_W - ((w * t) >> 8);
    gfx_rect(x, 0, w, SCREEN_H, C_SURFACE);
    gfx_rect(x, 0, 1, SCREEN_H, C_LINE);

    gfx_text_fit(FONT_BOLD, x + PAD, 12, w - 2 * PAD, O.e.title, C_TEXT);
    char sub[48], size[16];
    format_size(size, sizeof(size), O.e.size);
    snprintf(sub, sizeof(sub), "%s" DOT "%s",
             kind_is_picture(O.e.kind) ? img_format_name(O.e.path) : kind_name(O.e.kind), size);
    gfx_text(FONT_SMALL, x + PAD, 32, sub, C_MUTED);

    static const Hint rows_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Select"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Close"},
        {-1, "L/R", 0, "Change"},
    };
    static const Hint plug_hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Change"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    static const Hint details_hints[] = {
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };

    if (O.plugins_open) {
        draw_plugins(x, w);
        ui_draw_hints(plug_hints, COUNT(plug_hints), x + w / 2, 253);
        return;
    }
    if (O.details) {
        draw_details(x, w);
        ui_draw_hints(details_hints, COUNT(details_hints), x + w / 2, 253);
        return;
    }
    const int rh = O.nrows > 7 ? 23 : ROW_H; /* eight rows (a game with every setting) still fit */
    for (int i = 0; i < O.nrows; i++) {
        int row = O.rows[i], y = ROW_Y + i * rh + (row >= R_FAV || (O.hidden && row >= R_DETAILS) ? 8 : 0);
        int focused = i == O.focus;
        if (i > 0 && (row == R_FAV || (O.hidden && row == R_DETAILS)) )
            gfx_rect(x + PAD, y - 6, w - 2 * PAD, 1, C_LINE);
        if (focused)
            gfx_rrect(x + 8, y, w - 16, rh - 2, 5, C_ACCENT);
        int ty = y + (rh - 2 - gfx_line_h(FONT_BODY)) / 2;
        u32 c = focused ? C_WHITE : (row == R_REMOVE && !O.hidden) || row == R_DELETE ? C_LOW : C_TEXT;
        if (row == R_FAV && favorite_get(O.e.path))
            gfx_icon(ICON_STAR, x + w - PAD - 12, ty + 2, 12, 12, focused ? C_WHITE : C_FOCUS);
        gfx_text(row == R_PLAY ? FONT_BOLD : FONT_BODY, x + PAD + 2, ty, row_label(row), c);
        char v[40];
        row_value(row, v, sizeof(v));
        if (v[0]) {
            char shown[48];
            int arrows = focused && ((O.loaded && (row == R_DRIVER || row == R_CPU)) || row == R_SORT); /* the rows ←/→ change */
            snprintf(shown, sizeof(shown), arrows ? "< %s >" : "%s", v);
            int room = w - 2 * PAD - 16 - gfx_text_width(row == R_PLAY ? FONT_BOLD : FONT_BODY, row_label(row));
            int vw = gfx_text_width(FONT_SMALL, shown);
            if (vw > room) vw = room; /* a long folder name: cut, not over the label */
            gfx_text_fit(FONT_SMALL, x + w - PAD - 2 - vw, ty + 1, vw, shown, focused ? C_WHITE : C_MUTED);
        }
    }
    if (O.has_clock && O.clock != CLOCK_AUTO && ROW_Y + O.nrows * rh + 20 + gfx_line_h(FONT_SMALL) < 248)
        gfx_text_fit(FONT_SMALL, x + PAD, ROW_Y + O.nrows * rh + 20, w - 2 * PAD,
                     "CPU speed is applied by ARK at boot", C_MUTED);
    ui_draw_hints(rows_hints, COUNT(rows_hints), x + w / 2, 253);
}
