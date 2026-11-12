/* XLT - the in-game screenshot plugin (plugin/xltshot), carried inside XLT and
 * installed by it.
 *
 * At every start (on the background worker): SEPLUGINS/xltshot.prx is written
 * if it is missing or isn't the one this XLT carries. If no PLUGINS.TXT mentions
 * it yet, other screenshot plugins that load in games are turned off and
 * "game, ms0:/SEPLUGINS/xltshot.prx, on" is added to SEPLUGINS/PLUGINS.TXT.
 * Once its line exists, on or off, the lists are the user's and are left alone. */
#ifndef SHOTPLUG_H
#define SHOTPLUG_H

void shotplug_install(void);

/* A note for the user after the first install, once. 1 if out got one. */
int shotplug_take_message(char *out, int n);

#endif
