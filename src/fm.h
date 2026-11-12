/* XLT - file manager: browse ms0:/ef0:, open files with the right viewer
 * (pictures, music, video, text, PDF, games), copy / move / rename / delete
 * and new folders. */
#ifndef FM_H
#define FM_H

#include "system.h"
#include "entry.h"

void fm_init(const char *app_dir);
void fm_open(const char *path); /* NULL: where it was left (or the storage list) */
int fm_is_open(void);           /* open or still fading */
int fm_covers(void);            /* fully opaque: nothing underneath needs drawing */
int fm_input(unsigned int pressed);
int fm_step(void);              /* 1 if a redraw is needed; also finishes file jobs while closed */
void fm_draw(const SysStatus *st);

/* The Files row on the home screen: storage, then recent folders. */
#define FM_PLACES 3
int fm_place_count(void);
const char *fm_place_name(int i);
const char *fm_place_path(int i);
int fm_place_icon(int i);

int fm_busy(void); /* a copy / move / delete is running */

/* The focused file as an Entry, if it is a video, disc image or EBOOT.PBP
 * that can be previewed (and the file manager is showing), else NULL. */
const Entry *fm_preview_entry(void);

#endif
