/* XLT - start games and homebrew through ARK's SystemControl. */
#ifndef LAUNCH_H
#define LAUNCH_H

#include "entry.h"

/* ISO drivers, values of ARK's sctrlSESetBootConfFileIndex() modes. */
enum {
    DRIVER_MARCH33 = 2,
    DRIVER_NP9660 = 3,
    DRIVER_INFERNO = 4,
};
#define DRIVER_DEFAULT DRIVER_NP9660 /* as psp5; Inferno and March33 stay selectable */

/* 1 if ARK's SystemControl answers (real PSP with CFW), 0 in e.g. PPSSPP. */
int launch_supported(void);

/* Hands the PSP to the game. `driver` applies to ISO/CSO/ZSO only.
 * Only returns on failure (negative). Copies what it needs from e first. */
int launch_entry(const Entry *e, int driver);

#endif
