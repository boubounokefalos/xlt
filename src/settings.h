/* XLT - the Settings screen: XLT's own options, ARK's CFW settings,
 * plugins, and the PSP's system settings that games use. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "system.h"

void settings_open(void);
int settings_is_open(void);   /* open or still fading */
int settings_covers(void);    /* fully opaque: nothing underneath needs drawing */
int settings_input(unsigned int pressed);
int settings_step(void);      /* 1 if a redraw is needed */
void settings_draw(const SysStatus *st);

/* Sections, for the Settings row on the home screen. */
#define SETTINGS_SECTIONS 8
#define SETTINGS_RECENT 3
int settings_recent(int i);                /* section index, newest first */
const char *settings_section_name(int s);
const char *settings_section_sub(int s);   /* a few words on what is in it */
int settings_section_icon(int s);
void settings_open_section(int s);         /* opens Settings in that section */

#endif
