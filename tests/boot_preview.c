/* Renders the boot screen on the PC: the real gfx.c and bootdraw.c through the
 * software GE of saver_preview.c. Writes PPM frames.
 *
 *   boot_preview OUTDIR   -> intro frames, the bar (filling, first run), leaving
 */
#define main saver_main
#include "saver_preview.c"
#undef main
#include "bootdraw.c"

static void shot(const char *dir, const char *name, const BootView *v)
{
    gfx_begin(RGB(0x0d1117));
    if (v->leave) { /* something to fade into */
        gfx_rrect(40, 60, 400, 120, 10, C_RAISED);
        gfx_text(FONT_BOLD, 60, 80, "(the launcher underneath)", C_TEXT);
    }
    bootdraw(v);
    gfx_end();
    save(dir, name);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    BootView v = {0};
    v.progress = -1;
    if (getenv("XLT_DEMO")) { /* the intro and the library check, every other frame saved */
        const char *d = getenv("XLT_DEMO");
        char nm[16];
        int n = 0;
        for (int f = 0; f < 170; f++) {
            v.frame = f;
            if (f >= 70) {
                int k = f - 70;
                v.bar_show = k * 256 / 20 > 256 ? 256 : k * 256 / 20;
                v.progress = k * 256 / 90 > 256 ? 256 : k * 256 / 90;
                static char line[48];
                int done = v.progress * 342 / 256;
                snprintf(line, sizeof(line), "Checking music \xC2\xB7 %d / 342", done);
                v.line = v.progress >= 256 ? "Ready" : line;
            }
            if (f & 1) continue;
            gfx_begin(RGB(0x0d1117));
            bootdraw(&v);
            gfx_end();
            snprintf(nm, sizeof(nm), "f%05d", n++);
            save(d, nm);
        }
        return 0;
    }
    char name[32];
    for (int f = 0; f <= 102; f += 6) { /* the shapes popping in */
        v.frame = f;
        snprintf(name, sizeof(name), "intro_%02d", f);
        shot(dir, name, &v);
    }
    v.frame = 120; v.bar_show = 256; v.progress = 150; v.line = "Checking music \xC2\xB7 210 / 342";
    shot(dir, "bar_filling", &v);
    v.hint = 256; v.progress = 230;
    shot(dir, "bar_hint", &v);
    v.hint = 0; v.indefinite = 1; v.progress = 0; v.line = "Looking for games \xC2\xB7 37 found";
    shot(dir, "first_run", &v);
    v.indefinite = 0; v.progress = 256; v.line = "Ready"; v.leave = 128;
    shot(dir, "leaving", &v);
    return 0;
}
