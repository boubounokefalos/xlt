/* XLT - shared between the screens (ui.c) and the sheets (opts.c, cc.c). */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <pspctrl.h>
#include "gfx.h"
#include "entry.h"
#include "system.h"

#include "theme.h"

/* The palette: from the theme in use (theme.c). */
#define C_BG      (g_th->bg)
#define C_SURFACE (g_th->surface)
#define C_RAISED  (g_th->raised)
#define C_LINE    (g_th->line)
#define C_TEXT    (g_th->text)
#define C_MUTED   (g_th->muted)
#define C_ACCENT  (g_th->accent)
#define C_FOCUS   (g_th->focus)
#define C_PILL    (g_th->pill)
#define C_MARK    (g_th->mark)
#define C_ACCENT_DK (g_th->accent_dk)
#define C_WHITE   RGB(0xffffff)
#define C_LOW     RGB(0xe5534b)
#define C_CHARGE  RGB(0x57ab5a)

#define C_BTN_CROSS    RGB(0x8fb3ee)
#define C_BTN_CIRCLE   RGB(0xec8e8e)
#define C_BTN_TRIANGLE RGB(0x7fd3ad)
#define C_BTN_SQUARE   RGB(0xe0a3e0)
/* The button tints above are pastel for the dark themes; on the light theme
 * the same colour comes out deeper (ui_draw_hints does it for every hint). */
static inline u32 btn_tint(u32 c)
{
    if (!g_th->light) return c;
    return c == C_BTN_CROSS ? RGB(0x2f6fd0) : c == C_BTN_CIRCLE ? RGB(0xc9473f)
         : c == C_BTN_TRIANGLE ? RGB(0x1f8a5b) : c == C_BTN_SQUARE ? RGB(0xa845a8) : c;
}

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define DOT " \xC2\xB7 "

typedef struct {
    int icon;        /* button glyph, or -1 for a text key */
    const char *key; /* used when icon < 0 */
    u32 tint;
    const char *label;
} Hint;

/* A row of button hints centred on cx at y. */
void ui_draw_hints(const Hint *hints, int n, int cx, int y);
void ui_draw_battery(int x, int y, const SysStatus *st);
void ui_draw_status_bar(const char *title, const SysStatus *st); /* title left; clock, battery right */
/* Dims everything drawn so far; t = 0..256. */
void ui_dim(int t);

void ui_toast(const char *msg);
void ui_request_launch(const Entry *e);
void ui_request_exit(void);
void ui_library_changed(void); /* the library views were rebuilt */
void ui_open_music(void);       /* the Music list, from the control center */
void ui_open_removed(void);     /* the Removed list, from Settings */
void ui_open_move(const Entry *e); /* Games / Homebrew: the Move to menu (tab and folder) */
/* Music (the song's options sheet): */
void ui_music_sort(int dir);                 /* the next / previous order (g_cfg.music_sort), applied and saved */
void ui_game_sort(int tab, int dir);         /* the same for Games / Homebrew (g_cfg.game_sort) */
int ui_music_in_playlist(void);              /* the Music list shows an open playlist */
int pl_row(int k);                          /* the Music list row of playlist k */
/* Add to playlist for many songs (an artist's or an album's): takes over list (malloc'd). */
void ui_open_add_many(const char *title, Entry *list, int n);
void ui_open_add_to_playlist(const Entry *e); /* the Add to playlist menu, over the sheet */
void ui_playlist_remove(const Entry *e);     /* out of the open playlist (saved) */
void ui_playlist_move(const Entry *e);       /* ↑/↓ move it in the open playlist, ✕ saves */

/* Square cover art for a song, corners rounded against bg (or a note). */
void ui_draw_cover(const Entry *e, int x, int y, int s, int r, u32 bg);

/* Formatting shared by the list preview and the details panel. */
const char *kind_name(int kind);
const char *region_name(const char *id);
void format_size(char *b, int n, uint64_t bytes);
void format_date(char *b, int n, uint32_t packed_mtime);

#endif
