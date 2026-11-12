#ifndef UI_H
#define UI_H

#include "input.h"
#include "system.h"
#include "entry.h"

void ui_init(void);
/* Handles input and advances animations by one frame.
 * Returns 1 if the screen must be redrawn this frame. */
int ui_update(const Input *in, int status_changed);
void ui_draw(const SysStatus *st);

/* After a drawn frame: 1 and the entry if the user asked to start something. */
int ui_take_launch(Entry *out);
void ui_toast(const char *msg);
int ui_take_exit(void); /* 1 once "Exit to XMB" was chosen */
void ui_prefetch(void);  /* asks for the home screen's thumbnails (while the boot screen shows) */

#endif
