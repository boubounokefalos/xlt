/* XLT - Now Playing: the playing song full screen (big cover, time, what
 * comes next) with play/pause, previous/next, seeking, shuffle and repeat.
 * When nothing plays it offers to carry on where the music stopped last time. */
#ifndef NOWPLAYING_H
#define NOWPLAYING_H

#include "system.h"

void np_open(void);
int np_is_open(void);
int np_covers(void);       /* fully opaque: nothing underneath needs drawing */
int np_input(unsigned int pressed);
int np_step(void);         /* 1 if a redraw is needed */
void np_draw(const SysStatus *st);

#endif
