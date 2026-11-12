/* Renders Settings > About on the PC: the real settings.c (gfx.c through the
 * software GE of saver_preview.c), with the free-space count stood in for, so
 * the dialog can be seen at its stages: counting, done, the driver's fallback,
 * stopped with ○, and About after. Checks the dialog closes and the rows update.
 * Writes PPM frames.
 *
 *   settings_preview OUTDIR [THEME] */
#define main saver_main
#include "saver_preview.c"
#undef main

#include "../src/gamecfg.h"
#include "../src/sysctl.h"
#include "../src/library.h"
#include "../src/bg.h"
#include "../src/sfx.h"

/* ------------------------------------------------------------ stand-ins */
GlobalCfg g_cfg;
void bg_flush(void) {}
void bg_post(void (*fn)(void *), const void *arg, int size) { (void)fn; (void)arg; (void)size; }
void globalcfg_save(void) {}
void history_clear(void) {}
int history_count(void) { return 0; }
int library_count(int v) { (void)v; return 0; }
static char s_note[160];
void library_note(const char *m) { snprintf(s_note, sizeof(s_note), "%s", m); printf("note: %s\n", m); }
void library_rescan(void) {}
void library_resort(void) {}
int library_scanning(void) { return 0; }
void saver_start(void) {}
void sfx_play(int id) { (void)id; }
void ui_toast(const char *m) { printf("toast: %s\n", m); }
void ui_library_changed(void) {}
void ui_open_removed(void) {}
void ui_draw_status_bar(const char *t, const SysStatus *st) { (void)st; gfx_text(FONT_SMALL, 12, 5, t, C_MUTED); }
void format_size(char *b, int n, uint64_t bytes)
{
    unsigned tenths = (unsigned)(bytes * 10 / (1024ull * 1024 * 1024));
    snprintf(b, n, "%u.%u GB", tenths / 10, tenths % 10);
}
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * 16;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? 12 : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, 12, 12, C_TEXT); x += 16; }
        else x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + 16;
    }
}
/* ARK, plugins, the registry: nothing there */
#include "../src/arkcfg.h"
#include "../src/plugins.h"
#include "../src/ark.h"
int ark_load_cfg(ArkCfg *c, char *e, int cap) { (void)c; (void)e; (void)cap; return -1; }
int ark_save_cfg(const ArkCfg *c, const char *e) { (void)c; (void)e; return -1; }
int ark_version(void) { return 4; }
void arkcfg_set(ArkCfg *c, int k, int v) { (void)c; (void)k; (void)v; }
int plugins_global(PlugLine *p, int max) { (void)p; (void)max; return 0; }
int plugins_global_set(const PlugLine *p, const char *r, int on) { (void)p; (void)r; (void)on; return -1; }
int psreg_get_int(const char *d, const char *k, int *v) { (void)d; (void)k; (void)v; return -1; }
int psreg_get_str(const char *d, const char *k, char *v, int n) { (void)d; (void)k; (void)v; (void)n; return -1; }
int psreg_set_int(const char *d, const char *k, int v) { (void)d; (void)k; (void)v; return -1; }
void system_read_formats(void) {}
void launcher_sync_now(void) {} /* arklauncher.c: the ARK folder isn't there on the PC */
int sysctl_impose_set(int p, int v) { (void)p; (void)v; return -1; }
void sysctl_firmware(char *b, int n) { snprintf(b, n, "6.61"); }
int sysctl_model(void) { return 10; }
const char *sysctl_model_name(void) { return "PSP-E1004 (Street)"; }

/* the count: driven by hand below */
static int s_cstate = COUNT_IDLE, s_cpm, s_raw_ok = 1, s_driver_calls, s_cancels;
static uint64_t s_free, s_total;
int sysctl_storage(const char *dev, uint64_t *f, uint64_t *t)
{
    if (dev[0] == 'e' || !s_total) return -1;
    *f = s_free; *t = s_total;
    return 0;
}
int sysctl_storage_date(void) { return s_total ? 20261112 : 0; }
int sysctl_storage_measure(void) { s_driver_calls++; s_free = 47ull << 30; s_total = 119ull << 30; return 12051; }
int sysctl_storage_count_start(void) { s_cstate = s_raw_ok ? COUNT_RUNNING : COUNT_FAILED; s_cpm = 0; return 0; }
int sysctl_storage_count_state(int *pm) { if (pm) *pm = s_cpm; return s_cstate; }
void sysctl_storage_count_cancel(void) { s_cancels++; s_cstate = COUNT_CANCELLED; }
const char *sysctl_storage_count_how(int *ms) { if (ms) *ms = 1840; return "msstor0p1: (kernel)"; }

#include "../src/settings.c"

/* ---------------------------------------------------------------- frames */
static SysStatus s_st = {21, 41, 76, 0, 1};
static const char *s_dir;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); puts(""); fails++; } } while (0)

static void frames(int n) { for (int i = 0; i < n; i++) settings_step(); }
static void shot(const char *name)
{
    gfx_begin(g_th->bg);
    settings_draw(&s_st);
    gfx_end();
    save(s_dir, name);
}
static void go_about(void) /* from the sections column */
{
    if (S.col == 1) settings_input(PSP_CTRL_CIRCLE);
    for (int i = 0; i < C_COUNT && S.cat != C_ABOUT; i++) settings_input(PSP_CTRL_UP);
}

/* XLT_DEMO=DIR: Settings opening, the theme changed live, About counting; every other frame saved */
static int s_mf, s_tk;
static void dframe(const char *dir, unsigned b)
{
    if (b) settings_input(b);
    if (s_cstate == COUNT_RUNNING) { /* the count, as the PSP's took (~1.1 s) */
        s_cpm += 15;
        if (s_cpm >= 1000) { s_cpm = 1000; s_cstate = COUNT_DONE; s_free = 47ull << 30; s_total = 119ull << 30; }
    }
    settings_step();
    if (++s_tk & 1) return;
    gfx_begin(g_th->bg);
    settings_draw(&s_st);
    gfx_end();
    char name[16];
    snprintf(name, sizeof(name), "f%05d", s_mf++);
    save(dir, name);
}
static void dwait(const char *dir, unsigned b, int n) { dframe(dir, b); for (int i = 1; i < n; i++) dframe(dir, 0); }
static int demo(const char *dir)
{
    settings_open();
    dwait(dir, 0, 40);
    dwait(dir, PSP_CTRL_RIGHT, 30);                    /* into XLT: Theme */
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_RIGHT, 32); /* the themes, live */
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_LEFT, 8);   /* back to Night */
    dwait(dir, PSP_CTRL_CIRCLE, 16);
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_DOWN, 14);  /* the sections */
    dwait(dir, PSP_CTRL_UP, 10);
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_UP, 10);    /* ...round to About */
    dframe(dir, 0);
    if (S.cat != C_ABOUT) printf("settings demo: ended on section %d, not About\n", S.cat);
    dwait(dir, 0, 24);
    dwait(dir, PSP_CTRL_RIGHT, 110);                   /* in: the count, then the result */
    printf("settings demo: %d frames\n", s_mf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_dir = argv[1];
    if (getenv("XLT_DEMO")) {
        dm = gfx_dither_matrix;
        sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
        sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
        theme_set(0);
        return demo(getenv("XLT_DEMO"));
    }
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(argc > 2 ? atoi(argv[2]) : 0);

    settings_open();
    frames(40);
    go_about();                       /* ↑ from XLT wraps to About: only highlighted, nothing counted */
    frames(20);
    CHECK(s_space == SP_IDLE, "a count on only highlighting About");
    shot("set_0_highlighted");
    /* stopped: moving off About, then ○ (Settings stays open); neither counts as done */
    settings_input(PSP_CTRL_RIGHT);
    frames(10);
    CHECK(s_space == SP_COUNTING, "no count on entering About");
    settings_input(PSP_CTRL_RTRIGGER); /* L/R: another section */
    frames(30);
    CHECK(s_space == SP_IDLE && s_cancels == 1, "moving off didn't stop it (%d)", s_cancels);
    go_about();
    settings_input(PSP_CTRL_RIGHT);
    frames(10);
    settings_input(PSP_CTRL_CIRCLE);
    frames(30);
    CHECK(s_space == SP_IDLE && s_cancels == 2 && S.open, "○ didn't stop it, or closed Settings (state %d, cancels %d, open %d, cat %d col %d)", s_space, s_cancels, S.open, S.cat, S.col);
    /* counted to the end */
    CHECK(S.col == 0, "○ didn't go back to the sections");
    settings_input(PSP_CTRL_RIGHT);   /* in again: counts, as nothing was counted yet */
    frames(20);
    CHECK(s_space == SP_COUNTING, "no count after a stopped one");
    shot("set_1_counting_0");
    s_cpm = 420;
    frames(2);
    shot("set_2_counting_42");
    s_cstate = COUNT_DONE; s_cpm = 1000; s_free = 47ull << 30; s_total = 119ull << 30;
    frames(2);
    shot("set_3_done");
    CHECK(strstr(s_note, "counted in 1840 ms"), "no log line: %s", s_note);
    frames(60);
    CHECK(s_space == SP_IDLE && s_space_t.x == 0, "dialog stayed after done");
    shot("set_4_about_after");
    /* round the sections again and back in: no second count this time XLT runs */
    settings_input(PSP_CTRL_CIRCLE);
    for (int i = 0; i < C_COUNT; i++) { settings_input(PSP_CTRL_DOWN); frames(3); }
    settings_input(PSP_CTRL_RIGHT);
    frames(20);
    CHECK(S.cat == C_ABOUT && s_space == SP_IDLE, "counted again (cat %d, state %d)", S.cat, s_space);

    /* the raw stick can't be read: the driver, with the warning drawn first (a fresh start of XLT) */
    s_counted = 0;
    s_raw_ok = 0;
    settings_input(PSP_CTRL_CIRCLE);
    settings_input(PSP_CTRL_RIGHT);
    frames(1);
    CHECK(s_space == SP_DRIVER && s_driver_calls == 0, "driver fallback: state %d, calls %d", s_space, s_driver_calls);
    frames(2);
    shot("set_5_driver_note");
    frames(30);
    CHECK(s_driver_calls == 1 && s_space == SP_IDLE, "driver count: %d calls, state %d", s_driver_calls, s_space);
    settings_input(PSP_CTRL_CIRCLE);
    settings_input(PSP_CTRL_RIGHT);
    frames(5);
    CHECK(s_driver_calls == 1, "the driver counted again (%d)", s_driver_calls);

    puts(fails ? "settings_preview: FAILED" : "settings_preview: ok");
    return fails != 0;
}
