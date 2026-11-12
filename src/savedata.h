/* XLT - Save data: the games' saves (PSP/SAVEDATA) and their backups
 * (SAVEDATA_BACKUP/<date>/ at the root of the same stick): look through them
 * with their icons, back a save up, restore a backup, delete either. */
#ifndef SAVEDATA_H
#define SAVEDATA_H

#include "system.h"

void sd_open(void);
int sd_is_open(void);
int sd_covers(void);       /* fully opaque: nothing underneath needs drawing */
int sd_input(unsigned int pressed);
int sd_step(void);         /* 1 if a redraw is needed; also finishes its jobs while closed */
void sd_draw(const SysStatus *st);

#endif
