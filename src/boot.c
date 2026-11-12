/* XLT - boot screen controller. See boot.h; the picture is bootdraw.c.
 *
 *   0 .. ~1.6 s     the logo's four cards pop in, left to right
 *   meanwhile       the library check (library.c's scan); once the logo is up the
 *                   bar fades in: files checked out of last time's count (a
 *                   moving segment on the first run, when there is no count)
 *   check done      the bar fills, a short pause, everything fades into the launcher
 *   any button      the same fade at once; the check goes on, with "Scanning..."
 *                   in the status bar
 * (Free space is not measured here: on a big card that stops the PSP for ~12 s.
 * Settings > About measures it on request.)
 * Meanwhile the home screen's first thumbnails are loaded, so the launcher
 * shows them as it appears. */
#include <pspkernel.h>
#include <stdio.h>
#include "boot.h"
#include "sfx.h"
#include "bootdraw.h"
#include "library.h"
#include "ui.h"

#define LEAVE_FRAMES 34
#define HOLD_FRAMES 24  /* the finished logo, before it fades */
#define HINT_AFTER 90 /* frames of bar before "Press any button to skip" */

static struct {
    int active, frame;
    int leaving, leave;  /* leave: 0..256 */
    int bar, bar_frames; /* bar: 0..256 shown */
    int prog;            /* shown progress, 0..256 (eases toward the real one) */
    int hold;
    int hint;
    int skipped, scanned;
    SceInt64 t0;
    char line[64];
} B;

static const char *const s_phase[LIB_PHASES] = {"games", "pictures", "music", "videos"};

void boot_init(void)
{
    B.active = 1;
    B.frame = 0;
    B.scanned = library_scanning();
    B.t0 = sceKernelGetSystemTimeWide();
}

int boot_active(void) { return B.active; }
int boot_covers(void) { return B.active && !B.leaving; }

static void leave(int skipped)
{
    if (B.leaving)
        return;
    B.leaving = 1;
    B.skipped = skipped;
}

int boot_update(unsigned int pressed)
{
    if (!B.active)
        return 0;
    B.frame++;
    for (int i = 0; i < 4; i++) /* each card's note, as it pops in (bootdraw.c's timing) */
        if (B.frame == 8 + i * 16 && !B.leaving)
            sfx_now(SFX_NOTE0 + i);
    ui_prefetch(); /* the launcher's first thumbnails, while nothing else needs the loader */
    int scanning = library_scanning();

    if (pressed && B.frame > 8)
        leave(1);

    /* the bar, once the logo is up */
    if (scanning && B.frame >= BOOT_INTRO_FRAMES - 10 && !B.leaving) {
        B.bar += 16;
        if (B.bar > 256) B.bar = 256;
    }
    if (B.bar > 0) {
        B.bar_frames++;
        int done, total, target;
        library_progress(&done, &total);
        if (!scanning)
            target = 256;
        else if (total > 0)
            target = done >= total ? 240 : done * 240 / total; /* the last bit is for when it really ends */
        else
            target = 0;
        B.prog += (target - B.prog + 3) / 4;
        if (target == 256 && B.prog > 250) B.prog = 256;
        if (B.bar_frames > HINT_AFTER && scanning && B.hint < 256)
            B.hint += 12;
    }
    if (!scanning && B.hint > 0)
        B.hint -= 24; /* nothing left to skip: the hint goes */

    if (!B.leaving && !scanning && (B.bar == 0 || B.prog >= 256) && B.frame >= BOOT_INTRO_FRAMES + HOLD_FRAMES) {
        if (B.hold++ >= HOLD_FRAMES / 2)
            leave(0);
    }
    if (B.leaving) {
        B.leave += 256 / LEAVE_FRAMES + 1;
        if (B.leave >= 256) {
            B.active = 0;
            char msg[96];
            snprintf(msg, sizeof(msg), "boot screen %d ms: %s", (int)((sceKernelGetSystemTimeWide() - B.t0) / 1000),
                     B.skipped ? "skipped with a button" : B.scanned ? "library checked" : "no check needed");
            library_note(msg);
        }
    }
    return 1;
}

void boot_draw(void)
{
    if (!B.active)
        return;
    BootView v = {0};
    v.frame = B.frame;
    v.leave = B.leave > 256 ? 256 : B.leave;
    v.bar_show = B.bar;
    v.hint = B.hint > 256 ? 256 : B.hint;
    v.progress = B.bar > 0 ? B.prog : -1;
    if (B.bar > 0) {
        int done, total, ph = library_progress(&done, &total);
        v.indefinite = total == 0 && library_scanning();
        if (!library_scanning())
            snprintf(B.line, sizeof(B.line), "Ready");
        else if (total > 0)
            snprintf(B.line, sizeof(B.line), "Checking %s" " \xC2\xB7 " "%d / %d", s_phase[ph], done, done > total ? done : total);
        else
            snprintf(B.line, sizeof(B.line), "Looking for %s" " \xC2\xB7 " "%d found", s_phase[ph], done);
        v.line = B.line;
    }
    bootdraw(&v);
}
