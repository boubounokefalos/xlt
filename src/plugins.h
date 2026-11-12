/* XLT - per-game plugin overrides in ARK's PLUGINS.TXT files (see plugtext.h). */
#ifndef PLUGINS_H
#define PLUGINS_H

#include "entry.h"
#include "plugtext.h"

/* 1 if ARK can load plugins per game for e: ARK present, a game ID, and not
 * homebrew (ARK only matches game IDs for PSP and PS1 games). */
int plugins_supported(const Entry *e);

/* Plugins relevant to games, with their state for e. Returns count. */
int plugins_for(const Entry *e, PlugState *out, int max);

/* Writes e's override for plugin p (PLUG_GLOBAL removes it). 0 on success.
 * Applies the next time the game starts. */
int plugins_set(const Entry *e, const PlugState *p, int override);

/* Global plugin lines of every PLUGINS.TXT, in ARK's file order (Settings). */
int plugins_global(PlugLine *out, int max);
/* Sets one line's runlevel (NULL keeps it) and on/off. 0 on success; -1 if
 * the file changed under it. */
int plugins_global_set(const PlugLine *p, const char *runlevel, int on);

/* The PLUGINS.TXT files in ARK's order (its folder, ms0:/SEPLUGINS, ef0:/SEPLUGINS). Returns the count (up to 3). */
int plugins_file_paths(char paths[3][160]);
/* A whole file (up to 32 KB) as a string to free(), or NULL. */
char *plugins_read_text(const char *path);
/* The same, and whether a NULL means the file isn't there (1) or couldn't be read (0): a list
 * that exists but can't be read must never be written over. */
char *plugins_read_file(const char *path, int *missing);
int plugins_write_text(const char *path, const char *text, int len); /* 0 on success */

#endif
