#ifndef HOST_PSPRTC_H
#define HOST_PSPRTC_H
/* Host stand-in (tests/sd_preview.c): a fixed day. */
#include "pspiofilemgr.h"
static inline int sceRtcGetCurrentClockLocalTime(ScePspDateTime *t)
{ t->year = 2026; t->month = 11; t->day = 8; t->hour = 12; t->minute = 0; t->second = 0; t->microsecond = 0; return 0; }
#endif
