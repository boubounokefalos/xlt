/* XLT - XLT as ARK's launcher ("Start XLT at boot").
 *
 * ARK's launcher setting starts whatever is at <ARK folder>/VBOOT.PBP, which is ARK's own
 * menu unless something replaces it (psp5 has you do that by hand). So while the setting is
 * on, XLT puts a copy of itself there, keeping ARK's menu as VBOOT_ARKMENU.PBP, and keeps
 * the copy the same as the XLT in PSP/GAME; turned off, ARK's menu goes back. Started as
 * VBOOT.PBP, XLT still keeps its data in its own folder (VBOOT_XLT.TXT says where).
 *
 * Leaving to the XMB as the launcher would only start the launcher again, so the setting is
 * paused (turned off, with cache/launcher.paused) and switched back on at XLT's next start,
 * as psp5 does. */
#ifndef ARKLAUNCHER_H
#define ARKLAUNCHER_H

/* Early, after ark_init(): if XLT runs as VBOOT.PBP, dir (its data folder, ending in '/')
 * becomes the folder of the XLT it was copied from. */
void launcher_early(const char *self, char *dir, int cap);
int launcher_running(void); /* XLT was started as ARK's launcher */

/* On the background worker: launcher mode back on if it was paused; VBOOT.PBP made (or kept)
 * XLT, or ARK's menu put back, as the setting says. */
void launcher_sync(const char *dir);
void launcher_sync_now(void); /* the same, on the calling thread (after Settings saved it) */

/* Before XLT ends by going to the XMB. */
void launcher_before_exit(void);

#endif
