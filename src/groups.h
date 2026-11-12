/* XLT - how Games and Homebrew are organized inside XLT only: which tab an
 * entry is shown in (if not the one its type picks) and folders under All.
 * Files never move. Pure: no file access here (gamecfg.c loads and saves
 * cache/groups.txt), so it is tested on the host.
 *
 *   F <tab> <name>               a folder (kept even while empty)
 *   I <tab> <folder> <path>      an entry shown in <tab>, in <folder> ("" = All)
 * Fields are separated by tabs; <tab> is G (Games) or H (Homebrew).
 */
#ifndef GROUPS_H
#define GROUPS_H

#define GROUPS_TABS 2        /* VIEW_GAMES, VIEW_HOMEBREW */
#define GROUP_NAME 32        /* bytes with the NUL */
#define GROUP_FOLDERS 16     /* per tab */
#define GROUP_ITEMS 512

void groups_reset(void);
int groups_parse(const char *text);         /* replaces everything; number of lines used */
int groups_serialize(char *out, int cap);   /* bytes written, or -1 if cap is too small */
int groups_serialized_max(void);            /* a cap that always fits */

/* The tab an entry is shown in: def (from its type) unless it was moved. */
int groups_tab(const char *path, int def);
/* Its folder in that tab, "" for All (also when the folder no longer exists). */
const char *groups_folder(const char *path);
/* Moves an entry to tab / folder ("" = All). 0 ok, -1 list full, -2 no such folder. */
int groups_set(const char *path, int def_tab, int tab, const char *folder);

int groups_folder_count(int tab);
const char *groups_folder_name(int tab, int i); /* sorted by name */
int groups_folder_find(int tab, const char *name); /* index or -1, case ignored */
/* 0 ok, -1 full, -2 a folder with that name exists, -3 not a usable name. */
int groups_folder_add(int tab, const char *name);
int groups_folder_rename(int tab, const char *old_name, const char *new_name);
void groups_folder_delete(int tab, const char *name); /* its entries go back to All */

/* Turns typed text into a folder name (trims, drops tabs/newlines, cuts to fit). 0 if empty. */
int groups_clean_name(const char *in, char *out);

#endif
