/* XLT - the home and list screens' shared state: ui.c draws and drives them,
 * ui_menus.c has their side menus (folders, Move to, playlists, sorting).
 * Only those two include this. */
#ifndef UI_STATE_H
#define UI_STATE_H

#include "anim.h"
#include "entry.h"
#include "groups.h"
#include "library.h"

/* CAT_UMD is only there while a disc is in the drive (cat_shown). */
enum { CAT_UMD, CAT_HOMEBREW, CAT_GAMES, CAT_MUSIC, CAT_VIDEO, CAT_PICTURES, CAT_FILES, CAT_WEB, CAT_SETTINGS, CAT_COUNT };

enum { SCR_HOME, SCR_LIST };

typedef struct {
    int screen;
    Spring trans; /* 0 = home, FX(1) = list */

    int cat;
    Spring card[CAT_COUNT];
    /* At most CARDS_SHOWN cards on screen (with the UMD there are 8): the row
     * slides to keep the focused one in, cards outside fade out. */
    Spring card_a[CAT_COUNT]; /* 0..FX(1): how visible */
    Spring row_x;             /* where the row starts, px Q8 */
    int card_first;           /* first card in the window, counted among the shown ones */
    int umd_seen;             /* umd_state() at the last poll */
    int tile[VIEW_COUNT + 4]; /* focused tile per row (+ Settings, Files, UMD, Web); == recent count means "All" */
    Spring ring_x, ring_w;

    int view;                /* view shown by the list screen */
    int sel[VIEW_COUNT], top[VIEW_COUNT];
    Spring hl, scroll;       /* both in rows, Q8 (grid: the focused row and the top row) */
    Spring gcol;             /* grid: the focused column, Q8 */

    /* Games / Homebrew: what the list shows. At the top level the folders come
     * first (-1 - folder index), then the entries in no folder (library index);
     * inside a folder, its entries. */
    char folder[GROUPS_TABS][GROUP_NAME]; /* open folder, "" = the top level */
    int *disp[GROUPS_TABS], disp_n[GROUPS_TABS];
    int fcount[GROUPS_TABS][GROUP_FOLDERS]; /* entries per folder */
    char menu_folder[GROUP_NAME];            /* the folder the △ menu is for */
    char move_path[200];                     /* the entry the Move to menu is for */
    int move_def, move_tab;                  /* its type's tab; the tab a new folder goes in */

    /* Music: the playlists come first in the list (rows 0..pl_count()-1),
     * then the songs; an open playlist (playlists.c) shows its own songs. */
    int pl_shown;                            /* a playlist was open when last checked */
    char pl_back[200];                       /* the open playlist's path, to land on when leaving it */
    int menu_pl;                             /* the playlist the △ menu is for */
    int pl_confirm;                          /* ✕ once on Delete playlist */
    Entry pl_song;                           /* the song Add to playlist is for */
    int pl_with_song;                        /* New playlist... puts pl_song in it */
    int pl_moving;                           /* the focused song is being moved (↑/↓, ✕ done) */

    /* Music: Artists and Albums (mbrowse.c). Level 0: the Music list, its first two rows
     * open the lists; 1: the artists or the albums; 2: one artist's or album's songs. */
    int mb_level, mb_kind, mb_group;
    char mb_name[48], mb_sub[48];            /* the open group, to find it again after a rescan */
    int *mb_idx, mb_n;                       /* level 2: the songs, as library indices */

    int scanning;
    int music_gen;       /* last music_generation() drawn */
    Entry launch;        /* requested launch, handed to main after one drawn frame */
    int launch_pending;
    int exit_requested;
    char toast_buf[96];
    const char *toast;
    int toast_left;
} UiState;

#define S g_ui_state
extern UiState S;

/* ui.c: what the list shows. Games / Homebrew rows are folders (disp_entry
 * NULL) or entries; Music rows are playlists (disp_playlist >= 0) or songs. */
int def_tab(const Entry *e);                  /* VIEW_GAMES or VIEW_HOMEBREW by kind */
void disp_build(int v);                       /* Games / Homebrew rows, after groups changed */
const Entry *disp_entry(int v, int i);
int disp_playlist(int v, int i);
int disp_find(int v, const char *path);
int disp_find_folder(int v, const char *name);
int in_pl(void);                              /* Music shows an open playlist */
void select_entry(int i, int snap);           /* the focus, with its springs */

/* ui_menus.c: the menus the list's buttons open. */
void enter_folder(const char *name);
void folder_menu(const char *name);           /* a folder's △ menu */
void pl_menu(int k);                          /* a playlist's △ menu */
void open_playlist(int k);
void leave_playlist(void);
void music_focus_key(char *out, int n);       /* the focused Music row, to find again */
void music_refocus(const char *key);
int playlist_move_input(unsigned int p);      /* the list's buttons while a song is being moved */

#endif
