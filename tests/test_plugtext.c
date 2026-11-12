/* Host test for per-game PLUGINS.TXT handling (ARK loader rules). */
#include <stdio.h>
#include <string.h>
#include "../src/plugtext.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("OK   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static const PlugState *find(const PlugState *p, int n, const char *path)
{
    for (int i = 0; i < n; i++) if (!strcmp(p[i].path, path)) return &p[i];
    return NULL;
}

int main(void)
{
    const char *ark =
        "# ARK folder\r\n"
        "vsh, ms0:/SEPLUGINS/xmbctrl.prx, on\r\n"
        "game, ms0:/SEPLUGINS/cwcheat.prx, on\r\n";
    const char *sep =
        "always, ms0:/SEPLUGINS/remap.prx, on // trailing comment\n"
        "ULES00001, ms0:/SEPLUGINS/fps.prx, on\n"
        "pops, ms0:/SEPLUGINS/popsfix.prx, on\n"
        "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, off\n";
    const char *files[3] = {ark, sep, NULL};
    PlugState p[PLUG_MAX];

    int n = plugtext_list(files, 3, "ULUS10041", 0, p, PLUG_MAX);
    CHECK(n == 4, "4 game-relevant plugins (XMB-only xmbctrl left out)");
    const PlugState *cw = find(p, n, "ms0:/SEPLUGINS/cwcheat.prx");
    CHECK(cw && cw->global_on == 1, "cwcheat is on globally for games");
    CHECK(cw && cw->override == PLUG_OFF, "per-game off override read");
    CHECK(cw && cw->file == 1, "last file mentioning cwcheat is SEPLUGINS");
    const PlugState *fps = find(p, n, "ms0:/SEPLUGINS/fps.prx");
    CHECK(fps && fps->global_on == 0 && fps->override == PLUG_GLOBAL, "other game's plugin listed, off here");
    const PlugState *remap = find(p, n, "ms0:/SEPLUGINS/remap.prx");
    CHECK(remap && remap->global_on == 1, "'always' plugin on, trailing comment ignored");
    const PlugState *pf = find(p, n, "ms0:/SEPLUGINS/popsfix.prx");
    CHECK(pf && pf->global_on == 0, "pops-only plugin off for a PSP game");

    n = plugtext_list(files, 3, "SLUS00001", 1, p, PLUG_MAX);
    pf = find(p, n, "ms0:/SEPLUGINS/popsfix.prx");
    cw = find(p, n, "ms0:/SEPLUGINS/cwcheat.prx");
    CHECK(pf && pf->global_on == 1, "pops plugin on for a PS1 game");
    CHECK(cw && cw->global_on == 0, "'game' plugin off for a PS1 game");

    char out[2048];
    int len = plugtext_set(sep, "ULUS10041", "ms0:/SEPLUGINS/cwcheat.prx", PLUG_ON, out, sizeof(out));
    out[len > 0 ? len : 0] = 0;
    CHECK(len > 0 && strstr(out, "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, off") == NULL, "old override removed");
    CHECK(strstr(out, "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, on\n") != NULL, "new override written");
    CHECK(strstr(out, "always, ms0:/SEPLUGINS/remap.prx, on // trailing comment\n") != NULL, "other lines kept as-is");
    const char *last = strrchr(out, '\n');
    (void)last;
    CHECK(strlen(out) > 0 && !strcmp(out + strlen(out) - strlen("ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, on\n"),
                                     "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, on\n"), "override appended last");

    char back[2048];
    len = plugtext_set(out, "ULUS10041", "ms0:/SEPLUGINS/cwcheat.prx", PLUG_GLOBAL, back, sizeof(back));
    back[len > 0 ? len : 0] = 0;
    CHECK(len > 0 && strstr(back, "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx") == NULL, "set back to global removes the line");

    const char *files2[1] = {ark};
    char buf[2048];
    len = plugtext_set(ark, "ULUS10041", "ms0:/SEPLUGINS/cwcheat.prx", PLUG_OFF, buf, sizeof(buf));
    buf[len > 0 ? len : 0] = 0;
    files2[0] = buf;
    n = plugtext_list(files2, 1, "ULUS10041", 0, p, PLUG_MAX);
    cw = find(p, n, "ms0:/SEPLUGINS/cwcheat.prx");
    CHECK(cw && cw->global_on == 1 && cw->override == PLUG_OFF, "round trip: global on, overridden off");
    CHECK(strstr(buf, "# ARK folder\n") != NULL, "comment kept, CRLF normalised");

    /* global lines for the Settings tab */
    const char *g = "# header\r\n"
                    "always, ms0:/SEPLUGINS/leda.prx, on\r\n"
                    "vsh, ms0:/SEPLUGINS/vshmenu.prx, off # menu\n"
                    "ULUS10041, ms0:/SEPLUGINS/cwcheat.prx, on\n"
                    "ms0:/PSP/GAME/APP/EBOOT.PBP, ms0:/SEPLUGINS/x.prx, on\n"
                    "GAME, ms0:/SEPLUGINS/cwcheat.prx, on";
    PlugLine gl[8];
    int gn = plugtext_lines(g, 2, gl, 8);
    CHECK(gn == 3, "global lines only (no game ID or homebrew path lines)");
    CHECK(gn == 3 && gl[0].on && gl[0].line == 1 && gl[0].file == 2 && !strcmp(gl[0].runlevel, "always"),
          "line index, file and state");
    CHECK(gn == 3 && !gl[1].on && !strcmp(gl[1].runlevel, "vsh"), "off line with a trailing comment");
    CHECK(gn == 3 && !strcmp(gl[2].runlevel, "game") && gl[2].line == 5, "runlevel lower-cased, last line");
    char gout[1024];
    int glen = plugtext_set_line(g, 1, NULL, 0, gout, sizeof(gout));
    CHECK(glen > 0 && strstr(gout, "always, ms0:/SEPLUGINS/leda.prx, off\r\n") && strstr(gout, "# header\r\n"),
          "set off: only that field changes, CRLF kept");
    glen = plugtext_set_line(g, 5, NULL, 0, gout, sizeof(gout));
    CHECK(glen > 0 && strstr(gout, "GAME, ms0:/SEPLUGINS/cwcheat.prx, off") &&
          gout[glen - 1] == 'f', "last line without newline");
    glen = plugtext_set_line(g, 2, "game", 1, gout, sizeof(gout));
    CHECK(glen > 0 && strstr(gout, "\ngame, ms0:/SEPLUGINS/vshmenu.prx, on\n"), "runlevel changed, path kept, comment dropped");
    {
        PlugLine back[8];
        int bn = plugtext_lines(gout, 0, back, 8);
        CHECK(bn == 3 && back[1].on && !strcmp(back[1].runlevel, "game") &&
              !strcmp(back[1].path, "ms0:/SEPLUGINS/vshmenu.prx"), "changed line reads back");
    }
    CHECK(plugtext_set_line(g, 0, NULL, 1, gout, sizeof(gout)) == -1, "comment line refused");
    CHECK(plugtext_set_line(g, 40, NULL, 1, gout, sizeof(gout)) == -1, "missing line refused");

    /* the in-game screenshot plugin (src/shotplug.c) */
    {
        const char *t =
            "# mine\r\n"
            "game, ms0:/SEPLUGINS/ScreenShot.prx, on\r\n"
            "vsh, ms0:/SEPLUGINS/vshsnapshot.prx, on\r\n"
            "always, ef0:/seplugins/sshot_v2.prx, on\r\n"
            "pops, ms0:/SEPLUGINS/capture.prx, off\r\n"
            "game, ms0:/SEPLUGINS/cwcheat.prx, on\r\n"
            "ULUS10041, ms0:/SEPLUGINS/XLTSHOT.PRX, off\r\n";
        CHECK(plugtext_mentions(t, "xltshot.prx"), "xltshot found on a game-ID line, any case");
        CHECK(!plugtext_mentions("game, ms0:/SEPLUGINS/cwcheat.prx, on\n# xltshot.prx\n", "xltshot.prx"),
              "a comment isn't a mention");
        CHECK(plugtext_is_screenshot("ms0:/SEPLUGINS/ScreenShot.prx") && plugtext_is_screenshot("ms0:/x/PSPSHOT.PRX") &&
              plugtext_is_screenshot("ef0:/seplugins/sshot_v2.prx") && plugtext_is_screenshot("ms0:/s/ScreenCap.prx") &&
              plugtext_is_screenshot("ms0:/SEPLUGINS/prxshot-micropng.prx"),
              "screenshot plugins by name");
        CHECK(!plugtext_is_screenshot("ms0:/SEPLUGINS/xltshot.prx") && !plugtext_is_screenshot("ms0:/SEPLUGINS/cwcheat.prx") &&
              !plugtext_is_screenshot("ms0:/screenshots/remap.prx"), "not ours, not others, not by folder");
        char out[2048], names[256];
        int k = plugtext_off_screenshots(t, out, sizeof(out), names, sizeof(names));
        CHECK(k == 2 && !strcmp(names, "screenshot.prx, sshot_v2.prx"), "two game screenshot plugins turned off");
        CHECK(strstr(out, "game, ms0:/SEPLUGINS/ScreenShot.prx, off\r\n") && strstr(out, "always, ef0:/seplugins/sshot_v2.prx, off\r\n"),
              "their lines say off, path as written");
        CHECK(strstr(out, "vsh, ms0:/SEPLUGINS/vshsnapshot.prx, on\r\n") && strstr(out, "game, ms0:/SEPLUGINS/cwcheat.prx, on\r\n") &&
              strstr(out, "# mine\r\n"), "the XMB's one, other plugins and comments kept");
        CHECK(strlen(out) == strlen(t) + 2, "only the two states changed (on -> off: a letter longer each)");
        int n = plugtext_append(out, "game, ms0:/SEPLUGINS/xltshot.prx, on", out, sizeof(out));
        CHECK(n > 0 && strstr(out, "XLTSHOT.PRX, off\r\ngame, ms0:/SEPLUGINS/xltshot.prx, on\r\n"), "appended in CRLF");
        n = plugtext_append("vsh, a.prx, on", "game, b.prx, on", out, sizeof(out));
        CHECK(n > 0 && !strcmp(out, "vsh, a.prx, on\ngame, b.prx, on\n"), "appended after a last line without newline");
        n = plugtext_append("", "game, b.prx, on", out, sizeof(out));
        CHECK(n > 0 && !strcmp(out, "game, b.prx, on\n"), "appended to an empty file");
        CHECK(plugtext_append("vsh, a.prx, on\n", "game, b.prx, on", out, 10) == -1, "too small refused");
        CHECK(plugtext_off_screenshots("game, x.prx, on\n", out, sizeof(out), names, sizeof(names)) == 0, "nothing to turn off");
    }

    /* where ARK looks for each plugin's file (missing ones are hidden in Settings) */
    {
        char r[160];
        const char *ark = "ms0:/PSP/SAVEDATA/ARK_01234/";
        CHECK(plugtext_resolve("ms0:/SEPLUGINS/cxmb/cxmb.prx", ark, r, sizeof(r)) == 0 && !strcmp(r, "ms0:/SEPLUGINS/cxmb/cxmb.prx"),
              "a path with a device as it is");
        CHECK(plugtext_resolve("leda.prx", ark, r, sizeof(r)) == 0 && !strcmp(r, "ms0:/PSP/SAVEDATA/ARK_01234/leda.prx"),
              "a bare name in ARK's folder");
        CHECK(plugtext_resolve("/SEPLUGINS/x.prx", ark, r, sizeof(r)) == 0 && !strcmp(r, "ms0:/SEPLUGINS/x.prx"),
              "a rooted path on ms0:");
        CHECK(plugtext_resolve("leda.prx", "", r, sizeof(r)) == -1, "a bare name without ARK's folder: unknown");
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
