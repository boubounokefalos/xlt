/* XLT - the colour themes. See theme.h. Pure data (also built into the PC previews). */
#include "theme.h"
#include "gfx.h"

const Theme g_themes[THEME_COUNT] = {
    /* name        bg            surface       raised        line          text          muted
                   accent        focus         pill          mark          accent_dk     light */
    {"Night",      RGB(0x0d1117), RGB(0x161b22), RGB(0x21262d), RGB(0x21262d), RGB(0xe6edf3), RGB(0x8b949e),
                   RGB(0x2f6feb), RGB(0x58a6ff), RGB(0x30363d), RGB(0x1c2d4a), RGB(0x1d4fb0), 0},
    {"Midnight",   RGB(0x0a0f1e), RGB(0x111a2f), RGB(0x1a2540), RGB(0x1a2540), RGB(0xe4ecf7), RGB(0x8794b0),
                   RGB(0x0b7bc4), RGB(0x46c0f5), RGB(0x263354), RGB(0x12304a), RGB(0x095a90), 0},
    {"Forest",     RGB(0x0b1310), RGB(0x121e19), RGB(0x1b2b23), RGB(0x1b2b23), RGB(0xe2eee7), RGB(0x87a194),
                   RGB(0x23845a), RGB(0x5dd39e), RGB(0x25392f), RGB(0x143426), RGB(0x18603f), 0},
    {"Ember",      RGB(0x140e0b), RGB(0x1e1612), RGB(0x2b1f19), RGB(0x2b1f19), RGB(0xf2e8e2), RGB(0xa6948a),
                   RGB(0xc24914), RGB(0xff9a5a), RGB(0x3a2a22), RGB(0x3a1d10), RGB(0x8f350e), 0},
    {"Plum",       RGB(0x110b16), RGB(0x1b1322), RGB(0x271c31), RGB(0x271c31), RGB(0xede4f3), RGB(0x9d8dad),
                   RGB(0x8340c9), RGB(0xc393ff), RGB(0x362845), RGB(0x2b1a3f), RGB(0x602e96), 0},
    {"Graphite",   RGB(0x111111), RGB(0x1b1b1b), RGB(0x262626), RGB(0x262626), RGB(0xececec), RGB(0x9a9a9a),
                   RGB(0x52525b), RGB(0xe4e4e7), RGB(0x333333), RGB(0x2c2c33), RGB(0x3f3f46), 0},
    /* Gruvbox (dark): its warm greys and cream text, the faded blue for rows, yellow rings */
    {"Gruvbox",    RGB(0x282828), RGB(0x32302f), RGB(0x3c3836), RGB(0x3c3836), RGB(0xebdbb2), RGB(0xa89984),
                   RGB(0x458588), RGB(0xfabd2f), RGB(0x504945), RGB(0x2f3b38), RGB(0x076678), 0},
    /* Light: dark text on near-white; rows and rings in a deeper blue so white text still reads */
    {"Light",      RGB(0xeef1f5), RGB(0xffffff), RGB(0xdde3ea), RGB(0xd0d7de), RGB(0x1f2328), RGB(0x5d6670),
                   RGB(0x0969da), RGB(0x0a5fc4), RGB(0xd0d7de), RGB(0xd6e8fd), RGB(0x0550ae), 1},
};

const Theme *g_th = &g_themes[0];

void theme_set(int i)
{
    g_th = &g_themes[i >= 0 && i < THEME_COUNT ? i : 0];
}
