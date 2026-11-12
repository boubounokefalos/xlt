/* XLT - the UMD drive. A thread watches for a disc (as ARK's arkMenu and psp5
 * do); when one goes in it is spun up and its PARAM.SFO read, and the home
 * screen shows a UMD card before Homebrew. */
#ifndef UMD_H
#define UMD_H

#include "entry.h"

enum {
    UMD_NONE,    /* no disc (or no drive: PSP Go) */
    UMD_READING, /* a disc went in, it is spinning up */
    UMD_GAME,    /* a game: umd_entry() has its title, ID and ICON0 */
    UMD_VIDEO,   /* a UMD Video: the XMB plays those */
    UMD_OTHER,   /* something we can't use (unreadable, or neither) */
};

void umd_start(void);
void umd_stop(void);

/* Main thread, every frame: 1 if the state changed since the last call. */
int umd_poll(void);
int umd_state(void);
const Entry *umd_entry(void); /* the disc as an entry (KIND_UMD), valid in UMD_GAME / UMD_VIDEO */

#define UMD_ICON0 "disc0:/PSP_GAME/ICON0.PNG"
#define UMD_ICON1 "disc0:/PSP_GAME/ICON1.PMF"

#endif
