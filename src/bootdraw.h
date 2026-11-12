/* XLT - the boot screen's picture: the logo's four cards drawing in, then the
 * library check as a thin bar. Pure drawing (only gfx calls), so
 * tests/boot_preview.c can render it on the PC. */
#ifndef BOOTDRAW_H
#define BOOTDRAW_H

typedef struct {
    int frame;      /* 60 per second since XLT started */
    int progress;   /* 0..256 of the bar, -1 = no bar (nothing to check, or not yet) */
    int bar_show;   /* 0..256: the bar and its line fading in */
    int indefinite; /* first run: nothing to compare with, so a moving segment instead of a fill */
    const char *line; /* under the bar, e.g. "Checking games 120 / 340" */
    int hint;       /* 0..256: "Press any button to skip" */
    int leave;      /* 0..256: fading into the launcher */
} BootView;

/* Frames until the four cards have settled. */
#define BOOT_INTRO_FRAMES 96

void bootdraw(const BootView *v);

#endif
