/* XLT - the side menus of the home and list screens: a Games / Homebrew
 * folder's menu and Move to, and in Music the sort order, the playlists'
 * menu and Add to playlist. Split from ui.c, whose state they share
 * (ui_state.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ui_internal.h"
#include "ui_state.h"
#include "gamecfg.h"
#include "menu.h"
#include "osk.h"
#include "opts.h"
#include "bg.h"
#include "music.h"
#include "playlists.h"
#include "musicsort.h"

static void job_save_ini(void *arg) { globalcfg_save(); }

/* ------------------------------------------- folder and Move to menus */

static const char *tab_name(int t) { return t == VIEW_HOMEBREW ? "Homebrew" : "Games"; }

void enter_folder(const char *name)
{
    snprintf(S.folder[S.view], sizeof(S.folder[S.view]), "%s", name);
    disp_build(S.view);
    S.top[S.view] = 0;
    select_entry(0, 1);
}

/* After a change to groups: save, rebuild the views, keep the focus sensible. */
static void groups_changed(const char *msg)
{
    groups_save();
    library_resort();
    ui_library_changed();
    if (msg)
        ui_toast(msg);
}

static const char *folder_error(int r)
{
    return r == -1 ? "That's the most folders a tab can have (16)" : r == -2 ? "There's a folder with that name already"
                                                                    : "Type a name for the folder";
}

/* Focus the folder named name, if the list shows it. */
static void focus_folder(int tab, const char *name)
{
    if (S.view != tab || S.folder[tab][0])
        return;
    int at = disp_find_folder(tab, name);
    if (at >= 0)
        select_entry(at, 1);
}

static void on_new_folder(const char *text)
{
    if (!text)
        return;
    int t = S.move_tab, r = groups_folder_add(t, text);
    char name[GROUP_NAME], msg[96];
    if (r != 0) {
        ui_toast(folder_error(r));
        return;
    }
    groups_clean_name(text, name);
    if (S.move_path[0]) { /* made from Move to: the entry goes in */
        r = groups_set(S.move_path, S.move_def, t, name);
        S.move_path[0] = 0;
        snprintf(msg, sizeof(msg), r ? "Folder made, but the game couldn't be moved" : "Moved to %s / %s",
                 tab_name(t), name);
    } else {
        snprintf(msg, sizeof(msg), "Folder %s made", name);
    }
    groups_changed(msg);
    focus_folder(t, name);
}

static void on_rename_folder(const char *text)
{
    if (!text)
        return;
    int t = S.view, r = groups_folder_rename(t, S.menu_folder, text);
    if (r != 0) {
        ui_toast(folder_error(r));
        return;
    }
    char name[GROUP_NAME];
    groups_clean_name(text, name);
    if (!strcasecmp(S.folder[t], S.menu_folder))
        snprintf(S.folder[t], sizeof(S.folder[t]), "%s", name);
    groups_changed("Folder renamed");
    focus_folder(t, name);
}

enum { FM_OPEN = 1, FM_RENAME, FM_NEW, FM_DELETE };
static int s_folder_confirm;

static void fill_folder_menu(MenuItem *it, int *n)
{
    int k = 0;
    memset(it, 0, sizeof(MenuItem) * 4);
    it[k].id = FM_OPEN; snprintf(it[k++].label, sizeof(it[0].label), "Open");
    it[k].id = FM_RENAME; snprintf(it[k++].label, sizeof(it[0].label), "Rename...");
    it[k].id = FM_NEW; snprintf(it[k++].label, sizeof(it[0].label), "New folder...");
    it[k].id = FM_DELETE; it[k].danger = 1; it[k].gap = 1;
    snprintf(it[k++].label, sizeof(it[0].label), s_folder_confirm ? "Press X again to delete" : "Delete folder");
    *n = k;
}

static void folder_menu_cb(int id, int dir)
{
    if (dir)
        return;
    if (id != FM_DELETE)
        s_folder_confirm = 0;
    switch (id) {
    case FM_OPEN:
        menu_close();
        enter_folder(S.menu_folder);
        return;
    case FM_RENAME:
        menu_close();
        osk_open("Rename folder", S.menu_folder, OSK_FILENAME, on_rename_folder);
        return;
    case FM_NEW:
        menu_close();
        S.move_path[0] = 0;
        S.move_tab = S.view;
        osk_open("New folder", "New folder", OSK_FILENAME, on_new_folder);
        return;
    case FM_DELETE:
        if (!s_folder_confirm) {
            s_folder_confirm = 1;
            int n;
            fill_folder_menu(menu_items(NULL), &n);
            return;
        }
        s_folder_confirm = 0;
        menu_close();
        groups_folder_delete(S.view, S.menu_folder);
        groups_changed("Folder deleted: its games are back in All");
        return;
    }
}

void folder_menu(const char *name)
{
    snprintf(S.menu_folder, sizeof(S.menu_folder), "%s", name);
    s_folder_confirm = 0;
    MenuItem items[4];
    int n, f = groups_folder_find(S.view, name), k = f >= 0 ? S.fcount[S.view][f] : 0;
    fill_folder_menu(items, &n);
    char sub[48];
    snprintf(sub, sizeof(sub), "Folder" DOT "%d %s", k, S.view == VIEW_HOMEBREW ? (k == 1 ? "app" : "apps") : k == 1 ? "game" : "games");
    menu_open(name, sub, items, n, folder_menu_cb);
}

/* Move to: id = MV_BASE + tab * MV_TAB + (folder + 1), or + MV_NEW for a new folder there. */
#define MV_BASE 100
#define MV_TAB 50
#define MV_NEW 49

static void move_cb(int id, int dir)
{
    if (dir || id < MV_BASE)
        return;
    int t = (id - MV_BASE) / MV_TAB, k = (id - MV_BASE) % MV_TAB;
    const char *here = groups_folder(S.move_path);
    if (k != MV_NEW && t == groups_tab(S.move_path, S.move_def) &&
        (k ? !strcasecmp(here, groups_folder_name(t, k - 1)) : !here[0])) {
        menu_close(); /* it is there already */
        return;
    }
    menu_close();
    opts_close();
    if (k == MV_NEW) {
        S.move_tab = t;
        osk_open("New folder", "New folder", OSK_FILENAME, on_new_folder);
        return;
    }
    const char *folder = k ? groups_folder_name(t, k - 1) : "";
    char msg[96];
    int r = groups_set(S.move_path, S.move_def, t, folder);
    S.move_path[0] = 0;
    if (r == -1)
        snprintf(msg, sizeof(msg), "Too many moved games (%d)", GROUP_ITEMS);
    else
        snprintf(msg, sizeof(msg), folder[0] ? "Moved to %s / %s" : "Moved to %s", tab_name(t), folder);
    groups_changed(msg);
}

void ui_open_move(const Entry *e)
{
    if (kind_is_picture(e->kind) || kind_is_music(e->kind) || kind_is_video(e->kind))
        return;
    snprintf(S.move_path, sizeof(S.move_path), "%s", e->path);
    S.move_def = def_tab(e);
    int cur = groups_tab(e->path, S.move_def);
    const char *cur_f = groups_folder(e->path);
    MenuItem items[MENU_MAX];
    int k = 0;
    for (int pass = 0; pass < 2; pass++) {
        int t = pass ? !cur : cur; /* this tab first, then the other one */
        for (int f = -1; f <= groups_folder_count(t) && k < MENU_MAX; f++) {
            MenuItem *it = &items[k++];
            memset(it, 0, sizeof(*it));
            it->gap = pass && f == -1;
            if (f == groups_folder_count(t)) {
                it->id = MV_BASE + t * MV_TAB + MV_NEW;
                snprintf(it->label, sizeof(it->label), "New folder in %s...", tab_name(t));
                continue;
            }
            it->id = MV_BASE + t * MV_TAB + f + 1;
            if (f < 0)
                snprintf(it->label, sizeof(it->label), "%s (no folder)", tab_name(t));
            else
                snprintf(it->label, sizeof(it->label), "  %s", groups_folder_name(t, f));
            if (t == cur && (f < 0 ? !cur_f[0] : !strcasecmp(cur_f, groups_folder_name(t, f))))
                snprintf(it->value, sizeof(it->value), "Here");
        }
    }
    menu_open("Move to", e->title, items, k, move_cb);
}

/* ------------------------------------------------ Music: sort and playlists */

/* What the focused Music row is (a song's or a playlist's path), to find it
 * again after the rows moved. */
void music_focus_key(char *out, int n)
{
    int s = S.sel[VIEW_MUSIC], k = disp_playlist(VIEW_MUSIC, s);
    const Entry *e = disp_entry(VIEW_MUSIC, s);
    snprintf(out, n, "%s", k >= 0 ? pl_path(k) : e ? e->path : "");
}

void music_refocus(const char *key)
{
    int at = -1;
    if (!in_pl())
        for (int k = 0; k < pl_count() && at < 0; k++)
            if (!strcmp(pl_path(k), key)) at = pl_row(k);
    if (at < 0) at = disp_find(VIEW_MUSIC, key);
    if (at < 0) at = S.sel[VIEW_MUSIC];
    if (S.view == VIEW_MUSIC) select_entry(at, 1);
    else S.sel[VIEW_MUSIC] = at;
}

/* The sheet's "Sort songs by": the next order, applied to the list at once. */
void ui_music_sort(int dir)
{
    char key[200];
    music_focus_key(key, sizeof(key));
    g_cfg.music_sort = (g_cfg.music_sort + dir + MSORT_COUNT) % MSORT_COUNT;
    library_resort();
    ui_library_changed();
    music_refocus(key);
    bg_post(job_save_ini, NULL, 0);
}

/* The sheet's "Sort games by" / "Sort apps by" for tab t (VIEW_GAMES / VIEW_HOMEBREW). */
void ui_game_sort(int t, int dir)
{
    char key[200] = "";
    const Entry *e = S.view == t ? disp_entry(t, S.sel[t]) : NULL;
    if (e) snprintf(key, sizeof(key), "%s", e->path);
    int *m = &g_cfg.game_sort[t == VIEW_HOMEBREW];
    *m = (*m + dir + GSORT_COUNT) % GSORT_COUNT;
    library_resort();
    ui_library_changed();
    int at = key[0] ? disp_find(t, key) : -1;
    if (at >= 0) select_entry(at, 1); /* stay on the same game */
    bg_post(job_save_ini, NULL, 0);
}

int ui_music_in_playlist(void) { return S.screen == SCR_LIST && S.view == VIEW_MUSIC && in_pl(); }

void open_playlist(int k)
{
    if (pl_open(k) != PL_OK) {
        ui_toast("Couldn't read the playlist");
        return;
    }
    snprintf(S.pl_back, sizeof(S.pl_back), "%s", pl_path(k));
    S.pl_shown = 1;
    S.top[VIEW_MUSIC] = 0;
    select_entry(0, 1);
}

/* Out of the open playlist, onto its row. */
void leave_playlist(void)
{
    pl_close();
    S.pl_shown = 0;
    S.top[VIEW_MUSIC] = 0;
    music_refocus(S.pl_back);
}

static const char *pl_error(int r)
{
    return r == PL_ERR_EXISTS ? "There's a playlist with that name already"
         : r == PL_ERR_NAME   ? "Type a name for the playlist"
         : r == PL_ERR_FULL   ? "That's as many as XLT can show"
         : r == PL_ERR_MEM    ? "Not enough memory for that"
                              : "Couldn't read the playlist";
}

static void on_new_playlist(const char *text)
{
    if (!text)
        return;
    char key[200];
    music_focus_key(key, sizeof(key));
    int r = pl_create(text, S.pl_with_song ? &S.pl_song : NULL);
    if (r < 0) {
        ui_toast(pl_error(r));
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), S.pl_with_song ? "Added to %s" : "Playlist %s made", pl_name(r));
    ui_toast(msg);
    if (S.pl_with_song) music_refocus(key); /* the rows moved: stay on the song */
    else if (S.view == VIEW_MUSIC && !in_pl()) select_entry(pl_row(r), 1); /* or go to the new playlist */
}

static void on_rename_playlist(const char *text)
{
    if (!text)
        return;
    int r = pl_rename(S.menu_pl, text);
    if (r < 0) {
        ui_toast(pl_error(r));
        return;
    }
    ui_toast("Playlist renamed");
    if (S.view == VIEW_MUSIC && !in_pl())
        select_entry(pl_row(r), 1);
}

/* Add to playlist: id = PA_BASE + playlist, or PA_NEW. */
#define PA_BASE 100
#define PA_NEW 99

static void add_cb(int id, int dir)
{
    if (dir || (id < PA_BASE && id != PA_NEW))
        return;
    menu_close();
    opts_close();
    if (id == PA_NEW) {
        S.pl_with_song = 1;
        osk_open("New playlist", "", OSK_FILENAME, on_new_playlist);
        return;
    }
    int k = id - PA_BASE, r = pl_add(k, &S.pl_song);
    char msg[96];
    if (r == 1) snprintf(msg, sizeof(msg), "It's in %s already", pl_name(k));
    else if (r == PL_ERR_FULL) snprintf(msg, sizeof(msg), "%s is full (%d songs)", pl_name(k), PL_SONGS);
    else if (r < 0) snprintf(msg, sizeof(msg), "%s", pl_error(r));
    else snprintf(msg, sizeof(msg), "Added to %s", pl_name(k));
    ui_toast(msg);
}

/* Add to playlist for an artist or album: the songs wait here until a playlist is picked. */
static Entry *s_many;
static int s_many_n;

static void many_done(int k, int r, int added)
{
    char msg[96];
    if (r == PL_ERR_FULL) snprintf(msg, sizeof(msg), "%s is full (%d songs)", pl_name(k), PL_SONGS);
    else if (r < 0) snprintf(msg, sizeof(msg), "%s", pl_error(r));
    else if (!added) snprintf(msg, sizeof(msg), "They're in %s already", pl_name(k));
    else snprintf(msg, sizeof(msg), "Added %d %s to %s", added, added == 1 ? "song" : "songs", pl_name(k));
    ui_toast(msg);
    free(s_many);
    s_many = NULL;
    s_many_n = 0;
}

static void on_new_playlist_many(const char *text)
{
    if (!text) {
        free(s_many);
        s_many = NULL;
        return;
    }
    int r = pl_create(text, NULL), added = 0;
    if (r < 0) {
        ui_toast(pl_error(r));
        free(s_many);
        s_many = NULL;
        return;
    }
    int k = r;
    r = pl_add_many(k, s_many, s_many_n, &added);
    many_done(k, r, added);
}

static void add_many_cb(int id, int dir)
{
    if (dir || (id < PA_BASE && id != PA_NEW))
        return;
    menu_close();
    if (id == PA_NEW) {
        osk_open("New playlist", "", OSK_FILENAME, on_new_playlist_many);
        return;
    }
    int k = id - PA_BASE, added = 0;
    int r = pl_add_many(k, s_many, s_many_n, &added);
    many_done(k, r, added);
}

void ui_open_add_many(const char *title, Entry *list, int n)
{
    free(s_many);
    s_many = list;
    s_many_n = n;
    MenuItem items[MENU_MAX];
    int k = 0, c = pl_count();
    if (c > MENU_MAX - 1) c = MENU_MAX - 1;
    for (int i = 0; i < c; i++, k++) {
        memset(&items[k], 0, sizeof(items[k]));
        items[k].id = PA_BASE + i;
        snprintf(items[k].label, sizeof(items[k].label), "%s", pl_name(i));
        snprintf(items[k].value, sizeof(items[k].value), "%d", pl_songs(i));
    }
    memset(&items[k], 0, sizeof(items[k]));
    items[k].id = PA_NEW;
    items[k].gap = k > 0;
    snprintf(items[k++].label, sizeof(items[0].label), "New playlist...");
    char sub[64];
    snprintf(sub, sizeof(sub), "%.40s" DOT "%d %s", title, n, n == 1 ? "song" : "songs");
    menu_open("Add to playlist", sub, items, k, add_many_cb);
}

void ui_open_add_to_playlist(const Entry *e)
{
    S.pl_song = *e;
    MenuItem items[MENU_MAX];
    int k = 0, n = pl_count();
    if (n > MENU_MAX - 1) n = MENU_MAX - 1;
    for (int i = 0; i < n; i++, k++) {
        memset(&items[k], 0, sizeof(items[k]));
        items[k].id = PA_BASE + i;
        snprintf(items[k].label, sizeof(items[k].label), "%s", pl_name(i));
        snprintf(items[k].value, sizeof(items[k].value), "%d", pl_songs(i));
    }
    memset(&items[k], 0, sizeof(items[k]));
    items[k].id = PA_NEW;
    items[k].gap = k > 0;
    snprintf(items[k++].label, sizeof(items[0].label), "New playlist...");
    menu_open("Add to playlist", e->title, items, k, add_cb);
}

/* Remove from playlist: the focused row if it is that song, else its first time in the list. */
void ui_playlist_remove(const Entry *e)
{
    int k = S.sel[VIEW_MUSIC];
    const Entry *at = pl_item(k);
    if (!at || strcmp(at->path, e->path))
        for (k = 0; k < pl_item_count() && strcmp(pl_item(k)->path, e->path); k++) {}
    if (k >= pl_item_count())
        return;
    char msg[96];
    snprintf(msg, sizeof(msg), pl_remove_item(k) == PL_OK ? "Removed from %s" : "Couldn't save %s", pl_name(pl_open_index()));
    ui_toast(msg);
    select_entry(S.sel[VIEW_MUSIC], 1); /* the next song moves up under the focus */
}

/* Move in playlist: the song's row is picked up; ↑/↓ carry it, ✕ (or ○) puts it down and saves. */
void ui_playlist_move(const Entry *e)
{
    int k = S.sel[VIEW_MUSIC];
    const Entry *at = pl_item(k);
    if (!at || strcmp(at->path, e->path))
        for (k = 0; k < pl_item_count() && strcmp(pl_item(k)->path, e->path); k++) {}
    if (k >= pl_item_count())
        return;
    select_entry(k, 1);
    S.pl_moving = 1;
}

/* list_input while a song is being moved; every button is taken. */
int playlist_move_input(unsigned int p)
{
    int s = S.sel[VIEW_MUSIC], dir = 0;
    if (p & (PSP_CTRL_UP | PSP_CTRL_LEFT)) dir = -1;
    else if (p & (PSP_CTRL_DOWN | PSP_CTRL_RIGHT)) dir = 1;
    if (dir) {
        int to = pl_move_item(s, dir);
        if (to >= 0) select_entry(to, 0); /* the row travels with the focus */
        return 1;
    }
    if (p & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) {
        S.pl_moving = 0;
        ui_toast(pl_save_open() == PL_OK ? "Order saved" : "Couldn't save the playlist");
    }
    return 1;
}

/* A playlist's △ menu. */
enum { PM_PLAY = 1, PM_OPEN, PM_RENAME, PM_NEW, PM_DELETE };

static void fill_pl_menu(MenuItem *it, int *n)
{
    int k = 0;
    memset(it, 0, sizeof(MenuItem) * 5);
    it[k].id = PM_PLAY; snprintf(it[k++].label, sizeof(it[0].label), "Play");
    it[k].id = PM_OPEN; snprintf(it[k++].label, sizeof(it[0].label), "Open");
    it[k].id = PM_RENAME; snprintf(it[k++].label, sizeof(it[0].label), "Rename...");
    it[k].id = PM_NEW; snprintf(it[k++].label, sizeof(it[0].label), "New playlist...");
    it[k].id = PM_DELETE; it[k].danger = 1; it[k].gap = 1;
    snprintf(it[k++].label, sizeof(it[0].label), S.pl_confirm ? "Press X again to delete" : "Delete playlist");
    *n = k;
}

static void pl_menu_cb(int id, int dir)
{
    if (dir)
        return;
    if (id != PM_DELETE)
        S.pl_confirm = 0;
    int k = S.menu_pl;
    switch (id) {
    case PM_PLAY:
        menu_close();
        if (pl_open(k) != PL_OK) { ui_toast("Couldn't read the playlist"); return; }
        if (pl_item_count() == 0) ui_toast("This playlist is empty");
        else pl_play(0);
        pl_close(); /* it was only read to be played */
        return;
    case PM_OPEN:
        menu_close();
        open_playlist(k);
        return;
    case PM_RENAME:
        menu_close();
        osk_open("Rename playlist", pl_name(k), OSK_FILENAME, on_rename_playlist);
        return;
    case PM_NEW:
        menu_close();
        S.pl_with_song = 0;
        osk_open("New playlist", "", OSK_FILENAME, on_new_playlist);
        return;
    case PM_DELETE:
        if (!S.pl_confirm) {
            S.pl_confirm = 1;
            int n;
            fill_pl_menu(menu_items(NULL), &n);
            return;
        }
        S.pl_confirm = 0;
        menu_close();
        pl_delete(k);
        ui_toast("Playlist deleted (the songs are kept)");
        select_entry(S.sel[VIEW_MUSIC], 1);
        return;
    }
}

void pl_menu(int k)
{
    S.menu_pl = k;
    S.pl_confirm = 0;
    MenuItem items[5];
    int n, c = pl_songs(k);
    fill_pl_menu(items, &n);
    char sub[48];
    snprintf(sub, sizeof(sub), "Playlist" DOT "%d %s", c, c == 1 ? "song" : "songs");
    menu_open(pl_name(k), sub, items, n, pl_menu_cb);
}

