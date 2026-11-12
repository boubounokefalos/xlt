/* XLT - search screen (□): on-screen key grid with live results. */
#ifndef SEARCH_H
#define SEARCH_H

#include "system.h"

void search_open(void);
int search_is_open(void);     /* open or still fading */
int search_covers(void);      /* fully opaque: nothing underneath needs drawing */
int search_input(unsigned int pressed);
int search_step(void);        /* 1 if a redraw is needed */
void search_draw(const SysStatus *st);
void search_refresh(void);    /* the library changed: rerun the query */

#endif
