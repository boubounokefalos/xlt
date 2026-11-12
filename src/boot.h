/* XLT - the boot screen: the logo draws in while the library is checked, with
 * a progress bar if that takes longer than the logo. Any button skips to the
 * launcher (the check carries on in the background). */
#ifndef BOOT_H
#define BOOT_H

#include "system.h"

void boot_init(void);
int boot_active(void);              /* shown, or still fading out */
int boot_covers(void);              /* fully opaque: nothing under it needs drawing */
int boot_update(unsigned int pressed); /* every frame while active; 1 = redraw */
void boot_draw(void);

#endif
