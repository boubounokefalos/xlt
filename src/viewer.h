/* XLT - full-screen picture viewer. */
#ifndef VIEWER_H
#define VIEWER_H

#include "entry.h"

void viewer_init(const char *app_dir); /* the remembered rotations (cache/rotate.txt) */
void viewer_open(const Entry *e);
/* Pictures from somewhere else (a folder in the file manager): count() and
 * get(i) are called on the UI thread; get fills path, title, size, mtime. */
typedef struct {
    int (*count)(void);
    int (*get)(int i, Entry *out);
} ViewerSource;
void viewer_open_source(const ViewerSource *src, int idx);
int viewer_is_open(void);
int viewer_covers(void);   /* fully opaque: nothing underneath needs drawing */
int viewer_input(unsigned int pressed);
int viewer_step(void);     /* 1 if a redraw is needed */
void viewer_draw(void);
void viewer_refresh(void); /* the library changed */
void viewer_hold(unsigned int held); /* every frame while open: the d-pad moves a zoomed picture */
int viewer_slideshow(void);          /* a slideshow is running (keeps the screensaver away) */

#endif
