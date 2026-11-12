/* XLT - volume OSD.
 *
 * The volume buttons go straight to the firmware; XLT only sees the result
 * by polling sceImpose (sysctl_volume_get, a kernel call through kubridge).
 * A change pops up a pill under the status bar for HOLD_FRAMES. Changes made
 * by the control center's slider (osd_sync) and any change while the control
 * center is open are taken silently: the slider already shows them.
 */
#include <stdio.h>
#include "osd.h"
#include "ui_internal.h"
#include "anim.h"
#include "cc.h"
#include "sysctl.h"

#define POLL_FRAMES 2
#define HOLD_FRAMES 90 /* 1.5 s after the last change */
#define PILL_W 200
#define PILL_H 30
#define PILL_Y 34

static struct {
    int vol, muted;  /* last known; vol -1 = unknown */
    int avls;        /* the limiter is on: the level stops at AVLS_LEVEL */
    unsigned kbtn;   /* volume buttons last seen (kernel-only buttons) */
    int frames;
    int hold;        /* frames left fully shown */
    Spring t;        /* 0 hidden .. FX(1) shown */
    int bar;         /* drawn fill, Q8 0..256, eased toward the level */
} O;

void osd_init(void)
{
    O.vol = sysctl_volume_get(&O.muted);
    O.avls = sysctl_avls() > 0;
    spring_snap(&O.t, 0);
}

void osd_sync(void)
{
    int m = 0, v = sysctl_volume_get(&m);
    if (v >= 0) {
        O.vol = v;
        O.muted = m;
    }
}

int osd_step(void)
{
    int dirty = 0;
    if (++O.frames >= POLL_FRAMES) {
        O.frames = 0;
        int m = 0, v = sysctl_volume_get(&m), a = sysctl_avls() > 0;
        O.avls = a;
        /* Volume up at the AVLS limit changes nothing: say why instead */
        unsigned kb = O.avls ? sysctl_kernel_buttons() & PSP_CTRL_VOLUP : 0;
        if (kb && !O.kbtn && v >= AVLS_LEVEL && !m && !cc_is_open()) {
            ui_toast(AVLS_MESSAGE);
            dirty = 1;
        }
        O.kbtn = kb;
        if (v >= 0 && (v != O.vol || m != O.muted)) {
            int first = O.vol < 0;
            O.vol = v;
            O.muted = m;
            if (!first && !cc_is_open()) {
                O.hold = HOLD_FRAMES;
                spring_to(&O.t, FX(1));
                dirty = 1;
            }
        }
    }
    if (cc_is_open() && O.t.target) { /* the control center covers it */
        O.hold = 0;
        spring_to(&O.t, 0);
    }
    if (O.hold > 0 && --O.hold == 0)
        spring_to(&O.t, 0);
    int want = O.muted || O.vol < 0 ? 0 : O.vol * 256 / VOLUME_MAX;
    if (O.bar != want) {
        int d = want - O.bar;
        O.bar += d / 3 ? d / 3 : d; /* quick ease, ends exactly on the level */
        dirty |= O.t.x > 0;
    }
    int old = O.t.x;
    spring_step(&O.t);
    dirty |= O.t.x != old;
    return dirty;
}

void osd_draw(void)
{
    int t = O.t.x;
    if (t <= 0 || O.vol < 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    int x = (SCREEN_W - PILL_W) / 2, y = PILL_Y - 8 + (8 * t >> 8); /* drops in a little */
    gfx_rrect(x, y, PILL_W, PILL_H, PILL_H / 2, C_PILL);
    gfx_icon(O.muted ? ICON_MUTE : ICON_VOLUME, x + 14, y + (PILL_H - 16) / 2, 16, 16,
             O.muted ? C_MUTED : C_TEXT);
    const int bx = x + 40, bw = PILL_W - 40 - 60, by = y + PILL_H / 2 - 3;
    gfx_rrect(bx, by, bw, 6, 3, C_RAISED);
    int fill = bw * O.bar >> 8;
    if (fill >= 6)
        gfx_rrect(bx, by, fill, 6, 3, C_FOCUS);
    char num[8];
    snprintf(num, sizeof(num), "%d", O.vol);
    int fh = gfx_line_h(FONT_SMALL);
    gfx_text_right(FONT_SMALL, x + PILL_W - 16, y + (PILL_H - fh) / 2, O.muted ? "Muted" : num,
                   O.muted ? C_MUTED : C_TEXT);
    gfx_set_opacity(256);
}
