/* XLT - boot screen drawing. See bootdraw.h.
 *
 * The logo's four cards come from build/logo_data.h, baked from the same SVG code
 * as the XMB icon (tools/make_logo.py), so the two always match. Each one pops in a
 * little after the one before (scale with a slight overshoot, rising, fading
 * in), left to right; then the bar fades in under them if the library is
 * still being checked. Leaving, everything fades out and grows a touch while
 * the launcher shows through. */
#include <pspgu.h>
#include "gfx.h"
#include "bootdraw.h"
#include "logo_data.h"
#include "theme.h"

#define B_BG (g_th->bg)        /* the theme's (the cards keep their own colours) */
#define B_TRACK (g_th->raised)
#define B_MUTED (g_th->muted)

#define B_SHAPE LOGO_BOOT_CELL /* a texture cell on screen, px; logo_boot_x places each one */
#define B_CY 112
#define B_BAR_W 150
#define B_BAR_H 3
#define B_BAR_Y 172
#define B_LINE_Y 182
#define B_HINT_Y 250

static const u32 s_tint[4] = {RGB(0x7fd3ad), RGB(0xec8e8e), RGB(0x8fb3ee), RGB(0xe0a3e0)}; /* as the cards */

static float clampf(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* Overshoots a little past 1 and settles: the cards "pop". */
static float ease_out_back(float p)
{
    const float c1 = 1.05f, c3 = c1 + 1; /* a soft overshoot */
    float q = p - 1;
    return 1 + c3 * q * q * q + c1 * q * q;
}

static u32 alpha(u32 c, int a256)
{
    if (a256 < 0) a256 = 0;
    if (a256 > 256) a256 = 256;
    return WITH_ALPHA(c, (((c >> 24) & 0xFF) * (u32)a256) >> 8);
}

/* x0..x0+w of the bar's track, clipped to [lo, hi). */
static void bar_rect(int x, int w, int lo, int hi, u32 c)
{
    if (x < lo) { w -= lo - x; x = lo; }
    if (x + w > hi) w = hi - x;
    if (w > 0)
        gfx_rect(x, B_BAR_Y, w, B_BAR_H, c);
}

void bootdraw(const BootView *v)
{
    int keep = 256 - v->leave; /* everything fades together while leaving */
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, alpha(B_BG, keep));

    /* the logo's four cards */
    float grow = 1 + 0.06f * v->leave / 256.0f;
    gfx_bind(logo_tex, GU_PSM_8888, LOGO_TEX_W, LOGO_TEX_H, LOGO_TEX_W, 0);
    for (int i = 0; i < 4; i++) {
        float p = clampf((v->frame - 8 - i * 16) / 40.0f); /* one after another, calmly */
        if (p <= 0)
            continue;
        float e = ease_out_back(p);
        float s = B_SHAPE * (0.55f + 0.45f * e) * grow;
        int a = (int)(clampf(p * 1.8f) * keep);
        int cx = SCREEN_W / 2 + logo_boot_x[i];
        float cy = B_CY + (1 - e) * 10;
        int x0 = (int)(cx - s / 2), y0 = (int)(cy - s / 2), x1 = (int)(cx + s / 2), y1 = (int)(cy + s / 2);
        gfx_sprite(x0, y0, x1, y1, i * LOGO_CELL, 0, (i + 1) * LOGO_CELL, LOGO_CELL, alpha(0xFFFFFFFFu, a));
    }
    gfx_bind_atlas();

    /* the library check */
    int show = v->bar_show * keep >> 8;
    if (v->progress >= 0 && show > 0) {
        int x = (SCREEN_W - B_BAR_W) / 2;
        gfx_rect(x, B_BAR_Y, B_BAR_W, B_BAR_H, alpha(B_TRACK, show));
        if (v->indefinite) { /* a segment that keeps moving: we don't know how much there is */
            int seg = 36, pos = (v->frame * 2) % (B_BAR_W + seg) - seg;
            bar_rect(x + pos, seg, x, x + B_BAR_W, alpha(s_tint[(v->frame / 90) % 4], show));
        } else { /* filled in the cards' colours, a quarter each */
            int fill = B_BAR_W * (v->progress > 256 ? 256 : v->progress) >> 8;
            for (int k = 0; k < 4; k++)
                bar_rect(x + k * B_BAR_W / 4, B_BAR_W / 4 + 1, x, x + fill, alpha(s_tint[k], show));
        }
        if (v->line)
            gfx_text_center(FONT_SMALL, SCREEN_W / 2, B_LINE_Y, v->line, alpha(B_MUTED, show));
    }
    if (v->hint > 0)
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, B_HINT_Y, "Press any button to skip", alpha(B_MUTED, v->hint * keep >> 8));
}
