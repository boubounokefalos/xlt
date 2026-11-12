/* XLT - the screensaver's picture: a clock, the date and a few quiet lights.
 * Pure drawing (only gfx calls), so tests/saver_preview.c can render it on the PC. */
#ifndef SAVERDRAW_H
#define SAVERDRAW_H

#include "system.h"

enum { SAVER_YMD, SAVER_MDY, SAVER_DMY }; /* the registry's date_format values */

typedef struct {
    SysStatus st;          /* hour, minute, battery, charging */
    int wday;              /* 0 = Sunday */
    int day, month, year;  /* month 1..12 */
    int h12;               /* the PSP's time format is 12-hour */
    int date_fmt;          /* SAVER_* */
    const char *music;     /* "Title · Artist", or NULL */
    int music_paused;
    unsigned frame;        /* 60 per second since the screensaver started */
    int show;              /* 0..256: how far it has faded in */
    int leaving;           /* fading out: the face rises instead of settling */
    int hint;              /* 0..256: "Press any button" (fades out after a few seconds) */
} SaverView;

/* Must be called once before the first draw (writes the texture out of the cache). */
void saverdraw_init(void);
void saverdraw(const SaverView *v);

#endif
