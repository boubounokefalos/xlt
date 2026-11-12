/* XLT - screensaver: after a few idle minutes (Settings > Power & Display),
 * a lock-screen-style clock fades in over whatever was open. Any button wakes
 * it, and that press is not passed on. */
#ifndef SAVER_H
#define SAVER_H

#include "input.h"
#include "system.h"

void saver_init(void);
/* Every frame, before ui_update(): counts idle time, starts and wakes the
 * screensaver, and clears in->pressed while it is up or being woken.
 * Returns 1 when it needs a redraw (30 times a second while it shows). */
int saver_update(Input *in);
int saver_is_open(void);  /* showing or fading */
int saver_covers(void);   /* fully opaque: nothing underneath needs drawing */
void saver_draw(const SysStatus *st);
void saver_start(void);   /* now, from Settings */
void saver_on_resume(void); /* power callback: woke from sleep */

#endif
