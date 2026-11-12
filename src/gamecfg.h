/* XLT - per-game settings, favorites and the global settings file.
 *
 *   cache/games/<GAMEID>.cfg   only for games whose settings were changed
 *                              (homebrew without an ID uses its entry key)
 *   cache/favorites.txt        one path per line
 *   cache/hidden.txt           entries removed from the library (files untouched)
 *   cache/xlt.ini            launcher-wide settings
 * The per-game CPU clock lives in ARK's SETTINGS.TXT instead (see ark.h),
 * because ARK is what applies it when the game boots.
 */
#ifndef GAMECFG_H
#define GAMECFG_H

#include "entry.h"

typedef struct {
    int driver; /* DRIVER_* from launch.h */
} GameCfg;

void gamecfg_init(const char *app_dir);

void gamecfg_load(const Entry *e, GameCfg *out); /* defaults if no file */
void gamecfg_save(const Entry *e, const GameCfg *cfg);
void gamecfg_forget(const Entry *e); /* removes the game's .cfg (the game was deleted) */

int favorite_get(const char *path);
void favorite_set(const char *path, int on); /* saves the file */

/* "Remove from library" (pictures, music, video) only hides the entry; the
 * file is never touched. Games and homebrew are deleted instead (opts.c). */
int hidden_get(const char *path);
int hidden_set(const char *path, int on); /* -1 if the list is full */

/* Launcher-wide settings (xlt.ini). */
typedef struct {
    int brightness; /* 0..100 %, -1 = leave as is */
    int iso_driver; /* DRIVER_* for disc images without their own choice */
    int set_recent[3]; /* Settings sections last opened, newest first (home row) */
    int saver_min;     /* screensaver after this many idle minutes, 0 = off */
    int grid;          /* bit per library view (1 << VIEW_*), plus GRID_FILES: shown as a grid */
    int ui_sounds;     /* interface sounds on (1) or off (0) */
    int theme;         /* index into g_themes (theme.c), 0 = Night */
    int music_sort;    /* MSORT_* (musicsort.h): the Music list's order */
    int game_sort[2];  /* GSORT_* for the Games and Homebrew tabs (VIEW_GAMES, VIEW_HOMEBREW) */
    int autoplay;      /* when a video ends, play the next one in its folder (1) or not (0) */
    char audio_lang[4]; /* the audio language last picked in a video (ISO 639-2), "" = the file's default */
    int level;          /* music volume levelling: LEVEL_* (audiofmt.h), 1 = by song */
    char sub_lang[4];   /* the subtitle language last picked (ISO 639-2), "off", or "" = the file's default */
} GlobalCfg;

#define GRID_FILES (1 << 7)

extern GlobalCfg g_cfg;
void globalcfg_save(void);

/* Writes cache/groups.txt (Games / Homebrew tabs and folders, groups.h) on the worker. */
void groups_save(void);

#endif
