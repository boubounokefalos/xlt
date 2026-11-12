/* XLT - per-game plugin state from ARK's PLUGINS.TXT files. Portable (host-tested).
 *
 * Follows ARK's loader (core/systemctrl/src/plugin.c): files are read in a
 * fixed order (ARK folder, ms0:/SEPLUGINS, ef0:/SEPLUGINS), lines top to
 * bottom; a matching "on" line adds the plugin, a matching "off" line removes
 * it again (same path text). A game ID in the runlevel matches that game. */
#ifndef PLUGTEXT_H
#define PLUGTEXT_H

#define PLUG_PATH 128
#define PLUG_MAX 48

enum { PLUG_GLOBAL = -1, PLUG_OFF = 0, PLUG_ON = 1 };

typedef struct {
    char path[PLUG_PATH]; /* as written in the file */
    int file;             /* index of the last file that mentions it */
    int global_on;        /* state for this game without its own lines */
    int override;         /* PLUG_GLOBAL, PLUG_OFF or PLUG_ON from "<ID>, path, ..." lines */
} PlugState;

/* texts[i] = contents of file i in ARK's order (NULL if missing). pops = the
 * game runs as PS1 (pops runlevel) rather than as a PSP game. Returns count. */
int plugtext_list(const char *const *texts, int nfiles, const char *id, int pops, PlugState *out, int max);

/* Rewrites one file: drops its "<id>, <path>, ..." lines and, unless
 * override is PLUG_GLOBAL, appends one at the end (after the global lines,
 * so ARK applies it last). Returns the new length or -1. */
int plugtext_set(const char *text, const char *id, const char *path, int override, char *out, int cap);

/* One global line ("always, ms0:/seplugins/x.prx, on"), for the Settings tab.
 * Lines whose runlevel is a game ID or a homebrew path are left out. */
typedef struct {
    char path[PLUG_PATH];
    char runlevel[16];  /* lower case, as written: "always", "game", "vsh", ... */
    int on;
    int file, line;     /* which file, and which line (0-based) in it */
} PlugLine;

/* Global lines of one file's text. Returns count. */
int plugtext_lines(const char *text, int file, PlugLine *out, int max);

/* Rewrites one line's state, and its runlevel unless runlevel is NULL; the
 * path and every other line stay byte for byte. Returns the new length, or
 * -1 if that line isn't a plugin line or it doesn't fit. */
int plugtext_set_line(const char *text, int line, const char *runlevel, int on, char *out, int cap);

/* Where ARK looks for a plugin's file: a path with a device ("ms0:/x.prx") as it is,
 * "/SEPLUGINS/x.prx" on ms0:, and a bare name ("leda.prx") in ARK's own folder
 * (ark_dir, ending in '/'). Returns 0, or -1 for a bare name when ARK's folder is unknown. */
int plugtext_resolve(const char *path, const char *ark_dir, char *out, int n);

/* ---- the in-game screenshot plugin (src/shotplug.c) ---- */

/* 1 if any plugin line in text (any runlevel, on or off) loads a file called
 * `name` (compared without case, path ignored). */
int plugtext_mentions(const char *text, const char *name);

/* text with `line` added at the end, in the file's own line endings. Returns the
 * new length or -1. */
int plugtext_append(const char *text, const char *line, char *out, int cap);

/* 1 if a plugin's file name says it takes screenshots ("screenshot.prx",
 * "sshot.prx", "ScreenCap.prx", ...). XLT's own xltshot.prx is not one of them. */
int plugtext_is_screenshot(const char *path);

/* Turns off every "on" screenshot plugin that loads in games (any runlevel but
 * the XMB's and the launcher's). Writes the result to out and the file names
 * turned off, comma separated, to names. Returns how many, or -1 if out is too small. */
int plugtext_off_screenshots(const char *text, char *out, int cap, char *names, int ncap);

#endif
