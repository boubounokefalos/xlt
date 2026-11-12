/* XLT - safe start: a way in when something on the stick stops XLT from
 * starting. Holding L at launch, or two starts in a row that never settled,
 * runs XLT once from its own empty folder (safe/ next to the EBOOT): no saved
 * settings or caches are read, none are overwritten, and there is no library
 * scan, live preview or UMD watcher. Files, Settings and USB mode still work,
 * to put things right. The next start is a normal one.
 *
 * The decisions are portable (host-tested); main.c does the reading and writing. */
#ifndef SAFESTART_H
#define SAFESTART_H

enum { SAFE_NO, SAFE_HELD, SAFE_CRASHED };

#define SAFE_AFTER_FAILS 2 /* starts in a row that never settled */

extern int g_safe_start; /* this run: SAFE_* (SAFE_NO = a normal start) */

/* How many starts never settled, from the file's text (0 if it makes no sense). */
int safestart_parse(const char *text, int len);
int safestart_decide(int failed, int held);
/* What the file should say while this start is running. */
int safestart_next(int failed, int mode);
/* One line for the screen, NULL for a normal start. */
const char *safestart_message(int mode);

#endif
