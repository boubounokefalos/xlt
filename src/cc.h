/* XLT - control center: a sheet that drops from the top, toggled with SELECT. */
#ifndef CC_H
#define CC_H

#include "system.h"

void cc_init(const char *app_dir);
void cc_toggle(void);
int cc_is_open(void);   /* open or still sliding */
int cc_input(unsigned int pressed);
int cc_step(void);      /* 1 if a redraw is needed */
void cc_draw(const SysStatus *st);

/* After a drawn frame with the sheet fully closed: 1 once per Capture press. */
int cc_take_capture(void);

/* USB mode: a full screen over everything while the PC has the Memory Stick. */
int cc_usb_screen(void);  /* shown, or still fading out */
int cc_usb_covers(void);  /* fully opaque: skip drawing what it hides */
int cc_usb_input(unsigned int pressed); /* takes every press while USB is on */
void cc_usb_draw(const SysStatus *st);

/* From the power callback: the PSP woke from sleep (re-apply brightness). */
void cc_on_resume(void);

#endif
