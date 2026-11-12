/* XLT - the web browser screen. Pages are fetched and laid out by
 * webfetch.c; this is what you see and press:
 *
 *   ↑/↓ the previous / next link or field (scrolling when there is none
 *   close by), ←/→ a screen up / down, ✕ open the link or use the field,
 *   □ address or search, L/R back / forward, START top of the page,
 *   △ menu, ○ back (closes the browser on the first page). */
#ifndef WEB_H
#define WEB_H

#include "system.h"

void web_init(const char *app_dir);
/* url NULL: the last page, or the address bar if there is none. */
void web_open(const char *url);
void web_open_search(void);  /* opens with the address bar up, empty */
int web_is_open(void);       /* open or still fading */
int web_covers(void);        /* fully opaque */
int web_input(unsigned int pressed);
int web_step(void);
void web_draw(const SysStatus *st);
void web_shutdown(void);     /* before a launch, USB mode or exit */

/* The home row: a few places, then the browser itself. */
int web_tile_count(void);
const char *web_tile_name(int i);
const char *web_tile_sub(int i);
void web_tile_open(int i);
const char *web_last_title(void); /* "" if no page was opened yet */

/* The globe and the other icons of the second texture (tools/bake_webicons.py):
 * the browser's, and the control center's power dialog. */
enum { WEB_ICON_GLOBE, WEB_ICON_GLOBE_S, WEB_ICON_POWER = 7, WEB_ICON_RESTART, WEB_ICON_EXIT, WEB_ICON_CANCEL,
       WEB_ICON_USB };
void web_draw_icon(int id, int x, int y, int w, int h, unsigned int color);

#endif
