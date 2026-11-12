/* XLT - live previews for the focused game (its ICON1.PMF, as the XMB plays
 * it) or video (muted, from 10% in), on the home tile and in the list. */
#ifndef XL_PREVIEW_H
#define XL_PREVIEW_H

#include "entry.h"

/* Every frame: what has the focus and may be previewed (NULL for nothing).
 * A preview starts once the focus has rested on it for a moment. */
void preview_target(const Entry *e);
int preview_step(void); /* 1 if a redraw is needed */
/* Draws e's preview fitted into the box; 1 if there was a picture to draw. */
int preview_draw(const Entry *e, int x, int y, int w, int h);
/* A video drawn without a still frame: queue one to be made (10% in). */
void preview_want_thumb(const Entry *e);

#endif
