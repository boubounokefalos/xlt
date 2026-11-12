/* XLT - the screensaver's picture.
 *
 * The theme's background with two soft lights (the accent blue and a violet
 * mixed from it) drifting slowly on Lissajous paths, the date over a large
 * light clock whose colon breathes, the playing song in a pill under it, and
 * the battery in the top-right corner like the status bar. Everything comes
 * from saver_data.h (baked by tools/bake_saver.py): a texture switch for the
 * lights, one for the digits, then back to the atlas for the small text.
 */
#include <stdio.h>
#include <pspkernel.h>
#include <pspgu.h>
#include "saverdraw.h"
#include "ui_internal.h"
#include "saver_data.h"

#define MARGIN 12
#define DATE_Y 64          /* top of the date line */
#define CLOCK_INK_Y 90     /* top of the digits' ink */
#define PILL_Y 174
#define PILL_H 26
#define HINT_Y 244

static const char *const WDAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MONTH[12] = {"January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December"};

void saverdraw_init(void)
{
    sceKernelDcacheWritebackRange(saver_pixels, sizeof(saver_pixels));
    sceKernelDcacheWritebackRange(saver_glow, sizeof(saver_glow));
}

/* sin of a 16-bit phase, Q12, interpolated between the table's 256 steps. */
static int isin(unsigned phase)
{
    phase &= 0xFFFF;
    int i = phase >> 8, f = phase & 255;
    int a = saver_sin[i], b = saver_sin[(i + 1) & 255];
    return a + (((b - a) * f) >> 8);
}

/* A phase that turns once every `sec` seconds at 60 frames a second. */
static unsigned turn(unsigned frame, int sec, unsigned offset)
{
    return (unsigned)((unsigned long long)frame * 65536u / (unsigned)(sec * 60)) + offset;
}

static void glow(int cx, int cy, int size, u32 c)
{
    int h = size / 2;
    gfx_sprite(cx - h, cy - h, cx + h, cy + h, 0, 0, SAVER_GLOW_SIZE, SAVER_GLOW_SIZE, c);
}

static int clock_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        w += saver_glyphs[*s == ':' ? 10 : *s - '0'].adv;
    return w;
}

/* y = top of the line; the colon gets its own colour. */
static void clock_text(int x, int y, const char *s, u32 c, u32 colon)
{
    for (; *s; s++) {
        const Glyph *g = &saver_glyphs[*s == ':' ? 10 : *s - '0'];
        gfx_sprite(x + g->ox, y + g->oy, x + g->ox + g->w, y + g->oy + g->h, g->x, g->y, g->x + g->w, g->y + g->h,
                   *s == ':' ? colon : c);
        x += g->adv;
    }
}

void saverdraw(const SaverView *v)
{
    int show = v->show < 0 ? 0 : v->show > 256 ? 256 : v->show;
    unsigned f = v->frame;
    /* arriving, the face settles from a little below; leaving, it lifts away */
    int dy = v->leaving ? -((256 - show) * 18 >> 8) : (256 - show) * 10 >> 8;

    gfx_set_opacity(show);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);

    gfx_bind(saver_glow, GU_PSM_8888, SAVER_GLOW_SIZE, SAVER_GLOW_SIZE, SAVER_GLOW_SIZE, 0);
    /* Two lights, ~40-90 s per loop, each breathing a little on its own. */
    {
        int ax = 140 + (isin(turn(f, 53, 0)) * 70 >> 12);
        int ay = 196 + (isin(turn(f, 71, 0x4000)) * 34 >> 12);
        int aa = 72 + (isin(turn(f, 23, 0x2000)) * 14 >> 12);
        glow(ax, ay, 440, WITH_ALPHA(C_ACCENT, aa));

        int bx = 350 + (isin(turn(f, 61, 0x8000)) * 60 >> 12);
        int by = 78 + (isin(turn(f, 43, 0xC000)) * 30 >> 12);
        int ba = 46 + (isin(turn(f, 29, 0x6000)) * 10 >> 12);
        glow(bx, by, 360, WITH_ALPHA(color_lerp(C_ACCENT, C_BTN_SQUARE, 150), ba));
    }

    /* The clock. */
    gfx_bind(saver_pixels, GU_PSM_4444, SAVER_TEX_W, SAVER_TEX_H, SAVER_TEX_W, 1);
    char clk[8];
    int hour = v->st.hour;
    const char *ampm = NULL;
    if (v->h12) {
        ampm = hour < 12 ? "AM" : "PM";
        hour %= 12;
        if (hour == 0)
            hour = 12;
        snprintf(clk, sizeof(clk), "%d:%02d", hour, v->st.minute);
    } else {
        snprintf(clk, sizeof(clk), "%02d:%02d", hour, v->st.minute);
    }
    int cw = clock_width(clk);
    int ampm_w = ampm ? gfx_text_width(FONT_BOLD, ampm) + 6 : 0;
    int cx = (SCREEN_W - cw - ampm_w) / 2;
    int line_y = CLOCK_INK_Y + dy - SAVER_DIGIT_TOP;
    /* the colon breathes: 45%..100% over two seconds */
    int ca = 186 + (isin(turn(f, 2, 0x4000)) * 69 >> 12);
    clock_text(cx, line_y, clk, C_TEXT, WITH_ALPHA(C_TEXT, ca));

    gfx_bind_atlas();

    if (ampm) /* sits on the digits' baseline */
        gfx_text(FONT_BOLD, cx + cw + 6, CLOCK_INK_Y + dy + SAVER_DIGIT_H - gfx_line_h(FONT_BOLD) + 4, ampm,
                 C_MUTED);

    /* The date, over the clock. */
    char date[48];
    const char *wd = WDAY[(unsigned)v->wday % 7], *mo = MONTH[(unsigned)(v->month - 1) % 12];
    if (v->date_fmt == SAVER_MDY)
        snprintf(date, sizeof(date), "%s, %s %d", wd, mo, v->day);
    else
        snprintf(date, sizeof(date), "%s, %d %s", wd, v->day, mo);
    gfx_text_center(FONT_BODY, SCREEN_W / 2, DATE_Y + dy, date, C_MUTED);

    /* Now playing, in a pill under the clock. */
    if (v->music) {
        const int icon = 13, pad = 12, max_w = 300;
        int tw = gfx_text_width(FONT_SMALL, v->music);
        int w = pad + icon + 8 + tw + pad;
        if (w > max_w) {
            w = max_w;
            tw = max_w - (pad + icon + 8 + pad);
        }
        int x = (SCREEN_W - w) / 2, y = PILL_Y + dy;
        gfx_rrect(x, y, w, PILL_H, PILL_H / 2, WITH_ALPHA(C_SURFACE, 200));
        u32 c = v->music_paused ? C_MUTED : C_FOCUS;
        gfx_icon(ICON_MUSIC, x + pad, y + (PILL_H - icon) / 2, icon, icon, c);
        gfx_text_fit(FONT_SMALL, x + pad + icon + 8, y + (PILL_H - gfx_line_h(FONT_SMALL)) / 2, tw, v->music,
                     v->music_paused ? C_MUTED : C_TEXT);
    }

    /* Battery, where the status bar keeps it. */
    if (v->st.battery >= 0) {
        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", v->st.battery);
        int x = SCREEN_W - MARGIN - gfx_text_width(FONT_SMALL, pct);
        gfx_text(FONT_SMALL, x, 5, pct, v->st.charging ? C_CHARGE : C_MUTED);
        ui_draw_battery(x - 8 - 22, 9, &v->st);
    }

    if (v->hint > 0) {
        int h = v->hint > 256 ? 256 : v->hint;
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, HINT_Y, "Press any button",
                        WITH_ALPHA(C_MUTED, (u32)(255 * h) >> 8));
    }
    gfx_set_opacity(256);
}
