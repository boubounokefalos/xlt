/* XLT - home screen (category cards + content row) and the library lists.
 *
 * Only the focused card and the focus ring move; everything else is flat
 * quads redrawn on demand. ui_update() reports whether a frame is needed,
 * so the launcher draws nothing at all while idle.
 */
#include <pspgu.h>
#include <pspkernel.h> /* sceKernelGetSystemTimeLow: Shuffle picks a first song */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "ui_internal.h"
#include "sysctl.h"
#include "opts.h"
#include "cc.h"
#include "osd.h"
#include "settings.h"
#include "gamecfg.h"
#include "safestart.h"
#include "search.h"
#include "viewer.h"
#include "music.h"
#include "video.h"
#include "imgdecode.h"
#include "anim.h"
#include "library.h"
#include "thumbs.h"
#include "history.h"
#include "fm.h"
#include "menu.h"
#include "osk.h"
#include "reader.h"
#include "web.h"
#include "preview.h"
#include "saver.h"
#include "boot.h"
#include "umd.h"
#include "sfx.h"
#include "bg.h"
#include "groups.h"
#include "playlists.h"
#include "nowplaying.h"
#include "savedata.h"
#include "musicsort.h"
#include "mbrowse.h"
#include "ui_state.h"
#include "recorder.h"
#include <stdlib.h>
#include <strings.h>

/* Placeholder tile colours for entries without an ICON0. */
static const u32 s_tints[] = {0x1f6f5c, 0x6e3b8e, 0x8a4b1f, 0x1f4f8a, 0x7a2a3a, 0x5c6b2a};

/* ----------------------------------------------------------------- layout */
#define MARGIN 12
#define FOOTER_Y 248

#define CARD_SMALL 46
#define CARD_BIG 72
#define CARD_GAP 12
#define CARD_CY 62
#define CARD_LABEL_Y 104

#define RECENT_LABEL_Y 126
#define TILE_Y 146
#define TILE_H 70
#define TILE_W 124
#define TILE_GAP 8
#define TILE_R 6
#define ALL_W (SCREEN_W - MARGIN * 2 - 3 * (TILE_W + TILE_GAP))

#define LIST_X MARGIN
#define LIST_W 216
#define LIST_Y 30
#define ROW_H 22
#define LIST_ROWS 9
#define PREVIEW_X 240
#define PREVIEW_W (SCREEN_W - MARGIN - PREVIEW_X)
#define PREVIEW_H (PREVIEW_W * 80 / 144) /* ICON0 aspect */

/* Grid view of a library list: 3 rows of tiles, the focused one's title below. */
#define GRID_Y 32
#define GRID_ROWS 3
#define GRID_VGAP 6
#define GRID_INFO_Y 224

#define TOAST_FRAMES 120

/* ------------------------------------------------------------- categories */

static const struct {
    const char *name;
    int icon;
    int view;          /* library view shown as a tile row, or -1 */
    const char *empty; /* message when there is nothing to show */
} s_cats[CAT_COUNT] = {
    {"UMD",      ICON_UMD,      -1, "The disc in the UMD drive"},
    {"Homebrew", ICON_HOMEBREW, VIEW_HOMEBREW, "No homebrew found in PSP/GAME"},
    {"Games",    ICON_GAMES,    VIEW_GAMES,    "No games found in ISO or PSP/GAME"},
    {"Music",    ICON_MUSIC,    VIEW_MUSIC, "No music found in MUSIC"},
    {"Video",    ICON_VIDEO,    VIEW_VIDEO, "No MP4 or PMF videos found in VIDEO"},
    {"Pictures", ICON_PICTURE,  VIEW_PICTURES, "No pictures found in PICTURE"},
    {"Files",    ICON_FOLDER,   -1, "Browse the Memory Stick"},
    {"Web",      -1,            -1, "Search and browse the web"}, /* its globe is drawn from the browser's icons */
    {"Settings", ICON_SETTINGS, -1, "XLT, ARK, plugins and PSP settings"},
};

static const char *s_view_name[VIEW_COUNT] = {"Games", "Homebrew", "Music", "Video", "Pictures", "Removed"};

/* ------------------------------------------------------------------ state */
UiState S;


static int cat_view(void) { return s_cats[S.cat].view; }

/* Tile rows: the library views, plus one for the Settings sections and one for Files. */
#define ROW_SETTINGS VIEW_COUNT
#define ROW_FILES (VIEW_COUNT + 1)
#define ROW_UMD (VIEW_COUNT + 2)
#define ROW_WEB (VIEW_COUNT + 3)
static int cat_row(void)
{
    return S.cat == CAT_SETTINGS ? ROW_SETTINGS : S.cat == CAT_FILES ? ROW_FILES : S.cat == CAT_UMD ? ROW_UMD
         : S.cat == CAT_WEB ? ROW_WEB : cat_view();
}
static int cat_shown(int c) { return c != CAT_UMD || umd_state() != UMD_NONE; }
/* The Files row: the storage and recent folders (two at most), then Save data, then Browse. */
static int files_places(void) { return fm_place_count() < 2 ? fm_place_count() : 2; }

static int recent_count(int r)
{
    return r == ROW_SETTINGS ? SETTINGS_RECENT : r == ROW_FILES ? files_places() + 1 : r == ROW_UMD ? 0
         : r == ROW_WEB ? web_tile_count()
         : library_recent_count(r);
}
static int tile_count(int r) { return recent_count(r) + 1; }

static void tile_rect(int r, int i, int *x, int *w)
{
    *x = MARGIN + i * (TILE_W + TILE_GAP);
    *w = i < recent_count(r) || r == ROW_UMD ? TILE_W : ALL_W; /* the UMD row is the one disc */
}

static void focus_tile(int i, int snap)
{
    int v = cat_row(), x, w;
    if (v < 0)
        return;
    if (i >= tile_count(v)) i = tile_count(v) - 1;
    if (i < 0) i = 0;
    if (!snap && i != S.tile[v]) sfx_play(SFX_MOVE);
    S.tile[v] = i;
    tile_rect(v, i, &x, &w);
    if (snap) {
        spring_snap(&S.ring_x, FX(x));
        spring_snap(&S.ring_w, FX(w));
    } else {
        spring_to(&S.ring_x, FX(x));
        spring_to(&S.ring_w, FX(w));
    }
}

#define CARDS_SHOWN 7

static void focus_card(int cat, int snap)
{
    if (!snap && cat != S.cat) sfx_play(SFX_CARD);
    S.cat = cat;
    int shown[CAT_COUNT], n = 0, at = 0;
    for (int i = 0; i < CAT_COUNT; i++)
        if (cat_shown(i)) {
            if (i == cat) at = n;
            shown[n++] = i;
        }
    /* the window: as before, unless the focus walks off one end */
    if (at < S.card_first) S.card_first = at;
    if (at >= S.card_first + CARDS_SHOWN) S.card_first = at - CARDS_SHOWN + 1;
    if (S.card_first > n - CARDS_SHOWN) S.card_first = n - CARDS_SHOWN;
    if (S.card_first < 0) S.card_first = 0;
    int in[CAT_COUNT] = {0};
    for (int k = S.card_first; k < n && k < S.card_first + CARDS_SHOWN; k++)
        in[shown[k]] = 1;
    /* where the row starts so the window is centred (from the sizes the cards are heading to) */
    int before = 0, width = -CARD_GAP;
    for (int k = 0; k < n; k++) {
        int w = (shown[k] == cat ? CARD_BIG : CARD_SMALL) + CARD_GAP;
        if (k < S.card_first) before += w;
        else if (k < S.card_first + CARDS_SHOWN) width += w;
    }
    int x0 = FX((SCREEN_W - width) / 2 - before);
    if (snap) spring_snap(&S.row_x, x0);
    else spring_to(&S.row_x, x0);
    for (int i = 0; i < CAT_COUNT; i++) {
        int t = FX(!cat_shown(i) ? 0 : i == cat ? CARD_BIG : CARD_SMALL); /* the UMD card grows in and out */
        int a = in[i] ? FX(1) : 0;
        if (snap) {
            spring_snap(&S.card[i], t);
            spring_snap(&S.card_a[i], a);
        } else {
            spring_to(&S.card[i], t);
            spring_to(&S.card_a[i], a);
        }
    }
    if (cat_row() >= 0)
        focus_tile(S.tile[cat_row()], 1); /* the ring belongs to the new row */
}

/* ------------------------------------------------- Games / Homebrew folders */

static int grouped(int v) { return v == VIEW_GAMES || v == VIEW_HOMEBREW; }
int def_tab(const Entry *e) { return kind_is_homebrew(e->kind) ? VIEW_HOMEBREW : VIEW_GAMES; }

void disp_build(int v)
{
    if (S.folder[v][0] && groups_folder_find(v, S.folder[v]) < 0)
        S.folder[v][0] = 0; /* it was deleted or renamed: back to the top level */
    int n = library_count(v), nf = groups_folder_count(v);
    free(S.disp[v]);
    S.disp[v] = malloc(sizeof(int) * (n + nf + 1));
    S.disp_n[v] = 0;
    memset(S.fcount[v], 0, sizeof(S.fcount[v]));
    if (!S.disp[v])
        return;
    int top = !S.folder[v][0];
    if (top)
        for (int f = 0; f < nf; f++)
            S.disp[v][S.disp_n[v]++] = -1 - f;
    for (int i = 0; i < n; i++) {
        const char *fo = groups_folder(library_get(v, i)->path);
        int f = fo[0] ? groups_folder_find(v, fo) : -1;
        if (f >= 0) S.fcount[v][f]++;
        if (top ? f < 0 : f >= 0 && !strcasecmp(fo, S.folder[v]))
            S.disp[v][S.disp_n[v]++] = i;
    }
}

/* Music: is a playlist open (shown instead of all the songs)? */
int in_pl(void) { return pl_open_index() >= 0; }

/* Music, level 0: the Artists and Albums rows come first (when there are songs). */
static int mb_top(void) { return S.mb_level == 0 && !in_pl() && library_count(VIEW_MUSIC) > 0 ? MB_KINDS : 0; }
int pl_row(int k) { return k + mb_top(); }

static const Entry *music_get(int i) { return library_get(VIEW_MUSIC, i); }

/* Rebuilds the Artists and Albums lists (after a scan or a new order) and finds the open
 * group again; if it went away, back up a level. */
static void mb_refresh(void)
{
    mb_build(music_get, library_count(VIEW_MUSIC));
    if (S.mb_level == 0)
        return;
    if (S.mb_level == 2) {
        int k = 0;
        for (; k < mb_count(S.mb_kind); k++)
            if (!strcmp(mb_group(S.mb_kind, k)->name, S.mb_name) && !strcmp(mb_group(S.mb_kind, k)->sub, S.mb_sub))
                break;
        if (k < mb_count(S.mb_kind)) {
            S.mb_group = k;
            S.mb_n = S.mb_idx ? mb_songs(S.mb_kind, k, S.mb_idx, library_count(VIEW_MUSIC)) : 0;
            return;
        }
        S.mb_level = 1;
    }
    if (S.mb_group >= mb_count(S.mb_kind))
        S.mb_group = mb_count(S.mb_kind) - 1;
    if (mb_count(S.mb_kind) == 0)
        S.mb_level = 0;
}

static int disp_count(int v)
{
    if (v == VIEW_MUSIC && S.mb_level == 1)
        return mb_count(S.mb_kind);
    if (v == VIEW_MUSIC && S.mb_level == 2)
        return S.mb_n;
    if (v == VIEW_MUSIC)
        return in_pl() ? pl_item_count() : mb_top() + pl_count() + library_count(v);
    return grouped(v) ? S.disp_n[v] : library_count(v);
}

/* The entry at row i, or NULL for a folder or playlist row. */
const Entry *disp_entry(int v, int i)
{
    if (v == VIEW_MUSIC) {
        if (S.mb_level == 1)
            return NULL;
        if (S.mb_level == 2)
            return i >= 0 && i < S.mb_n ? library_get(v, S.mb_idx[i]) : NULL;
        if (in_pl())
            return pl_item(i);
        i -= mb_top() + pl_count();
        return i >= 0 && i < library_count(v) ? library_get(v, i) : NULL;
    }
    if (!grouped(v))
        return library_get(v, i);
    if (i < 0 || i >= S.disp_n[v] || S.disp[v][i] < 0)
        return NULL;
    return library_get(v, S.disp[v][i]);
}

/* The folder at row i (index into groups), or -1. */
static int disp_folder(int v, int i)
{
    return grouped(v) && i >= 0 && i < S.disp_n[v] && S.disp[v][i] < 0 ? -1 - S.disp[v][i] : -1;
}

/* The playlist at row i (index into playlists.c), or -1. */
int disp_playlist(int v, int i)
{
    i -= mb_top();
    return v == VIEW_MUSIC && !in_pl() && S.mb_level == 0 && i >= 0 && i < pl_count() ? i : -1;
}

/* Music, level 0: MB_ARTISTS or MB_ALBUMS for the rows that open them, else -1. */
static int disp_mtop(int v, int i) { return v == VIEW_MUSIC && i >= 0 && i < mb_top() ? i : -1; }

/* Music, level 1: the artist or album at row i, else -1. */
static int disp_mgroup(int v, int i)
{
    return v == VIEW_MUSIC && S.mb_level == 1 && i >= 0 && i < mb_count(S.mb_kind) ? i : -1;
}

static const char *const s_mb_title[MB_KINDS] = {"Artists", "Albums"};
static void mb_line(char *line, int n, int kind, const MbGroup *g);
static const char *mb_label(const MbGroup *g) { return g->name[0] ? g->name : S.mb_kind == MB_ALBUMS ? "Unknown album" : "Unknown artist"; }

static void mb_open_kind(int kind)
{
    S.mb_kind = kind;
    S.mb_level = 1;
    S.top[VIEW_MUSIC] = 0;
    select_entry(0, 1);
}

static void mb_open_group(int k)
{
    const MbGroup *g = mb_group(S.mb_kind, k);
    if (!g)
        return;
    int n = library_count(VIEW_MUSIC);
    int *idx = realloc(S.mb_idx, sizeof(int) * (n ? n : 1));
    if (!idx) {
        ui_toast("Not enough memory");
        return;
    }
    S.mb_idx = idx;
    S.mb_n = mb_songs(S.mb_kind, k, idx, n);
    S.mb_group = k;
    snprintf(S.mb_name, sizeof(S.mb_name), "%s", g->name);
    snprintf(S.mb_sub, sizeof(S.mb_sub), "%s", g->sub);
    S.mb_level = 2;
    S.top[VIEW_MUSIC] = 0;
    select_entry(0, 1);
}

/* ○: one level up, onto the row we came from. */
static void mb_back(void)
{
    S.top[VIEW_MUSIC] = 0;
    if (S.mb_level == 2) {
        S.mb_level = 1;
        select_entry(S.mb_group, 1);
    } else {
        S.mb_level = 0;
        select_entry(S.mb_kind, 1);
    }
}

/* ✕ on a song of an open artist or album: play from it to the end of the group. */
static int mb_play(const Entry *e)
{
    int k = 0;
    while (k < S.mb_n && strcmp(library_get(VIEW_MUSIC, S.mb_idx[k])->path, e->path))
        k++;
    if (k >= S.mb_n)
        return 0;
    Entry *list = malloc(sizeof(Entry) * S.mb_n);
    if (!list) {
        ui_toast("Not enough memory");
        return 1;
    }
    for (int i = 0; i < S.mb_n; i++)
        list[i] = *library_get(VIEW_MUSIC, S.mb_idx[i]);
    music_play_entries(list, S.mb_n, k); /* the player copies the list */
    free(list);
    return 1;
}

/* The songs of artist or album k (of S.mb_kind), in play order, as a new list (the caller frees it). */
static Entry *mb_entries(int k, int *n)
{
    int total = library_count(VIEW_MUSIC);
    int *idx = malloc(sizeof(int) * (total ? total : 1));
    Entry *list = idx ? malloc(sizeof(Entry) * (total ? total : 1)) : NULL;
    *n = 0;
    if (list) {
        *n = mb_songs(S.mb_kind, k, idx, total);
        for (int i = 0; i < *n; i++)
            list[i] = *library_get(VIEW_MUSIC, idx[i]);
    }
    free(idx);
    if (list && !*n) {
        free(list);
        list = NULL;
    }
    return list;
}

/* △ on an artist or album. */
enum { MBM_PLAY = 1, MBM_SHUFFLE, MBM_OPEN, MBM_ADD };
static int s_mb_menu;

static void mb_menu_cb(int id, int dir)
{
    if (dir)
        return;
    int k = s_mb_menu, n;
    menu_close();
    if (id == MBM_OPEN) {
        mb_open_group(k);
        return;
    }
    Entry *list = mb_entries(k, &n);
    if (!list) {
        ui_toast("Not enough memory");
        return;
    }
    if (id == MBM_ADD) {
        ui_open_add_many(mb_label(mb_group(S.mb_kind, k)), list, n); /* it keeps the list */
        return;
    }
    if (id == MBM_SHUFFLE) {
        if (!music_shuffle()) {
            music_set_shuffle(1);
            ui_toast("Shuffle on");
        }
        music_play_entries(list, n, (int)(sceKernelGetSystemTimeLow() % (unsigned)n)); /* a random first song */
    } else {
        music_play_entries(list, n, 0);
    }
    free(list);
}

static void mb_menu(int k)
{
    const MbGroup *g = mb_group(S.mb_kind, k);
    if (!g)
        return;
    s_mb_menu = k;
    static const char *const labels[] = {"Play", "Shuffle", "Open", "Add to playlist..."};
    MenuItem items[4];
    memset(items, 0, sizeof(items));
    for (int i = 0; i < 4; i++) {
        items[i].id = MBM_PLAY + i;
        snprintf(items[i].label, sizeof(items[i].label), "%s", labels[i]);
    }
    items[3].gap = 1;
    char sub[64];
    mb_line(sub, sizeof(sub), S.mb_kind, g);
    menu_open(mb_label(g), sub, items, 4, mb_menu_cb);
}

int disp_find(int v, const char *path)
{
    for (int i = 0; i < disp_count(v); i++) {
        const Entry *e = disp_entry(v, i);
        if (e && !strcmp(e->path, path))
            return i;
    }
    return -1;
}

int disp_find_folder(int v, const char *name)
{
    for (int i = 0; i < disp_count(v); i++) {
        int f = disp_folder(v, i);
        if (f >= 0 && !strcasecmp(groups_folder_name(v, f), name))
            return i;
    }
    return -1;
}

/* Views that can be shown as a grid (all but Removed), and whether one is. */
static int grid_able(int v) { return v >= 0 && v < VIEW_COUNT && v != VIEW_HIDDEN; }
static int is_grid(int v) { return grid_able(v) && (g_cfg.grid >> v & 1); }

typedef struct { int cols, w, h, gap, x0; } GridGeom;

static GridGeom grid_geom(int v)
{
    GridGeom g;
    if (v == VIEW_MUSIC) { g.cols = 7; g.w = 58; g.h = 58; g.gap = 8; } /* square covers */
    else { g.cols = 4; g.w = 104; g.h = 58; g.gap = 8; }                /* ICON0 aspect */
    g.x0 = (SCREEN_W - (g.cols * g.w + (g.cols - 1) * g.gap)) / 2;
    return g;
}

static void grid_select(int i, int snap)
{
    int v = S.view, n = disp_count(v);
    GridGeom g = grid_geom(v);
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    if (!snap && i != S.sel[v]) sfx_play(SFX_MOVE);
    S.sel[v] = i;
    int row = i / g.cols, rows = (n + g.cols - 1) / g.cols;
    int top = S.top[v];
    if (row < top) top = row;
    if (row >= top + GRID_ROWS) top = row - GRID_ROWS + 1;
    if (top > rows - GRID_ROWS) top = rows - GRID_ROWS;
    if (top < 0) top = 0;
    S.top[v] = top;
    if (snap) {
        spring_snap(&S.hl, FX(row));
        spring_snap(&S.gcol, FX(i % g.cols));
        spring_snap(&S.scroll, FX(top));
    } else {
        spring_to(&S.hl, FX(row));
        spring_to(&S.gcol, FX(i % g.cols));
        spring_to(&S.scroll, FX(top));
    }
}

void select_entry(int i, int snap)
{
    int v = S.view, n = disp_count(v);
    if (is_grid(v)) {
        grid_select(i, snap);
        return;
    }
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    if (!snap && i != S.sel[v]) sfx_play(SFX_MOVE);
    S.sel[v] = i;
    if (S.sel[v] < S.top[v]) S.top[v] = S.sel[v];
    if (S.sel[v] >= S.top[v] + LIST_ROWS) S.top[v] = S.sel[v] - LIST_ROWS + 1;
    if (S.top[v] > 0 && S.top[v] > n - LIST_ROWS) S.top[v] = n > LIST_ROWS ? n - LIST_ROWS : 0;
    if (snap) {
        spring_snap(&S.hl, FX(S.sel[v]));
        spring_snap(&S.scroll, FX(S.top[v]));
    } else {
        spring_to(&S.hl, FX(S.sel[v]));
        spring_to(&S.scroll, FX(S.top[v]));
    }
}

static void open_list(int view, const Entry *focus)
{
    S.view = view;
    S.screen = SCR_LIST;
    spring_to(&S.trans, FX(1));
    if (focus && view == VIEW_MUSIC && S.mb_level) /* a song found elsewhere: shown in the whole list */
        S.mb_level = 0;
    if (focus && grouped(view)) { /* show it where it is: in its folder */
        snprintf(S.folder[view], sizeof(S.folder[view]), "%s", groups_folder(focus->path));
        disp_build(view);
    }
    int i = focus ? disp_find(view, focus->path) : S.sel[view];
    select_entry(i < 0 ? 0 : i, 1);
}

static void toast(const char *msg)
{
    S.toast = msg;
    S.toast_left = TOAST_FRAMES;
}

void ui_toast(const char *msg)
{
    snprintf(S.toast_buf, sizeof(S.toast_buf), "%s", msg);
    toast(S.toast_buf);
}

void ui_request_launch(const Entry *e)
{
    if (kind_is_picture(e->kind)) { /* "launching" a picture means viewing it */
        viewer_open(e);
        return;
    }
    if (kind_is_video(e->kind)) {
        video_open(e);
        return;
    }
    if (kind_is_music(e->kind)) { /* and a song means playing it, from there on down the list */
        if (S.screen == SCR_LIST && S.view == VIEW_MUSIC && S.mb_level == 2 && mb_play(e)) /* ...or the open artist or album */
            return;
        if (S.screen == SCR_LIST && S.view == VIEW_MUSIC && in_pl()) { /* ...or down the open playlist */
            int k = S.sel[VIEW_MUSIC];
            const Entry *at = pl_item(k);
            if (!at || strcmp(at->path, e->path))
                for (k = 0; k < pl_item_count() && strcmp(pl_item(k)->path, e->path); k++) {}
            if (k < pl_item_count()) {
                pl_play(k);
                return;
            }
        }
        int i = library_find(VIEW_MUSIC, e->path);
        if (i >= 0)
            music_play_view(i);
        return;
    }
    sfx_play(SFX_LAUNCH); /* the logo's notes; main waits for them before the game starts */
    S.launch = *e;
    S.launch_pending = 1;
    snprintf(S.toast_buf, sizeof(S.toast_buf), "Starting %.80s...", e->title);
    toast(S.toast_buf);
}

void ui_request_exit(void) { S.exit_requested = 1; }

void ui_open_removed(void)
{
    focus_card(CAT_SETTINGS, 1); /* back from the list lands on the Settings row */
    open_list(VIEW_HIDDEN, NULL);
}

void ui_open_music(void)
{
    if (search_is_open() || viewer_is_open() || video_is_open() || fm_is_open() || reader_is_open() || web_is_open()) {
        toast("Pick a song in Music to start playing");
        return;
    }
    focus_card(CAT_MUSIC, 1);
    open_list(VIEW_MUSIC, NULL);
}
int ui_take_exit(void) { return S.exit_requested; }

int ui_take_launch(Entry *out)
{
    if (!S.launch_pending)
        return 0;
    S.launch_pending = 0;
    *out = S.launch;
    return 1;
}

void ui_prefetch(void)
{
    if (umd_state() == UMD_GAME)
        thumbs_get(umd_entry());
    for (int c = 0; c < CAT_COUNT; c++) { /* every tab's Recent row: the first card shown, then the rest */
        int v = s_cats[(S.cat + c) % CAT_COUNT].view;
        if (v < 0)
            continue;
        for (int i = 0; i < library_recent_count(v); i++)
            thumbs_get(library_recent(v, i));
    }
}

void ui_init(void)
{
    S.screen = SCR_HOME;
    spring_snap(&S.trans, 0);
    focus_card(CAT_GAMES, 1);
    spring_snap(&S.hl, 0);
    spring_snap(&S.scroll, 0);
    S.scanning = library_scanning();
    disp_build(VIEW_GAMES);
    disp_build(VIEW_HOMEBREW);
    mb_refresh(); /* the songs of the cached index, until a scan brings new ones */
    pl_refresh();
}

/* The library was replaced by a finished scan: keep focus where it makes sense. */
void ui_library_changed(void)
{
    search_refresh();
    viewer_refresh();
    if (cat_view() >= 0)
        focus_tile(S.tile[cat_view()], 1);
    disp_build(VIEW_GAMES);
    disp_build(VIEW_HOMEBREW);
    mb_refresh();
    for (int v = 0; v < VIEW_COUNT; v++) {
        int n = disp_count(v);
        if (S.sel[v] >= n) S.sel[v] = n ? n - 1 : 0;
    }
    select_entry(S.sel[S.view], 1);
}

/* ------------------------------------------------------------------ input */

/* START: play/pause, or (nothing playing) carry on where the music stopped last time. */
static void start_button(void)
{
    const char *title;
    if (music_state() != MUSIC_STOPPED) {
        music_toggle();
    } else if (music_can_resume(&title, NULL)) {
        if (music_resume() == 0) {
            snprintf(S.toast_buf, sizeof(S.toast_buf), "Carrying on: %.80s", title);
            toast(S.toast_buf);
        } else {
            toast("The songs from last time aren't there any more");
        }
    }
}

/* 1 if e is the song playing (or paused) now. */
static int is_playing(const Entry *e)
{
    MusicNow now;
    return e && music_state() != MUSIC_STOPPED && music_now(&now) == 0 && !strcmp(now.path, e->path);
}

static int home_input(unsigned int p)
{
    if (p & PSP_CTRL_LTRIGGER && S.cat > 0 && cat_shown(S.cat - 1)) { focus_card(S.cat - 1, 0); return 1; }
    if (p & PSP_CTRL_RTRIGGER && S.cat < CAT_COUNT - 1) { focus_card(S.cat + 1, 0); return 1; }
    if (S.cat == CAT_UMD) {
        int st = umd_state();
        if (p & PSP_CTRL_CROSS) {
            if (st == UMD_GAME) ui_request_launch(umd_entry());
            else if (st == UMD_VIDEO) toast("UMD Video plays from the XMB");
            else if (st == UMD_READING) toast("Reading the disc...");
            else toast("This disc can't be read");
            return 1;
        }
        if (p & PSP_CTRL_TRIANGLE) {
            if (st == UMD_GAME) opts_open(umd_entry());
            return 1;
        }
    }

    if (p & PSP_CTRL_SQUARE) { search_open(); return 1; }
    if (p & PSP_CTRL_START) { start_button(); return 1; }

    int v = cat_view();
    if (v >= 0 && library_count(v) > 0) {
        int t = S.tile[v];
        if (p & PSP_CTRL_LEFT && t > 0) { focus_tile(t - 1, 0); return 1; }
        if (p & PSP_CTRL_RIGHT && t < tile_count(v) - 1) { focus_tile(t + 1, 0); return 1; }
        if (p & PSP_CTRL_CROSS) {
            if (t == tile_count(v) - 1)
                open_list(v, NULL);
            else if (v == VIEW_MUSIC && is_playing(library_recent(v, t)))
                np_open();
            else
                ui_request_launch(library_recent(v, t));
            return 1;
        }
        if (p & PSP_CTRL_TRIANGLE) {
            if (t < tile_count(v) - 1)
                opts_open(library_recent(v, t));
            else
                toast("Pick a game first");
            return 1;
        }
    } else if (S.cat == CAT_SETTINGS) {
        int t = S.tile[ROW_SETTINGS];
        if (p & PSP_CTRL_LEFT && t > 0) { focus_tile(t - 1, 0); return 1; }
        if (p & PSP_CTRL_RIGHT && t < SETTINGS_RECENT) { focus_tile(t + 1, 0); return 1; }
        if (p & PSP_CTRL_CROSS) {
            if (t < SETTINGS_RECENT) {
                settings_open_section(settings_recent(t));
                focus_tile(0, 1); /* that section is first in the row now */
            } else {
                settings_open();
            }
            return 1;
        }
        if (p & PSP_CTRL_TRIANGLE) return 1;
    } else if (S.cat == CAT_WEB) {
        int t = S.tile[ROW_WEB], n = web_tile_count();
        if (t > n) t = n;
        if (p & PSP_CTRL_LEFT && t > 0) { focus_tile(t - 1, 0); return 1; }
        if (p & PSP_CTRL_RIGHT && t < n) { focus_tile(t + 1, 0); return 1; }
        if (p & PSP_CTRL_CROSS) {
            web_tile_open(t < n ? t : -1);
            return 1;
        }
        if (p & PSP_CTRL_TRIANGLE) return 1;
    } else if (S.cat == CAT_FILES) {
        int t = S.tile[ROW_FILES], n = files_places();
        if (t > n + 1) t = n + 1;
        if (p & PSP_CTRL_LEFT && t > 0) { focus_tile(t - 1, 0); return 1; }
        if (p & PSP_CTRL_RIGHT && t < n + 1) { focus_tile(t + 1, 0); return 1; }
        if (p & PSP_CTRL_CROSS) {
            if (t == n) sd_open(); /* Save data */
            else fm_open(t < n ? fm_place_path(t) : NULL);
            return 1;
        }
        if (p & PSP_CTRL_TRIANGLE) return 1;
    } else if (p & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)) {
        toast("Nothing here yet");
        return 1;
    }
    return 0;
}

static void job_save_ini(void *arg) { globalcfg_save(); }

static int list_input(unsigned int p)
{
    int s = S.sel[S.view];
    if (S.pl_moving) {
        if (S.view == VIEW_MUSIC && in_pl())
            return playlist_move_input(p);
        S.pl_moving = 0; /* the playlist went away under it */
    }
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER) && grid_able(S.view)) { /* list <-> grid */
        g_cfg.grid ^= 1 << S.view;
        S.top[S.view] = 0;
        select_entry(s, 1);
        bg_post(job_save_ini, NULL, 0);
        return 1;
    }
    if (is_grid(S.view)) {
        int cols = grid_geom(S.view).cols, n = disp_count(S.view);
        if (p & PSP_CTRL_UP) { if (s >= cols) select_entry(s - cols, 0); return 1; }
        if (p & PSP_CTRL_DOWN) {
            if (s + cols < n) select_entry(s + cols, 0);
            else if (s / cols < (n - 1) / cols) select_entry(n - 1, 0); /* into a shorter last row */
            return 1;
        }
        if (p & PSP_CTRL_LEFT) { select_entry(s - 1, 0); return 1; }
        if (p & PSP_CTRL_RIGHT) { select_entry(s + 1, 0); return 1; }
    }
    if (p & PSP_CTRL_UP) { select_entry(s - 1, 0); return 1; }
    if (p & PSP_CTRL_DOWN) { select_entry(s + 1, 0); return 1; }
    if (p & PSP_CTRL_LEFT) { select_entry(s - LIST_ROWS, 0); return 1; }
    if (p & PSP_CTRL_RIGHT) { select_entry(s + LIST_ROWS, 0); return 1; }
    if (p & PSP_CTRL_CIRCLE && grouped(S.view) && S.folder[S.view][0]) { /* out of the folder, onto it */
        char was[GROUP_NAME];
        snprintf(was, sizeof(was), "%s", S.folder[S.view]);
        S.folder[S.view][0] = 0;
        disp_build(S.view);
        S.top[S.view] = 0;
        int at = disp_find_folder(S.view, was);
        select_entry(at < 0 ? 0 : at, 1);
        return 1;
    }
    if (p & PSP_CTRL_CIRCLE && S.view == VIEW_MUSIC && in_pl()) { /* out of the playlist, onto it */
        leave_playlist();
        return 1;
    }
    if (p & PSP_CTRL_CIRCLE && S.view == VIEW_MUSIC && S.mb_level) { /* up from an artist or album */
        mb_back();
        return 1;
    }
    if (p & PSP_CTRL_CIRCLE) {
        S.screen = SCR_HOME;
        spring_to(&S.trans, 0);
        return 1;
    }
    if (p & PSP_CTRL_START) { start_button(); return 1; }
    if (p & PSP_CTRL_SQUARE && S.view != VIEW_HIDDEN && S.view != VIEW_PICTURES) { search_open(); return 1; }
    if (disp_count(S.view) == 0)
        return 0;
    int mt = disp_mtop(S.view, s), mg = disp_mgroup(S.view, s);
    if (mt >= 0 && p & PSP_CTRL_CROSS) { mb_open_kind(mt); return 1; }
    if (mg >= 0 && p & PSP_CTRL_CROSS) { mb_open_group(mg); return 1; }
    if (mg >= 0 && p & PSP_CTRL_TRIANGLE) { mb_menu(mg); return 1; }
    if (mt >= 0 || mg >= 0)
        return 0;
    int pk = disp_playlist(S.view, s);
    if (pk >= 0 && p & PSP_CTRL_CROSS) { open_playlist(pk); return 1; }
    if (pk >= 0 && p & PSP_CTRL_TRIANGLE) { pl_menu(pk); return 1; }
    if (pk >= 0)
        return 0;
    int fo = disp_folder(S.view, s);
    if (fo >= 0 && p & PSP_CTRL_CROSS) { enter_folder(groups_folder_name(S.view, fo)); return 1; }
    if (fo >= 0 && p & PSP_CTRL_TRIANGLE) { folder_menu(groups_folder_name(S.view, fo)); return 1; }
    if (fo >= 0)
        return 0;
    if (p & PSP_CTRL_CROSS && S.view == VIEW_HIDDEN) {
        hidden_set(library_get(S.view, s)->path, 0);
        library_resort();
        ui_library_changed();
        toast("Restored to the library");
        return 1;
    }
    if (p & PSP_CTRL_CROSS && S.view == VIEW_MUSIC && is_playing(disp_entry(S.view, s))) { np_open(); return 1; }
    if (p & PSP_CTRL_CROSS) { ui_request_launch(disp_entry(S.view, s)); return 1; }
    if (p & PSP_CTRL_TRIANGLE) { opts_open(disp_entry(S.view, s)); return 1; }
    return 0;
}

/* The game or video whose preview may play: the focused tile or list row,
 * when nothing covers it. */
static int previewable(int v) { return v == VIEW_GAMES || v == VIEW_HOMEBREW || v == VIEW_VIDEO; }

static const Entry *preview_entry(void)
{
    if (opts_is_open() || cc_is_open() || settings_is_open() || search_is_open() || viewer_is_open() ||
        video_is_open() || reader_is_open() || web_is_open() || menu_is_open() || osk_is_open() || S.launch_pending ||
        np_is_open() || sd_is_open() ||
        saver_is_open() || cc_usb_screen())
        return NULL;
    if (fm_is_open())
        return fm_preview_entry(); /* the focused file, if it's a video or a game */
    if (S.screen == SCR_HOME && S.trans.x == 0 && S.cat == CAT_UMD) {
        return umd_state() == UMD_GAME ? umd_entry() : NULL; /* the disc's ICON1 */
    } else if (S.screen == SCR_HOME && S.trans.x == 0) {
        int v = cat_view();
        if (v >= 0 && previewable(v) && S.tile[v] < library_recent_count(v))
            return library_recent(v, S.tile[v]);
    } else if (S.screen == SCR_LIST && S.trans.x == FX(1) && previewable(S.view) && disp_count(S.view)) {
        return disp_entry(S.view, S.sel[S.view]); /* NULL on a folder */
    }
    return NULL;
}

static int step(Spring *s)
{
    int old = s->x;
    spring_step(s);
    return s->x != old;
}

int ui_update(const Input *in, int status_changed)
{
    int dirty = status_changed;

    if (library_poll()) {
        ui_library_changed();
        pl_refresh(); /* the scan may have brought new playlists */
        dirty = 1;
    }
    {
        char key[200];
        int err = 0;
        music_focus_key(key, sizeof(key));
        if (pl_poll(&err)) { /* the playlists were reread: the songs below them may have moved */
            music_refocus(key);
            dirty = 1;
        }
        if (S.pl_shown && !in_pl()) { /* the open one is gone (deleted from the PC, say) */
            pl_close();
            S.pl_shown = 0;
            music_refocus(key);
            dirty = 1;
        }
        if (err) {
            toast("Couldn't save the playlist");
            dirty = 1;
        }
    }
    if (umd_poll()) { /* a disc went in or out: the UMD card grows (and takes the focus) or goes */
        int st = umd_state();
        if (S.umd_seen == UMD_NONE && st != UMD_NONE)
            focus_card(CAT_UMD, 0);
        else if (!cat_shown(S.cat))
            focus_card(CAT_HOMEBREW, 0);
        else
            focus_card(S.cat, 0);
        S.umd_seen = st;
        dirty = 1;
    }
    if (S.music_gen != music_generation()) { /* track or play state changed */
        S.music_gen = music_generation();
        if (music_state() == MUSIC_STOPPED)
            power_hold(0); /* the list ended or was stopped: back to the idle clock */
        dirty = 1;
    }
    if (S.scanning != library_scanning()) {
        S.scanning = library_scanning();
        dirty = 1;
    }
    dirty |= thumbs_pump();

    unsigned int p = S.launch_pending ? 0 : in->pressed;
    if (p) {
        dirty = 1; /* the sheets and screens all react to some button */
        if (!video_is_open() && !saver_is_open()) { /* the buttons' own sounds; what they do may outrank them */
            if (p & PSP_CTRL_CROSS) sfx_play(SFX_SELECT);
            else if (p & PSP_CTRL_CIRCLE) sfx_play(SFX_BACK);
            else if (p & PSP_CTRL_TRIANGLE) sfx_play(SFX_OPEN);
        }
        if (sysctl_usb_active()) {
            cc_usb_input(p); /* the USB mode screen; nothing underneath reacts */
        } else if (cc_is_open() && cc_input(p)) {
        } else if (osk_is_open()) {
            osk_input(p); /* SELECT switches its layout */
        } else if (p & PSP_CTRL_SELECT && !opts_is_open() && !menu_is_open()) {
            cc_toggle();
        } else if (menu_is_open()) {
            menu_input(p);
        } else if (opts_is_open()) {
            opts_input(p);
        } else if (np_is_open()) {
            np_input(p);
        } else if (sd_is_open()) {
            sd_input(p);
        } else if (settings_is_open()) {
            settings_input(p);
        } else if (video_is_open()) {
            video_input(p);
        } else if (viewer_is_open()) {
            viewer_input(p);
        } else if (reader_is_open()) {
            reader_input(p);
        } else if (web_is_open()) {
            web_input(p);
        } else if (fm_is_open()) {
            fm_input(p);
        } else if (search_is_open()) {
            search_input(p);
        } else if (S.screen == SCR_HOME) {
            home_input(p);
        } else {
            list_input(p);
        }
    }
    dirty |= opts_step();
    dirty |= np_step();
    dirty |= sd_step();
    music_poll(); /* saves where the music is, for next time */
    dirty |= osd_step();
    dirty |= settings_step();
    dirty |= search_step();
    if (viewer_is_open())
        viewer_hold(in->held);
    dirty |= viewer_step();
    dirty |= video_step();
    dirty |= fm_step();
    dirty |= reader_step();
    dirty |= web_step();
    dirty |= menu_step();
    dirty |= osk_step();
    dirty |= cc_step();
    preview_target(preview_entry());
    dirty |= preview_step();

    dirty |= step(&S.trans);
    for (int i = 0; i < CAT_COUNT; i++) {
        dirty |= step(&S.card[i]);
        dirty |= step(&S.card_a[i]);
    }
    dirty |= step(&S.row_x);
    dirty |= step(&S.ring_x);
    dirty |= step(&S.ring_w);
    dirty |= step(&S.hl);
    dirty |= step(&S.scroll);
    dirty |= step(&S.gcol);

    if (S.toast_left > 0 && --S.toast_left == 0) {
        S.toast = NULL;
        dirty = 1;
    }
    sfx_commit(); /* the one sound this frame asked for */
    return dirty;
}

/* ------------------------------------------------------------- formatting */

const char *kind_name(int k)
{
    static const char *n[] = {"ISO", "CSO", "ZSO", "PSN", "PS1", "Homebrew", "Picture", "Music", "Video", "UMD"};
    return k < 10 ? n[k] : "?";
}

const char *region_name(const char *id)
{
    if (strlen(id) != 9)
        return NULL;
    switch (id[2]) {
    case 'U': return "US";
    case 'E': return "EU";
    case 'J': return "JP";
    case 'A': return "Asia";
    case 'K': return "KR";
    case 'H': return "HK";
    default: return NULL;
    }
}

void format_date(char *b, int n, uint32_t m)
{
    /* the scanner's packed mtime: see pack_time() in library.c */
    snprintf(b, n, "%04u-%02u-%02u %02u:%02u", (unsigned)(m >> 25) + 1980, (unsigned)(m >> 21) & 15,
             (unsigned)(m >> 16) & 31, (unsigned)(m >> 11) & 31, (unsigned)(m >> 5) & 63);
}

void format_size(char *b, int n, uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        unsigned tenths = (unsigned)(bytes * 10 / (1024ull * 1024 * 1024));
        snprintf(b, n, "%u.%u GB", tenths / 10, tenths % 10);
    } else if (bytes >= 1024 * 1024) {
        snprintf(b, n, "%u MB", (unsigned)(bytes / (1024 * 1024)));
    } else {
        snprintf(b, n, "%u KB", (unsigned)((bytes + 1023) / 1024));
    }
}

/* ------------------------------------------------------------------- draw */

void ui_draw_battery(int x, int y, const SysStatus *st)
{
    u32 c = st->charging ? C_CHARGE : st->battery < 15 ? C_LOW : C_MUTED;
    const int w = 20, h = 10;
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y + 1, 1, h - 2, c);
    gfx_rect(x + w - 1, y + 1, 1, h - 2, c);
    gfx_rect(x + w, y + 3, 2, h - 6, c);
    int fill = (w - 4) * st->battery / 100;
    gfx_rect(x + 2, y + 2, fill, h - 4, c);
}

static void draw_status_bar(const char *title, int search, const SysStatus *st, int ox);

void ui_draw_status_bar(const char *title, const SysStatus *st)
{
    draw_status_bar(title, 0, st, 0);
}

static void draw_status_bar(const char *title, int search, const SysStatus *st, int ox)
{
    char buf[16];
    int tx = gfx_text(FONT_SMALL, MARGIN + ox, 5, title, C_MUTED);
    if (S.scanning)
        tx = gfx_text(FONT_SMALL, tx + 8, 5, "Scanning...", C_FOCUS);
    if (g_safe_start)
        tx = gfx_text(FONT_SMALL, tx + 8, 5, "Safe start", C_FOCUS);
    MusicNow now;
    if (music_state() != MUSIC_STOPPED && music_now(&now) == 0 && !np_is_open()) { /* Now Playing says it already */
        int paused = music_state() == MUSIC_PAUSED;
        u32 c = paused ? C_MUTED : C_FOCUS;
        gfx_icon(ICON_MUSIC, tx + 10, 6, 13, 13, c);
        char buf[96];
        snprintf(buf, sizeof(buf), paused ? "%s (paused)" : "%s", now.title);
        gfx_text_fit(FONT_SMALL, tx + 26, 5, 190 - tx > 60 ? 190 - tx : 60, buf, c);
    }

    format_clock(buf, sizeof(buf), st->hour, st->minute);
    int x = SCREEN_W - MARGIN + ox - gfx_text_width(FONT_SMALL, buf);
    gfx_text(FONT_SMALL, x, 5, buf, C_TEXT);
    x -= 8;
    if (st->battery >= 0) {
        x -= 22;
        ui_draw_battery(x, 9, st);
        x -= 8;
    }
    if (st->wifi_on) {
        x -= 14;
        gfx_icon(ICON_WIFI, x, 6, 14, 14, C_MUTED);
        x -= 8;
    }
    if (search) {
        x -= 14;
        gfx_icon(ICON_SEARCH, x, 7, 13, 13, C_MUTED);
    }
}

static const Hint s_home_hints[] = {
    {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open"},
    {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
    {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Search"},
    {-1, "L/R", 0, "Category"},
    {-1, "SELECT", 0, "Control"},
};
static const Hint s_music_hints[] = {
    {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Play"},
    {-1, "START", 0, "Pause"},
    {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
    {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Search"},
    {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
};
static const Hint s_picture_hints[] = {
    {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "View"},
    {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
};
static const Hint s_hidden_hints[] = {
    {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Restore"},
    {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
};
static const Hint s_list_hints[] = {
    {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Play"},
    {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
    {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Search"},
};

#define HINT_GAP 16
#define HINT_ICON 12

static int hint_width(const Hint *h)
{
    int w = h->icon >= 0 ? HINT_ICON : gfx_text_width(FONT_SMALL, h->key);
    return w + 4 + gfx_text_width(FONT_SMALL, h->label);
}

void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * HINT_GAP;
    for (int i = 0; i < n; i++)
        total += hint_width(&hints[i]);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) {
            gfx_icon(h->icon, x, y + 3, HINT_ICON, HINT_ICON, btn_tint(h->tint));
            x += HINT_ICON + 4;
        } else {
            x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        }
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + HINT_GAP;
    }
}

void ui_dim(int t)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, (u32)((150 * t) >> 8) << 24); /* black, up to ~60% */
}

static void draw_footer(const Hint *hints, int n, int ox)
{
    if (cc_is_open())
        return; /* the control center has its own hints; two rows of them confuse */
    gfx_rect(0, FOOTER_Y, SCREEN_W, 1, C_LINE);
    ui_draw_hints(hints, n, SCREEN_W / 2 + ox, FOOTER_Y + 5);
}

/* The recording light, top right under the status bar: it is in the recording too. */
static void draw_rec(void)
{
    if (!rec_active())
        return;
    char t[16];
    int s = rec_seconds();
    snprintf(t, sizeof(t), "%d:%02d", s / 60, s % 60);
    int lh = gfx_line_h(FONT_SMALL), h = 18, w = gfx_text_width(FONT_SMALL, t) + 28;
    int x = SCREEN_W - 8 - w, y = 26;
    gfx_rrect(x, y, w, h, h / 2, C_RAISED);
    gfx_rrect(x + 8, y + 5, 8, 8, 4, C_LOW);
    gfx_text(FONT_SMALL, x + 21, y + (h - lh) / 2, t, C_TEXT);
}

static void draw_toast(void)
{
    if (!S.toast)
        return;
    int w = gfx_text_width(FONT_SMALL, S.toast) + 24, h = gfx_line_h(FONT_SMALL) + 10;
    if (w > SCREEN_W - 2 * MARGIN) w = SCREEN_W - 2 * MARGIN;
    int x = (SCREEN_W - w) / 2, y = FOOTER_Y - h - 6;
    gfx_rrect(x, y, w, h, h / 2, C_PILL);
    gfx_text_fit(FONT_SMALL, x + 12, y + 5, w - 24, S.toast, C_TEXT);
}

/* ICON0 if it is loaded, otherwise a tinted tile with the title on it. */
/* Square cover art for a song (the left 80x80 of its thumbnail), or a note. */
void ui_draw_cover(const Entry *e, int x, int y, int s, int r, u32 bg)
{
    const void *tex = thumbs_get(e);
    if (tex) {
        gfx_texture(tex, GU_PSM_5551, 256, 128, 144, 1, 79, 79, x, y, s, s);
        gfx_corners(x, y, s, s, r, bg);
        return;
    }
    gfx_rrect(x, y, s, s, r, bg == C_RAISED ? C_SURFACE : C_RAISED);
    int is = s * 45 / 100;
    gfx_icon(ICON_MUSIC, x + (s - is) / 2, y + (s - is) / 2, is, is, C_MUTED);
}

static void draw_cover(const Entry *e, int x, int y, int s, int r) { ui_draw_cover(e, x, y, s, r, C_BG); }

static void draw_song_tile(const Entry *e, int x, int y, int w, int h)
{
    gfx_rrect(x, y, w, h, TILE_R, C_SURFACE);
    draw_cover(e, x, y, h, TILE_R);
    int tx = x + h + 6, tw = w - h - 10, lh = gfx_line_h(FONT_SMALL);
    gfx_text_fit(FONT_SMALL, tx, y + 8, tw, e->title, C_TEXT);
    gfx_text_fit(FONT_SMALL, tx, y + 8 + lh, tw, e->artist[0] ? e->artist : "Unknown artist", C_MUTED);
    MusicNow now;
    if (music_state() != MUSIC_STOPPED && music_now(&now) == 0 && !strcmp(now.path, e->path))
        gfx_icon(ICON_MUSIC, x + w - 18, y + h - 18, 12, 12, C_FOCUS);
}

static const char *video_format(const Entry *e)
{
    const char *dot = strrchr(e->path, '.');
    return dot && !strcasecmp(dot, ".pmf") ? "PMF" : "MP4";
}

/* Videos have no thumbnail yet: a film icon, the title and the length. */
/* The title across the bottom of a video picture. */
static void draw_video_caption(const Entry *e, int x, int y, int w, int h)
{
    int lh = gfx_line_h(FONT_SMALL);
    gfx_rect(x, y + h - lh - 4, w, lh + 4, 0xA0000000u);
    gfx_text_fit(FONT_SMALL, x + 6, y + h - lh - 2, w - 12, e->title, C_WHITE);
}

/* A video's still frame (made once, 10% in, and cached), or a film icon until it exists. */
/* How far in a video was stopped last time: a thin bar along the bottom of its picture. */
static void draw_watched(const Entry *e, int x, int y, int w, int h)
{
    int dur, ms = video_place(e->path, &dur);
    if (!ms || dur <= 0)
        return;
    gfx_rect(x, y + h - 3, w, 3, 0xC0000000u);
    gfx_rect(x, y + h - 3, (int)((long long)w * ms / dur), 3, C_FOCUS);
}

static void draw_video_tile(const Entry *e, int x, int y, int w, int h, int big)
{
    const void *tex = thumbs_get(e);
    if (tex) {
        gfx_rect(x, y, w, h, RGB(0x000000));
        gfx_thumb(tex, x, y, w, h);
        if (!big)
            draw_video_caption(e, x, y, w, h);
        draw_watched(e, x, y, w, h);
        gfx_corners(x, y, w, h, TILE_R, C_BG);
        return;
    }
    if (thumbs_missing(e))
        preview_want_thumb(e);
    gfx_rrect(x, y, w, h, TILE_R, C_SURFACE);
    int is = big ? 48 : 24;
    gfx_icon(ICON_VIDEO, x + (w - is) / 2, y + (h - is) / 2 - (big ? 0 : 8), is, is, C_MUTED);
    if (!big) {
        gfx_text_fit(FONT_SMALL, x + 6, y + h - 2 * gfx_line_h(FONT_SMALL) - 2, w - 12, e->title, C_TEXT);
        gfx_text_fit(FONT_SMALL, x + 6, y + h - gfx_line_h(FONT_SMALL) - 2, w - 12,
                     e->sysver[0] ? e->sysver : video_format(e), C_MUTED);
    }
}

static void draw_art(const Entry *e, int x, int y, int w, int h, int r, int label)
{
    const void *tex = thumbs_get(e);
    if (tex) {
        gfx_rect(x, y, w, h, C_SURFACE); /* behind transparent ICON0 edges */
        gfx_thumb(tex, x, y, w, h);
        gfx_corners(x, y, w, h, r, C_BG);
        return;
    }
    gfx_rrect(x, y, w, h, r, RGB(s_tints[e->key % COUNT(s_tints)]));
    if (label)
        gfx_text_fit(FONT_SMALL, x + 6, y + h - gfx_line_h(FONT_SMALL) - 3, w - 12, e->title, C_WHITE);
}

static void draw_cards(int ox)
{
    /* A card on its way in or out (the UMD) brings its gap with it. */
    int x = spring_int(&S.row_x) + ox;
    for (int i = 0; i < CAT_COUNT; i++) {
        int s = spring_int(&S.card[i]);
        if (s < 0) s = 0;
        int gap = s >= CARD_SMALL ? CARD_GAP : CARD_GAP * s / CARD_SMALL;
        int a = S.card_a[i].x;
        if (a > 256) a = 256;
        if (s < 2 || a <= 0 || x + s < 0 || x > SCREEN_W) {
            x += s + gap;
            continue;
        }
        int t = (S.card[i].x - FX(CARD_SMALL)) / (CARD_BIG - CARD_SMALL); /* Q8 0..256 */
        int y = CARD_CY - s / 2;
        u32 bg = color_lerp(C_SURFACE, C_ACCENT, t), fg = color_lerp(C_MUTED, C_WHITE, t);
        gfx_rrect(x, y, s, s, s * 22 / 100, WITH_ALPHA(bg, a * 255 / 256));
        int is = s * 56 / 100;
        if (i == CAT_WEB)
            web_draw_icon(WEB_ICON_GLOBE, x + (s - is) / 2, y + (s - is) / 2, is, is, WITH_ALPHA(fg, a * 255 / 256));
        else
            gfx_icon(s_cats[i].icon, x + (s - is) / 2, y + (s - is) / 2, is, is, WITH_ALPHA(fg, a * 255 / 256));
        x += s + gap;
    }
    gfx_text_center(FONT_BOLD, SCREEN_W / 2 + ox, CARD_LABEL_Y, s_cats[S.cat].name, C_TEXT);
}

static void draw_tile_row(int v, int ox)
{
    if (library_count(v) == 0) {
        const char *msg = S.scanning ? "Scanning the Memory Stick..." : s_cats[S.cat].empty;
        gfx_text_center(FONT_BODY, SCREEN_W / 2 + ox, 168, msg, C_MUTED);
        return;
    }
    gfx_text(FONT_SMALL, MARGIN + ox, RECENT_LABEL_Y, "Recent", C_MUTED);
    int n = library_recent_count(v);
    for (int i = 0; i <= n; i++) {
        int x, w;
        tile_rect(v, i, &x, &w);
        x += ox;
        if (i < n && v == VIEW_VIDEO) {
            const Entry *e = library_recent(v, i);
            if (i == S.tile[v] && preview_draw(e, x, TILE_Y, w, TILE_H)) { /* plays only where the ring is */
                draw_video_caption(e, x, TILE_Y, w, TILE_H);
                draw_watched(e, x, TILE_Y, w, TILE_H);
                gfx_corners(x, TILE_Y, w, TILE_H, TILE_R, C_BG);
            } else {
                draw_video_tile(e, x, TILE_Y, w, TILE_H, 0);
            }
        } else if (i < n && v == VIEW_MUSIC) {
            draw_song_tile(library_recent(v, i), x, TILE_Y, w, TILE_H);
        } else if (i < n) {
            const Entry *e = library_recent(v, i);
            if (i == S.tile[v] && preview_draw(e, x, TILE_Y, w, TILE_H)) /* ICON1 in place of ICON0 */
                gfx_corners(x, TILE_Y, w, TILE_H, TILE_R, C_BG);
            else
                draw_art(e, x, TILE_Y, w, TILE_H, TILE_R, 1);
        } else {
            char count[12];
            snprintf(count, sizeof(count), "%d", library_count(v));
            gfx_rrect(x, TILE_Y, w, TILE_H, TILE_R, C_SURFACE);
            gfx_icon(ICON_GRID, x + (w - 20) / 2, TILE_Y + 12, 20, 20, C_MUTED);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 36, "All", C_TEXT);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 51, count, C_MUTED);
        }
    }
    gfx_ring(spring_int(&S.ring_x) - 3 + ox, TILE_Y - 3, spring_int(&S.ring_w) + 6, TILE_H + 6, C_FOCUS);
}

/* Settings: the three sections used last, then All. */
static void draw_settings_row(int ox)
{
    gfx_text(FONT_SMALL, MARGIN + ox, RECENT_LABEL_Y, "Recent", C_MUTED);
    for (int i = 0; i <= SETTINGS_RECENT; i++) {
        int x, w;
        tile_rect(ROW_SETTINGS, i, &x, &w);
        x += ox;
        gfx_rrect(x, TILE_Y, w, TILE_H, TILE_R, C_SURFACE);
        if (i < SETTINGS_RECENT) {
            int s = settings_recent(i);
            gfx_icon(settings_section_icon(s), x + 10, TILE_Y + 10, 20, 20, C_FOCUS);
            gfx_text_fit(FONT_BOLD, x + 10, TILE_Y + 34, w - 20, settings_section_name(s), C_TEXT);
            gfx_text_fit(FONT_SMALL, x + 10, TILE_Y + 50, w - 20, settings_section_sub(s), C_MUTED);
        } else {
            char count[16];
            snprintf(count, sizeof(count), "%d", SETTINGS_SECTIONS); /* like the library rows' count */
            gfx_icon(ICON_GRID, x + (w - 20) / 2, TILE_Y + 12, 20, 20, C_MUTED);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 36, "All", C_TEXT);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 51, count, C_MUTED);
        }
    }
    gfx_ring(spring_int(&S.ring_x) - 3 + ox, TILE_Y - 3, spring_int(&S.ring_w) + 6, TILE_H + 6, C_FOCUS);
}

/* Files: storage and the folders opened last, then Browse. */
static void draw_files_row(int ox)
{
    gfx_text(FONT_SMALL, MARGIN + ox, RECENT_LABEL_Y, "Places", C_MUTED);
    int n = files_places();
    for (int i = 0; i <= n + 1; i++) {
        int x, w;
        tile_rect(ROW_FILES, i, &x, &w);
        x += ox;
        gfx_rrect(x, TILE_Y, w, TILE_H, TILE_R, C_SURFACE);
        if (i == n) { /* Save data */
            gfx_icon(ICON_GAMES, x + 10, TILE_Y + 10, 20, 20, C_FOCUS);
            gfx_text_fit(FONT_BOLD, x + 10, TILE_Y + 34, w - 20, "Save data", C_TEXT);
            gfx_text_fit(FONT_SMALL, x + 10, TILE_Y + 50, w - 20, "Back up, restore", C_MUTED);
        } else if (i < n) {
            gfx_icon(fm_place_icon(i), x + 10, TILE_Y + 10, 20, 20, fm_place_icon(i) == ICON_FOLDER ? RGB(0xd29922) : C_FOCUS);
            gfx_text_fit(FONT_BOLD, x + 10, TILE_Y + 34, w - 20, fm_place_name(i), C_TEXT);
            gfx_text_fit(FONT_SMALL, x + 10, TILE_Y + 50, w - 20, fm_place_path(i), C_MUTED);
        } else {
            gfx_icon(ICON_GRID, x + (w - 20) / 2, TILE_Y + 12, 20, 20, C_MUTED);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 36, "Browse", C_TEXT);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 51, fm_busy() ? "Working..." : "All files", C_MUTED);
        }
    }
    gfx_ring(spring_int(&S.ring_x) - 3 + ox, TILE_Y - 3, spring_int(&S.ring_w) + 6, TILE_H + 6, C_FOCUS);
}

/* Web: a few places, then the browser (the last page, or the address bar). */
static void draw_web_row(int ox)
{
    gfx_text(FONT_SMALL, MARGIN + ox, RECENT_LABEL_Y, "Go to", C_MUTED);
    int n = web_tile_count();
    for (int i = 0; i <= n; i++) {
        int x, w;
        tile_rect(ROW_WEB, i, &x, &w);
        x += ox;
        gfx_rrect(x, TILE_Y, w, TILE_H, TILE_R, C_SURFACE);
        if (i < n) {
            if (i == 0) gfx_icon(ICON_SEARCH, x + 12, TILE_Y + 12, 16, 16, C_FOCUS);
            else web_draw_icon(WEB_ICON_GLOBE_S, x + 10, TILE_Y + 10, 20, 20, C_FOCUS);
            gfx_text_fit(FONT_BOLD, x + 10, TILE_Y + 34, w - 20, web_tile_name(i), C_TEXT);
            gfx_text_fit(FONT_SMALL, x + 10, TILE_Y + 50, w - 20, web_tile_sub(i), C_MUTED);
        } else {
            const char *last = web_last_title();
            web_draw_icon(WEB_ICON_GLOBE_S, x + (w - 20) / 2, TILE_Y + 12, 20, 20, C_MUTED);
            gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 36, last[0] ? "Continue" : "Browser", C_TEXT);
            if (last[0]) gfx_text_fit(FONT_SMALL, x + 6, TILE_Y + 51, w - 12, last, C_MUTED);
            else gfx_text_center(FONT_SMALL, x + w / 2, TILE_Y + 51, "Type an address", C_MUTED);
        }
    }
    gfx_ring(spring_int(&S.ring_x) - 3 + ox, TILE_Y - 3, spring_int(&S.ring_w) + 6, TILE_H + 6, C_FOCUS);
}

/* The disc: its ICON0 (or ICON1 playing), and what it is beside it. */
static void draw_umd_row(int ox)
{
    int st = umd_state(), x = MARGIN + ox;
    const Entry *e = umd_entry();
    gfx_text(FONT_SMALL, x, RECENT_LABEL_Y, "In the drive", C_MUTED);
    if (st == UMD_GAME) {
        if (preview_draw(e, x, TILE_Y, TILE_W, TILE_H))
            gfx_corners(x, TILE_Y, TILE_W, TILE_H, TILE_R, C_BG);
        else
            draw_art(e, x, TILE_Y, TILE_W, TILE_H, TILE_R, 1);
    } else {
        gfx_rrect(x, TILE_Y, TILE_W, TILE_H, TILE_R, C_SURFACE);
        gfx_icon(st == UMD_VIDEO ? ICON_VIDEO : ICON_UMD, x + (TILE_W - 32) / 2, TILE_Y + (TILE_H - 32) / 2, 32, 32,
                 C_MUTED);
    }
    gfx_ring(x - 3, TILE_Y - 3, TILE_W + 6, TILE_H + 6, C_FOCUS);

    int tx = x + TILE_W + 14, tw = SCREEN_W - MARGIN + ox - tx, y = TILE_Y + 4;
    char line[64];
    const char *title = st == UMD_GAME || st == UMD_VIDEO ? e->title
                      : st == UMD_READING ? "Reading the disc..." : "This disc can't be read";
    gfx_text_fit(FONT_BOLD, tx, y, tw, title, C_TEXT);
    y += gfx_line_h(FONT_BOLD) + 2;
    if (st == UMD_GAME) {
        const char *reg = region_name(e->id);
        int o = snprintf(line, sizeof(line), "UMD");
        if (e->id[0]) o += snprintf(line + o, sizeof(line) - o, DOT "%s", e->id);
        if (reg) o += snprintf(line + o, sizeof(line) - o, DOT "%s", reg);
        if (e->sysver[0]) snprintf(line + o, sizeof(line) - o, DOT "FW %s", e->sysver);
    } else if (st == UMD_VIDEO) {
        snprintf(line, sizeof(line), "UMD Video" DOT "play it from the XMB");
    } else if (st == UMD_READING) {
        snprintf(line, sizeof(line), "The disc is spinning up");
    } else {
        snprintf(line, sizeof(line), "Not a PSP game, or the disc is dirty");
    }
    gfx_text_fit(FONT_SMALL, tx, y, tw, line, C_MUTED);
}

static void draw_home(const SysStatus *st, int ox)
{
    draw_status_bar("Home", 0, st, ox);
    draw_cards(ox);
    if (cat_view() >= 0) {
        draw_tile_row(cat_view(), ox);
    } else if (S.cat == CAT_SETTINGS) {
        draw_settings_row(ox);
    } else if (S.cat == CAT_FILES) {
        draw_files_row(ox);
    } else if (S.cat == CAT_WEB) {
        draw_web_row(ox);
    } else if (S.cat == CAT_UMD) {
        draw_umd_row(ox);
    } else {
        gfx_text_center(FONT_BODY, SCREEN_W / 2 + ox, 168, s_cats[S.cat].empty, C_MUTED);
    }
    draw_footer(s_home_hints, COUNT(s_home_hints), ox);
}

/* One grid tile: art (or a live preview where the ring is), a star for favorites. */
static void draw_grid_tile(const Entry *e, int x, int y, int w, int h, int focused, const char *playing)
{
    int v = S.view;
    if (v == VIEW_MUSIC) {
        draw_cover(e, x, y, w, TILE_R);
        if (playing && !strcmp(playing, e->path)) {
            gfx_rrect(x + w - 20, y + h - 20, 16, 16, 8, 0xB0000000u);
            gfx_icon(ICON_MUSIC, x + w - 18, y + h - 18, 12, 12, C_FOCUS);
        }
    } else if (kind_is_video(e->kind)) {
        if (focused && preview_draw(e, x, y, w, h)) {
            draw_video_caption(e, x, y, w, h);
            draw_watched(e, x, y, w, h);
            gfx_corners(x, y, w, h, TILE_R, C_BG);
        } else {
            draw_video_tile(e, x, y, w, h, 0);
        }
    } else if (focused && previewable(v) && preview_draw(e, x, y, w, h)) { /* ICON1 in place of ICON0 */
        gfx_corners(x, y, w, h, TILE_R, C_BG);
    } else {
        draw_art(e, x, y, w, h, TILE_R, 1);
    }
    if (favorite_get(e->path)) {
        gfx_rrect(x + w - 20, y + 4, 16, 16, 8, 0xB0000000u);
        gfx_icon(ICON_STAR, x + w - 18, y + 6, 12, 12, C_FOCUS);
    }
}

/* What the info line under the grid says after the title. */
static void grid_detail(const Entry *e, char *line, int n)
{
    char size[16];
    format_size(size, sizeof(size), e->size);
    if (kind_is_music(e->kind)) {
        snprintf(line, n, "%s", e->artist[0] ? e->artist : "Unknown artist");
    } else if (kind_is_video(e->kind)) {
        int o = snprintf(line, n, "%s" DOT "%s", video_format(e), size);
        if (e->sysver[0]) snprintf(line + o, n - o, DOT "%s", e->sysver);
    } else if (kind_is_picture(e->kind)) {
        snprintf(line, n, "%s" DOT "%s", img_format_name(e->path), size);
    } else {
        const char *reg = region_name(e->id);
        int o = snprintf(line, n, "%s", e->id);
        if (reg) o += snprintf(line + o, n - o, "%s%s", o ? DOT : "", reg);
        snprintf(line + o, n - o, "%s%s", o ? DOT : "", size);
    }
}

/* A folder in the grid: its icon, name and how many entries it holds. */
static void draw_folder_tile(int f, int x, int y, int w, int h)
{
    int v = S.view;
    gfx_rrect(x, y, w, h, TILE_R, C_SURFACE);
    gfx_icon(ICON_FOLDER, x + (w - 26) / 2, y + 6, 26, 26, RGB(0xd29922));
    gfx_text_fit(FONT_SMALL, x + 6, y + h - gfx_line_h(FONT_SMALL) - 3, w - 12, groups_folder_name(v, f), C_TEXT);
    char c[8];
    snprintf(c, sizeof(c), "%d", S.fcount[v][f]);
    gfx_text_right(FONT_SMALL, x + w - 6, y + 4, c, C_MUTED);
}

#define C_PLAYLIST btn_tint(C_BTN_TRIANGLE)

static void songs_line(char *line, int n, int k)
{
    int c = pl_songs(k);
    snprintf(line, n, "Playlist" DOT "%d %s", c, c == 1 ? "song" : "songs");
}

/* A playlist in the grid: a note over a stack of lines, its name and song count. */
static void draw_playlist_tile(int k, int x, int y, int w, int h)
{
    gfx_rrect(x, y, w, h, TILE_R, C_SURFACE);
    gfx_icon(ICON_MUSIC, x + (w - 22) / 2, y + 6, 22, 22, C_PLAYLIST);
    gfx_text_fit(FONT_SMALL, x + 6, y + h - gfx_line_h(FONT_SMALL) - 3, w - 12, pl_name(k), C_TEXT);
    char c[8];
    snprintf(c, sizeof(c), "%d", pl_songs(k));
    gfx_text_right(FONT_SMALL, x + w - 6, y + 4, c, C_MUTED);
}

/* Artists and Albums: the line under a name ("12 songs \xC2\xB7 2 albums", "Queen \xC2\xB7 9 songs"). */
static void mb_line(char *line, int n, int kind, const MbGroup *g)
{
    const char *s = g->count == 1 ? "song" : "songs";
    if (kind == MB_ARTISTS)
        snprintf(line, n, "%d %s" DOT "%d %s", g->count, s, g->albums, g->albums == 1 ? "album" : "albums");
    else if (g->sub[0])
        snprintf(line, n, "%s" DOT "%d %s", g->sub, g->count, s);
    else
        snprintf(line, n, "%d %s", g->count, s);
}

/* The two rows that open Artists and Albums, as tiles: the icon and the name. */
static void draw_mtop_tile(int kind, int x, int y, int w, int h)
{
    gfx_rrect(x, y, w, h, TILE_R, C_SURFACE);
    gfx_icon(kind == MB_ARTISTS ? ICON_ARTIST : ICON_ALBUM, x + (w - 22) / 2, y + 6, 22, 22, C_PLAYLIST);
    gfx_text_fit(FONT_SMALL, x + 6, y + h - gfx_line_h(FONT_SMALL) - 3, w - 12, s_mb_title[kind], C_TEXT);
    char c[8];
    snprintf(c, sizeof(c), "%d", mb_count(kind));
    gfx_text_right(FONT_SMALL, x + w - 6, y + 4, c, C_MUTED);
}

/* An artist or album as a tile: the cover of one of its songs. */
static void draw_mgroup_tile(int k, int x, int y, int w)
{
    const MbGroup *g = mb_group(S.mb_kind, k);
    if (g)
        draw_cover(music_get(g->cover), x, y, w, TILE_R);
}

static void draw_grid(int ox)
{
    int v = S.view, n = disp_count(v), sel = S.sel[v];
    GridGeom g = grid_geom(v);
    int pitch = g.h + GRID_VGAP;
    const int bottom = GRID_Y + GRID_ROWS * pitch - GRID_VGAP;
    MusicNow now;
    const char *playing = music_state() != MUSIC_STOPPED && music_now(&now) == 0 ? now.path : NULL;

    int first = (S.scroll.x >> 8) - 1;
    if (first < 0) first = 0;
    for (int r = first; r <= first + GRID_ROWS + 1; r++) {
        int y = GRID_Y + (((FX(r) - S.scroll.x) * pitch) >> 8);
        if (y >= bottom + 4 || y + g.h <= GRID_Y - 4)
            continue;
        for (int c = 0; c < g.cols; c++) {
            int i = r * g.cols + c;
            if (i >= n)
                break;
            int tx = g.x0 + c * (g.w + g.gap) + ox;
            if (disp_mtop(v, i) >= 0)
                draw_mtop_tile(disp_mtop(v, i), tx, y, g.w, g.h);
            else if (disp_mgroup(v, i) >= 0)
                draw_mgroup_tile(disp_mgroup(v, i), tx, y, g.w);
            else if (disp_playlist(v, i) >= 0)
                draw_playlist_tile(disp_playlist(v, i), tx, y, g.w, g.h);
            else if (disp_folder(v, i) >= 0)
                draw_folder_tile(disp_folder(v, i), tx, y, g.w, g.h);
            else
                draw_grid_tile(disp_entry(v, i), tx, y, g.w, g.h, i == sel, playing);
        }
    }
    /* Mask tiles that scroll past the grid area. */
    gfx_rect(0, 0, SCREEN_W, GRID_Y - 4, C_BG);
    gfx_rect(0, bottom + 4, SCREEN_W, SCREEN_H - bottom - 4, C_BG); /* to the bottom: the footer is drawn on top */

    int rx = g.x0 + ((S.gcol.x * (g.w + g.gap)) >> 8) + ox;
    int ry = GRID_Y + (((S.hl.x - S.scroll.x) * pitch) >> 8);
    gfx_ring(rx - 3, ry - 3, g.w + 6, g.h + 6, C_FOCUS);

    int rows = (n + g.cols - 1) / g.cols;
    if (rows > GRID_ROWS) { /* where in the list */
        int h = bottom - GRID_Y, bh = h * GRID_ROWS / rows;
        if (bh < 10) bh = 10;
        int y = GRID_Y + (h - bh) * S.top[v] / (rows - GRID_ROWS);
        gfx_rrect(SCREEN_W - 6 + ox, y, 3, bh, 1, C_RAISED);
    }

    const Entry *e = disp_entry(v, sel);
    char line[64];
    const char *title;
    if (e) {
        grid_detail(e, line, sizeof(line));
        title = e->title;
    } else if (disp_mtop(v, sel) >= 0) {
        int k = disp_mtop(v, sel), c = mb_count(k);
        snprintf(line, sizeof(line), "%d %s", c, k == MB_ARTISTS ? (c == 1 ? "artist" : "artists") : c == 1 ? "album" : "albums");
        title = s_mb_title[k];
    } else if (disp_mgroup(v, sel) >= 0) {
        const MbGroup *mg = mb_group(S.mb_kind, sel);
        mb_line(line, sizeof(line), S.mb_kind, mg);
        title = mb_label(mg);
    } else if (disp_playlist(v, sel) >= 0) {
        songs_line(line, sizeof(line), disp_playlist(v, sel));
        title = pl_name(disp_playlist(v, sel));
    } else { /* a folder */
        int f = disp_folder(v, sel), k = f >= 0 ? S.fcount[v][f] : 0;
        snprintf(line, sizeof(line), "Folder" DOT "%d %s", k, v == VIEW_HOMEBREW ? (k == 1 ? "app" : "apps") : k == 1 ? "game" : "games");
        title = groups_folder_name(v, f);
    }
    int dw = gfx_text_width(FONT_SMALL, line);
    if (dw > 200) dw = 200;
    int tw = SCREEN_W - 2 * MARGIN - dw - 12;
    gfx_text_fit(FONT_BOLD, MARGIN + ox, GRID_INFO_Y, tw, title, C_TEXT);
    gfx_text_fit(FONT_SMALL, SCREEN_W - MARGIN - dw + ox, GRID_INFO_Y + 3, dw, line, C_MUTED);
}

static void draw_list(const SysStatus *st, int ox)
{
    int v = S.view, n = disp_count(v), sel = S.sel[v];
    const int list_bottom = LIST_Y + LIST_ROWS * ROW_H;

    if (n > 0 && is_grid(v)) {
        draw_grid(ox);
    } else if (n > 0) {
        /* Highlight first, rows on top; both follow their springs. */
        int hl_y = LIST_Y + (((S.hl.x - S.scroll.x) * ROW_H) >> 8);
        gfx_rrect(LIST_X + ox, hl_y, LIST_W, ROW_H, 5, C_ACCENT);
        if (S.pl_moving && v == VIEW_MUSIC) /* picked up: a ring round it */
            gfx_ring(LIST_X + ox - 2, hl_y - 2, LIST_W + 4, ROW_H + 4, C_FOCUS);
        MusicNow now;
        const char *playing_path = music_state() != MUSIC_STOPPED && music_now(&now) == 0 ? now.path : NULL;
        int first = (S.scroll.x >> 8) - 1;
        for (int i = first < 0 ? 0 : first; i < n && i <= first + LIST_ROWS + 2; i++) {
            int y = LIST_Y + (((FX(i) - S.scroll.x) * ROW_H) >> 8);
            int f = i == sel ? FONT_BOLD : FONT_BODY;
            const Entry *re = disp_entry(v, i);
            int mt = disp_mtop(v, i), mg = disp_mgroup(v, i), pk = disp_playlist(v, i);
            if (mt >= 0 || mg >= 0) { /* Artists / Albums, or one of them: like a playlist */
                int tx = LIST_X + 6 + ox;
                char c[8];
                const MbGroup *g = mg >= 0 ? mb_group(S.mb_kind, mg) : NULL;
                snprintf(c, sizeof(c), "%d", g ? g->count : mb_count(mt));
                int kind = mt >= 0 ? mt : S.mb_kind;
                gfx_icon(kind == MB_ARTISTS ? ICON_ARTIST : ICON_ALBUM, tx, y + 4, 14, 14, i == sel ? C_WHITE : C_PLAYLIST);
                tx += 20;
                int cw = gfx_text_width(FONT_SMALL, c) + 10;
                gfx_text_fit(f, tx, y + (ROW_H - gfx_line_h(f)) / 2, LIST_X + LIST_W - 6 + ox - tx - cw,
                             g ? mb_label(g) : s_mb_title[mt], i == sel ? C_WHITE : C_TEXT);
                gfx_text_right(FONT_SMALL, LIST_X + LIST_W - 8 + ox, y + (ROW_H - gfx_line_h(FONT_SMALL)) / 2 + 1, c,
                               i == sel ? C_WHITE : C_MUTED);
                continue;
            }
            if (pk >= 0) { /* a playlist: like a folder, in its own colour */
                int tx = LIST_X + 6 + ox;
                char c[8];
                snprintf(c, sizeof(c), "%d", pl_songs(pk));
                gfx_icon(ICON_MUSIC, tx, y + 4, 14, 14, i == sel ? C_WHITE : C_PLAYLIST);
                tx += 20;
                int cw = gfx_text_width(FONT_SMALL, c) + 10;
                gfx_text_fit(f, tx, y + (ROW_H - gfx_line_h(f)) / 2, LIST_X + LIST_W - 6 + ox - tx - cw, pl_name(pk),
                             i == sel ? C_WHITE : C_TEXT);
                gfx_text_right(FONT_SMALL, LIST_X + LIST_W - 8 + ox, y + (ROW_H - gfx_line_h(FONT_SMALL)) / 2 + 1, c,
                               i == sel ? C_WHITE : C_MUTED);
                continue;
            }
            if (!re) { /* a folder: its icon, name and count */
                int fo = disp_folder(v, i), tx = LIST_X + 6 + ox;
                char c[8];
                snprintf(c, sizeof(c), "%d", S.fcount[v][fo]);
                gfx_icon(ICON_FOLDER, tx, y + 4, 14, 14, i == sel ? C_WHITE : RGB(0xd29922));
                tx += 20;
                int cw = gfx_text_width(FONT_SMALL, c) + 10;
                gfx_text_fit(f, tx, y + (ROW_H - gfx_line_h(f)) / 2, LIST_X + LIST_W - 6 + ox - tx - cw,
                             groups_folder_name(v, fo), i == sel ? C_WHITE : C_TEXT);
                gfx_text_right(FONT_SMALL, LIST_X + LIST_W - 8 + ox, y + (ROW_H - gfx_line_h(FONT_SMALL)) / 2 + 1, c,
                               i == sel ? C_WHITE : C_MUTED);
                continue;
            }
            int fav = favorite_get(re->path);
            int tx = LIST_X + 6 + ox;
            if (v == VIEW_MUSIC && playing_path && !strcmp(playing_path, re->path)) {
                gfx_icon(ICON_MUSIC, tx, y + 5, 12, 12, i == sel ? C_WHITE : C_FOCUS);
                tx += 16;
            }
            gfx_text_fit(f, tx, y + (ROW_H - gfx_line_h(f)) / 2, LIST_X + LIST_W - 6 + ox - tx - (fav ? 16 : 0),
                         re->title, i == sel ? C_WHITE : C_MUTED);
            if (fav)
                gfx_icon(ICON_STAR, LIST_X + LIST_W - 18 + ox, y + 5, 12, 12, i == sel ? C_WHITE : C_FOCUS);
        }
        /* Mask rows that scroll past the list area. */
        gfx_rect(0, 0, PREVIEW_X - 4 + ox, LIST_Y - 2, C_BG);
        gfx_rect(0, list_bottom + 2, PREVIEW_X - 4 + ox, SCREEN_H - list_bottom - 2, C_BG); /* to the bottom: the footer is drawn on top */

        const Entry *e = disp_entry(v, sel);
        char line[64], size[16];
        int x = PREVIEW_X + ox, y = LIST_Y + 2;
        if (!e && disp_mtop(v, sel) >= 0) { /* the row that opens Artists or Albums */
            int k = disp_mtop(v, sel), c = mb_count(k);
            gfx_rrect(x + (PREVIEW_W - PREVIEW_H) / 2, y, PREVIEW_H, PREVIEW_H, TILE_R, C_SURFACE);
            gfx_icon(k == MB_ARTISTS ? ICON_ARTIST : ICON_ALBUM, x + (PREVIEW_W - 48) / 2, y + (PREVIEW_H - 48) / 2, 48,
                     48, C_PLAYLIST);
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, s_mb_title[k], C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            snprintf(line, sizeof(line), "%d %s", c,
                     k == MB_ARTISTS ? (c == 1 ? "artist" : "artists") : c == 1 ? "album" : "albums");
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            y += gfx_line_h(FONT_SMALL);
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, "From the songs' tags", C_MUTED);
            goto preview_done;
        }
        if (!e && disp_mgroup(v, sel) >= 0) { /* an artist or album: a song's cover, its name and counts */
            const MbGroup *g = mb_group(S.mb_kind, sel);
            draw_cover(music_get(g->cover), x + (PREVIEW_W - PREVIEW_H) / 2, y, PREVIEW_H, TILE_R);
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, mb_label(g), C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            if (S.mb_kind == MB_ALBUMS && g->sub[0]) {
                gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, g->sub, C_MUTED);
                y += gfx_line_h(FONT_SMALL);
                snprintf(line, sizeof(line), "%d %s", g->count, g->count == 1 ? "song" : "songs");
            } else {
                mb_line(line, sizeof(line), S.mb_kind, g);
            }
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            goto preview_done;
        }
        if (!e && disp_playlist(v, sel) >= 0) { /* a playlist */
            int pk = disp_playlist(v, sel);
            gfx_rrect(x + (PREVIEW_W - PREVIEW_H) / 2, y, PREVIEW_H, PREVIEW_H, TILE_R, C_SURFACE);
            gfx_icon(ICON_MUSIC, x + (PREVIEW_W - 48) / 2, y + (PREVIEW_H - 48) / 2, 48, 48, C_PLAYLIST);
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, pl_name(pk), C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            songs_line(line, sizeof(line), pk);
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            y += gfx_line_h(FONT_SMALL);
            const char *file = strchr(pl_path(pk), '/');
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W,
                         pl_songs(pk) ? (file ? file + 1 : pl_path(pk)) : "Add songs: a song's Options", C_MUTED);
            goto preview_done;
        }
        if (!e) { /* a folder */
            int fo = disp_folder(v, sel), k = S.fcount[v][fo];
            gfx_rrect(x, y, PREVIEW_W, PREVIEW_H, TILE_R, C_SURFACE);
            gfx_icon(ICON_FOLDER, x + (PREVIEW_W - 48) / 2, y + (PREVIEW_H - 48) / 2, 48, 48, RGB(0xd29922));
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, groups_folder_name(v, fo), C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            snprintf(line, sizeof(line), "Folder" DOT "%d %s", k, v == VIEW_HOMEBREW ? (k == 1 ? "app" : "apps") : k == 1 ? "game" : "games");
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            y += gfx_line_h(FONT_SMALL);
            if (!k)
                gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, "Add games: Options, Move to...", C_MUTED);
            goto preview_done;
        }
        if (kind_is_video(e->kind)) {
            if (preview_draw(e, x, y, PREVIEW_W, PREVIEW_H)) {
                draw_watched(e, x, y, PREVIEW_W, PREVIEW_H);
                gfx_corners(x, y, PREVIEW_W, PREVIEW_H, TILE_R, C_BG);
            } else {
                draw_video_tile(e, x, y, PREVIEW_W, PREVIEW_H, 1);
            }
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, e->title, C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            format_size(size, sizeof(size), e->size);
            int o = snprintf(line, sizeof(line), "%s" DOT "%s", video_format(e), size);
            if (e->id[0]) o += snprintf(line + o, sizeof(line) - o, DOT "%s", e->id);
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            y += gfx_line_h(FONT_SMALL);
            int dur, at = video_place(e->path, &dur);
            if (at) { /* where it was stopped */
                char t1[16];
                int sec = at / 1000;
                snprintf(t1, sizeof(t1), sec >= 3600 ? "%d:%02d:%02d" : "%d:%02d", sec >= 3600 ? sec / 3600 : sec / 60,
                         sec >= 3600 ? sec / 60 % 60 : sec % 60, sec % 60);
                snprintf(line, sizeof(line), e->sysver[0] ? "%s" DOT "stopped at %s" : "Stopped at %s%s",
                         e->sysver[0] ? e->sysver : t1, e->sysver[0] ? t1 : "");
                gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_FOCUS);
            } else if (e->sysver[0]) {
                gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, e->sysver, C_MUTED);
            }
            goto preview_done;
        }
        if (kind_is_music(e->kind)) {
            draw_cover(e, x + (PREVIEW_W - PREVIEW_H) / 2, y, PREVIEW_H, TILE_R);
            y += PREVIEW_H + 8;
            gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, e->title, C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, e->artist[0] ? e->artist : "Unknown artist", C_MUTED);
            y += gfx_line_h(FONT_SMALL);
            if (e->album[0] && e->track)
                snprintf(line, sizeof(line), "%s" DOT "track %d", e->album, e->track);
            else
                snprintf(line, sizeof(line), "%s", e->album[0] ? e->album : "");
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            goto preview_done;
        }
        if (preview_draw(e, x, y, PREVIEW_W, PREVIEW_H))
            gfx_corners(x, y, PREVIEW_W, PREVIEW_H, TILE_R, C_BG);
        else
            draw_art(e, x, y, PREVIEW_W, PREVIEW_H, TILE_R, 0);
        y += PREVIEW_H + 8;
        gfx_text_fit(FONT_BOLD, x, y, PREVIEW_W, e->title, C_TEXT);
        y += gfx_line_h(FONT_BOLD) + 2;
        format_size(size, sizeof(size), e->size);
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s",
                 kind_is_picture(e->kind) ? img_format_name(e->path) : kind_name(e->kind), size);
        gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
        y += gfx_line_h(FONT_SMALL);
        if (kind_is_picture(e->kind)) {
            format_date(line, sizeof(line), e->mtime);
            gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
            goto preview_done;
        }
        const char *reg = region_name(e->id);
        int o = snprintf(line, sizeof(line), "%s", e->id);
        if (reg)
            o += snprintf(line + o, sizeof(line) - o, "%s%s", o ? " \xC2\xB7 " : "", reg);
        if (e->sysver[0])
            snprintf(line + o, sizeof(line) - o, "%sFW %s", o ? " \xC2\xB7 " : "", e->sysver);
        gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
        y += gfx_line_h(FONT_SMALL);
        history_describe(e->path, line, sizeof(line));
        gfx_text_fit(FONT_SMALL, x, y, PREVIEW_W, line, C_MUTED);
    preview_done:;
    } else {
        gfx_text_center(FONT_BODY, SCREEN_W / 2 + ox, 130,
                        v == VIEW_HIDDEN ? "Nothing removed from the library"
                        : grouped(v) && S.folder[v][0] ? "This folder is empty"
                        : v == VIEW_MUSIC && in_pl() ? "This playlist is empty" : "Nothing here yet", C_MUTED);
    }

    char title[64];
    if (grouped(v) && S.folder[v][0])
        snprintf(title, sizeof(title), "%s / %s \xC2\xB7 %d", s_view_name[v], S.folder[v], n);
    else if (v == VIEW_MUSIC && in_pl())
        snprintf(title, sizeof(title), "%s / %.40s \xC2\xB7 %d", s_view_name[v], pl_name(pl_open_index()), n);
    else if (v == VIEW_MUSIC && S.mb_level == 1)
        snprintf(title, sizeof(title), "%s / %s \xC2\xB7 %d", s_view_name[v], s_mb_title[S.mb_kind], n);
    else if (v == VIEW_MUSIC && S.mb_level == 2)
        snprintf(title, sizeof(title), "%s / %.40s \xC2\xB7 %d", s_view_name[v],
                 S.mb_name[0] ? S.mb_name : S.mb_kind == MB_ALBUMS ? "Unknown album" : "Unknown artist", n);
    else
        snprintf(title, sizeof(title), "%s \xC2\xB7 %d", s_view_name[v], library_count(v));
    draw_status_bar(title, 1, st, ox);
    const Hint *h = s_list_hints;
    int hn = COUNT(s_list_hints);
    if (v == VIEW_HIDDEN) { h = s_hidden_hints; hn = COUNT(s_hidden_hints); }
    else if (v == VIEW_PICTURES || v == VIEW_VIDEO) { h = s_picture_hints; hn = COUNT(s_picture_hints); } /* View / Back / Options */
    else if (v == VIEW_MUSIC) { h = s_music_hints; hn = COUNT(s_music_hints); }
    Hint all[8];
    memcpy(all, h, sizeof(Hint) * hn);
    if (S.pl_moving && v == VIEW_MUSIC) { /* moving a song: only these do anything */
        static const Hint move_h[] = {
            {-1, "Up/Down", 0, "Move"},
            {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Done"},
        };
        draw_footer(move_h, COUNT(move_h), ox);
        return;
    }
    if (disp_folder(v, sel) >= 0 || disp_playlist(v, sel) >= 0 || disp_mtop(v, sel) >= 0 || disp_mgroup(v, sel) >= 0)
        all[0].label = "Open"; /* ✕ on a folder, a playlist, Artists / Albums or one of them */
    else if (v == VIEW_MUSIC && is_playing(disp_entry(v, sel)))
        all[0].label = "Now Playing";
    if (grid_able(v)) { /* L/R switches between the list and the grid */
        all[hn].icon = -1;
        all[hn].key = "L/R";
        all[hn].tint = 0;
        all[hn].label = is_grid(v) ? "List" : "Grid";
        hn++;
    }
    draw_footer(all, hn, ox);
}

void ui_draw(const SysStatus *st)
{
    if (saver_covers()) { /* the screensaver hides everything but the volume OSD */
        saver_draw(st);
        osd_draw();
        return;
    }
    int t = S.trans.x; /* Q8: 0 home .. 256 list, may overshoot slightly */
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    if (boot_covers()) { /* the boot screen: nothing under it shows yet */
        boot_draw();
        osd_draw();
        return;
    }
    if (cc_usb_covers()) { /* USB mode: only its screen (and the volume OSD) */
        cc_usb_draw(st);
        draw_toast();
        saver_draw(st);
        osd_draw();
        return;
    }
    if (search_covers() || viewer_covers() || video_covers() || settings_covers() || fm_covers() || reader_covers() ||
        web_covers() || np_covers() || sd_covers())
        goto overlays; /* opaque search screen: skip drawing what it hides */
    if (t < 256) {
        gfx_set_opacity(256 - t);
        draw_home(st, -(t * 16 >> 8));
    }
    if (t > 0) {
        gfx_set_opacity(t);
        draw_list(st, (256 - t) * 24 >> 8);
    }
    gfx_set_opacity(256);
overlays:
    settings_draw(st);
    search_draw(st);
    fm_draw(st);
    reader_draw(st);
    web_draw(st);
    viewer_draw();
    video_draw();
    np_draw(st);
    sd_draw(st);
    opts_draw();
    menu_draw();
    osk_draw();
    cc_draw(st);
    cc_usb_draw(st); /* fading in or out */
    draw_toast();
    boot_draw(); /* fading into the launcher */
    saver_draw(st);
    osd_draw();
    draw_rec();
}
