/* XLT - per-game options: a side sheet from the right, opened with Triangle. */
#ifndef OPTS_H
#define OPTS_H

#include "entry.h"

void opts_open(const Entry *e);
int opts_is_open(void);
void opts_close(void);    /* saves what changed, slides out */   /* open or still sliding out */
int opts_input(unsigned int pressed); /* 1 if handled (always while open) */
int opts_step(void);      /* animation; 1 if a redraw is needed */
void opts_draw(void);

#endif
