/* XLT - colour themes (dark ones and one light). The palette the whole UI draws with; picked
 * in Settings > XLT > Theme and applied at once. Folder yellow, file-type
 * colours, the logo's shapes and the button tints are not themed. */
#ifndef THEME_H
#define THEME_H

#include <psptypes.h>

typedef struct {
    const char *name;
    u32 bg;      /* the screen */
    u32 surface; /* cards, tiles, sheets */
    u32 raised;  /* a step up from surface: tracks, keys, focused-but-inactive */
    u32 line;    /* separators */
    u32 text, muted;
    u32 accent;  /* the highlighted row, an "on" card (white text sits on it) */
    u32 focus;   /* rings, small highlights */
    u32 pill;    /* toasts, the volume pill, slider tracks */
    u32 mark;    /* marked files: accent faded into the background */
    u32 accent_dk; /* accent, darker (an "off" switch on a highlighted row, the keyboard's Done) */
    int light;     /* dark text on a light screen: the button tints are drawn deeper */
} Theme;

#define THEME_COUNT 8
extern const Theme g_themes[THEME_COUNT];
extern const Theme *g_th; /* the one in use */

void theme_set(int i);    /* clamps; 0 = Night, the original look */

#endif
