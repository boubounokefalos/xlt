/* XLT - screensaver controller.
 *
 * Idle time is counted in frames (the main loop runs once per vblank). It
 * doesn't start during full-screen video, and a video's running time doesn't
 * count. While it shows:
 *   - it redraws 30 times a second (the lights drift, the colon breathes) and
 *     everything underneath is skipped; previews pause (ui.c);
 *   - once it has faded in, the backlight eases down to half the user's level,
 *     and comes back at once on wake; not when the firmware's own backlight
 *     auto-off is due by then (setting the level could light the panel again);
 *   - the PSP's own date and time formats are used (read from the registry on
 *     the background worker each time it starts).
 * The press that wakes it is eaten, and so is that button's auto-repeat until
 * every button is released.
 */
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include "saver.h"
#include "saverdraw.h"
#include "ui_internal.h"
#include "gamecfg.h"
#include "music.h"
#include "viewer.h"
#include "sysctl.h"
#include "video.h"

#define BUTTONS (PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT | PSP_CTRL_DOWN | PSP_CTRL_LEFT | \
                 PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS |     \
                 PSP_CTRL_SQUARE)
#define FADE_IN 48   /* frames */
#define FADE_OUT 16
#define HINT_FRAMES (4 * 60)
#define DIM_STEPS 6  /* the backlight goes down in 6 steps... */
#define DIM_EVERY 10 /* ...10 frames apart */

enum { OFF, IN, ON, OUT };

static struct {
    int phase;
    int t;              /* frames in this phase */
    unsigned idle;      /* frames without a press */
    int eat;            /* swallow presses until every button is up */
    unsigned frame;     /* animation clock */
    int last_min;
    int wday, day, month, year;
    int dim_from;       /* backlight % before dimming, -1 = not dimmed */
    int dim_step;
    int shown;          /* frames fully shown, for the hint */
} S = {.dim_from = -1, .last_min = -1};

static volatile int s_resumed;

void saver_init(void)
{
    saverdraw_init();
}

void saver_on_resume(void) { s_resumed = 1; }
int saver_is_open(void) { return S.phase != OFF; }
int saver_covers(void) { return S.phase == ON; }

void saver_start(void)
{
    if (S.phase == ON || S.phase == IN)
        return;
    system_read_formats(); /* changed in the XMB or Settings? arrives within the fade */
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    S.frame = (unsigned)(tick / 16667); /* the lights pick up where the wall clock puts them */
    S.last_min = -1;
    S.phase = IN;
    S.t = 0;
    S.dim_step = 0;
    S.shown = 0;
}

static void undim(void)
{
    if (S.dim_from < 0)
        return;
    sysctl_brightness_set(g_cfg.brightness >= 0 ? g_cfg.brightness : S.dim_from);
    S.dim_from = -1;
}

static void wake(void)
{
    undim();
    S.phase = OUT;
    S.t = 0;
}

int saver_update(Input *in)
{
    unsigned pressed = in->pressed & BUTTONS;
    if (S.eat) {
        if (!(in->held & BUTTONS))
            S.eat = 0;
        in->pressed = 0;
        pressed = 0;
    }
    if (pressed || video_is_open() || viewer_slideshow())
        S.idle = 0;
    else if (S.idle < 0xFFFFFFF0u)
        S.idle++;

    if (s_resumed) { /* back from sleep: show what was open */
        s_resumed = 0;
        S.idle = 0;
        if (S.phase == IN || S.phase == ON) {
            S.dim_from = -1; /* the control center re-applies the user's level */
            wake();
        }
    }

    switch (S.phase) {
    case OFF:
        if (g_cfg.saver_min > 0 && S.idle >= (unsigned)g_cfg.saver_min * 3600u)
            saver_start();
        if (S.phase == OFF)
            return 0;
        break;
    case IN:
    case ON:
        if (pressed) {
            in->pressed = 0;
            S.eat = 1;
            wake();
            return 1;
        }
        in->pressed = 0;
        break;
    case OUT:
        break;
    }

    S.frame++;
    S.t++;
    if (S.phase == ON)
        S.shown++;
    if (S.phase == IN && S.t >= FADE_IN) {
        S.phase = ON;
        S.t = 0;
    } else if (S.phase == OUT && S.t >= FADE_OUT) {
        S.phase = OFF;
        S.idle = 0;
        return 1;
    }

    /* Ease the backlight down once it is fully up. */
    if (S.phase == ON && S.dim_step < DIM_STEPS && S.t % DIM_EVERY == 0 && sysctl_brightness_available()) {
        if (S.dim_step == 0) {
            int dark = system_backlight_off() > 0 && S.idle + 2 * 60 >= (unsigned)system_backlight_off() * 60u;
            S.dim_from = dark ? -1 : g_cfg.brightness >= 0 ? g_cfg.brightness : sysctl_brightness_get();
        }
        if (S.dim_from > 0) {
            S.dim_step++;
            sysctl_brightness_set(S.dim_from - S.dim_from / 2 * S.dim_step / DIM_STEPS);
        } else {
            S.dim_from = -1;
            S.dim_step = DIM_STEPS; /* at the lowest level already, unknown, or the panel goes dark anyway */
        }
    }
    return S.phase != ON || (S.frame & 1) == 0;
}

/* smoothstep, 0..256 */
static int ease(int t, int n)
{
    if (t <= 0) return 0;
    if (t >= n) return 256;
    int x = (t << 8) / n;
    return (x * x * (768 - 2 * x)) >> 16;
}

void saver_draw(const SysStatus *st)
{
    if (S.phase == OFF)
        return;
    if (st->minute != S.last_min) { /* the date only changes on a new minute */
        ScePspDateTime t;
        sceRtcGetCurrentClockLocalTime(&t);
        S.year = t.year;
        S.month = t.month;
        S.day = t.day;
        S.wday = sceRtcGetDayOfWeek(t.year, t.month, t.day);
        S.last_min = st->minute;
    }

    SaverView v = {0};
    v.st = *st;
    v.wday = S.wday;
    v.day = S.day;
    v.month = S.month;
    v.year = S.year;
    v.h12 = system_h12();
    v.date_fmt = system_date_fmt();
    v.frame = S.frame;
    v.show = S.phase == IN ? ease(S.t, FADE_IN) : S.phase == OUT ? 256 - ease(S.t, FADE_OUT) : 256;
    v.leaving = S.phase == OUT;
    /* "Press any button" for a few seconds after it appears */
    v.hint = S.phase == IN ? v.show : 256 - ease(S.shown - HINT_FRAMES, 60);

    static char music[160];
    MusicNow now;
    if (music_state() != MUSIC_STOPPED && music_now(&now) == 0) {
        if (now.artist && now.artist[0])
            snprintf(music, sizeof(music), "%s" DOT "%s", now.title, now.artist);
        else
            snprintf(music, sizeof(music), "%s", now.title);
        v.music = music;
        v.music_paused = music_state() == MUSIC_PAUSED;
    }
    saverdraw(&v);
}
