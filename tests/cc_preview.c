/* Renders the control center on the PC: the real cc.c (gfx.c through the
 * software GE of saver_preview.c), with the system calls stood in for, and
 * the Power tile growing into the power dialog frame by frame. Writes PPM frames.
 *
 *   cc_preview OUTDIR [THEME] */
#define main saver_main
#include "saver_preview.c"
#undef main

#include "../src/music.h"
#include "../src/library.h"
#include "../src/gamecfg.h"
#include "../src/sysctl.h"
#include "../src/bg.h"
#include "../src/sfx.h"
#include "../src/web.h"
#include "webicon_data.h"
#include "../src/thumbconv.h"

/* ------------------------------------------------------------ stand-ins */
GlobalCfg g_cfg;
static int s_video_open, s_music_started; /* a video is playing; anything that would start music */
int video_is_open(void) { return s_video_open; }
void bg_flush(void) {}
void bg_post(void (*fn)(void *), const void *arg, int size) { (void)fn; (void)arg; (void)size; }
int brightness_from_hw(int l) { return l; }
int brightness_to_hw(int p) { return p; }
int fm_busy(void) { return 0; }
void format_clock(char *b, int n, int h, int m) { snprintf(b, n, "%02d:%02d", h, m); }
void globalcfg_save(void) {}
static Entry s_demo_song; /* demo mode: the song playing has a library entry, so its cover is drawn */
static int s_demo_lib;
int library_find(int v, const char *p) { (void)v; (void)p; return s_demo_lib ? 0 : -1; }
const Entry *library_get(int v, int i) { (void)v; (void)i; return s_demo_lib ? &s_demo_song : NULL; }
void library_rescan(void) {}
void library_shutdown(void) {}
int music_can_resume(const char **t, const char **a) { (void)a; if (t) *t = "One Long Evening"; return s_video_open; }
void music_next(void) {}
void music_prev(void) {}
int music_resume(void) { s_music_started++; return -1; }
void music_seek(int d) { (void)d; }
void music_shutdown(void) {}
int music_sleep_cycle(void) { return 0; }
int music_sleep_left(void) { return 0; }
int music_state(void) { return s_video_open ? MUSIC_STOPPED : MUSIC_PLAYING; }
void music_toggle(void) {}
int music_now(MusicNow *o)
{
    o->title = "One Long Evening (Official Audio)"; o->artist = "Mira Ostend"; o->path = "ms0:/MUSIC/x.mp3";
    o->index = 2; o->count = 14; o->elapsed_ms = 83000; o->duration_ms = 233000;
    return 0;
}
void np_open(void) { s_music_started++; }
void reader_shutdown(void) {}
void sfx_play(int id) { (void)id; }
int sysctl_brightness_available(void) { return 1; }
int sysctl_brightness_hw(void) { return 60; }
int sysctl_brightness_set(int p) { (void)p; return 0; }
void sysctl_restart(void) {}
void sysctl_shutdown(void) {}
void sysctl_sleep(void) {}
int sysctl_usb_active(void) { return 0; }
int sysctl_usb_available(void) { return 1; }
int sysctl_usb_connected(void) { return 0; }
int sysctl_usb_start(void) { return -1; }
void sysctl_usb_stop(void) {}
static int s_vol = 18, s_avls;
int sysctl_volume_get(int *m) { if (m) *m = 0; return s_vol; }
int sysctl_avls(void) { return s_avls; }
static unsigned s_kbtn;
unsigned sysctl_kernel_buttons(void) { return s_kbtn; }
int sysctl_volume_set(int v, int m) { (void)m; s_vol = v; return 0; }
int sysctl_volume_settable(void) { return 1; }
void thumbs_init(const char *d) { (void)d; }
void thumbs_shutdown(void) {}
void ui_dim(int t) { gfx_rect(0, 0, SCREEN_W, SCREEN_H, (u32)((150 * t) >> 8) << 24); }
static const void *s_demo_cover; /* demo mode: a made-up cover */
void ui_draw_cover(const Entry *e, int x, int y, int s, int r, u32 bg)
{
    (void)e; (void)bg;
    if (s_demo_cover) gfx_texture(s_demo_cover, GU_PSM_5551, 256, 128, 144, 1, 79, 79, x, y, s, s);
    else gfx_rrect(x, y, s, s, r, RGB(0x6e40c9));
}
void ui_draw_status_bar(const char *t, const SysStatus *st) { (void)t; (void)st; }
void ui_open_music(void) { s_music_started++; }
void ui_request_exit(void) {}
static const char *s_toast;
void ui_toast(const char *m) { s_toast = m; printf("toast: %s\n", m); }
void video_shutdown(void) {}
void rec_stop(void) {}
void web_draw_icon(int id, int x, int y, int w, int h, unsigned int c)
{
    const short *r = webicon_rects[id];
    gfx_bind(webicon_pixels, GU_PSM_4444, WEBICON_W, WEBICON_H, WEBICON_W, 1);
    gfx_sprite(x, y, x + w, y + h, r[0], r[1], r[0] + r[2], r[1] + r[2], c);
    gfx_bind_atlas();
}
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * 16;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? 12 : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, 12, 12, btn_tint(h->tint)); x += 16; }
        else x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + 16;
    }
}

#include "../src/cc.c" /* in this file: the USB screen's state is set by hand below */
#include "../src/osd.c" /* the volume pill */

/* ---------------------------------------------------------------- frames */
static SysStatus s_st = {21, 41, 76, 0, 1};
static const char *s_dir;

static void frames(int n) { for (int i = 0; i < n; i++) cc_step(); }
static void shot(const char *name)
{
    gfx_begin(g_th->bg);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, g_th->bg); /* stands in for the screen behind */
    gfx_text(FONT_BOLD, 20, 160, "The screen behind the control center", C_MUTED);
    cc_draw(&s_st);
    gfx_end();
    save(s_dir, name);
}

/* XLT_DEMO=DIR: the sheet sliding in over a screen (XLT_DEMO_BG, a PPM), a walk over it and the
 * volume going up, every other frame saved. XLT_DEMO_ART: the made-up cover (album_low_tide.t16). */
static float s_bg[SCREEN_H][SCREEN_W][3];
static int s_has_bg, s_mf, s_tk;
static void load_bg(const char *p)
{
    FILE *f = p ? fopen(p, "rb") : NULL;
    int w, h, m;
    if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &m) != 3 || w != SCREEN_W || h != SCREEN_H) { if (f) fclose(f); return; }
    fgetc(f);
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            for (int k = 0; k < 3; k++) s_bg[y][x][k] = fgetc(f) / 255.f;
    fclose(f);
    s_has_bg = 1;
}
static void dframe(const char *dir, unsigned b)
{
    if (b) cc_input(b);
    cc_step();
    osd_step();
    if (++s_tk & 1) return;
    gfx_begin(g_th->bg);
    if (s_has_bg) memcpy(fb, s_bg, sizeof(fb));
    cc_draw(&s_st);
    osd_draw();
    gfx_end();
    char name[16];
    snprintf(name, sizeof(name), "f%05d", s_mf++);
    save(dir, name);
}
static void dwait(const char *dir, unsigned b, int n) { dframe(dir, b); for (int i = 1; i < n; i++) dframe(dir, 0); }
static int demo(const char *dir)
{
    static uint8_t cover[THUMB_BYTES] __attribute__((aligned(16)));
    char p[512];
    const char *art = getenv("XLT_DEMO_ART");
    snprintf(p, sizeof(p), "%s/album_low_tide.t16", art ? art : ".");
    FILE *f = art ? fopen(p, "rb") : NULL;
    if (f) { if (fread(cover, 1, THUMB_BYTES, f) == THUMB_BYTES) s_demo_cover = cover; fclose(f); }
    load_bg(getenv("XLT_DEMO_BG"));
    snprintf(s_demo_song.title, sizeof(s_demo_song.title), "One Long Evening (Official Audio)");
    snprintf(s_demo_song.artist, sizeof(s_demo_song.artist), "Mira Ostend");
    snprintf(s_demo_song.album, sizeof(s_demo_song.album), "Low Tide");
    s_demo_song.kind = KIND_MUSIC;
    s_demo_lib = 1;
    g_cfg.brightness = -1;
    s_vol = 14;
    osd_init();
    dwait(dir, 0, 20);
    cc_toggle();
    dwait(dir, 0, 45);                                 /* sliding in */
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_RIGHT, 16); /* the tiles */
    for (int i = 0; i < 4; i++) dwait(dir, PSP_CTRL_LEFT, 8);
    dwait(dir, PSP_CTRL_DOWN, 30);                     /* the song playing */
    dwait(dir, PSP_CTRL_DOWN, 20);                     /* volume */
    for (int i = 0; i < 5; i++) dwait(dir, PSP_CTRL_RIGHT, 9);
    dwait(dir, PSP_CTRL_DOWN, 20);                     /* brightness */
    for (int i = 0; i < 3; i++) dwait(dir, PSP_CTRL_RIGHT, 9);
    dwait(dir, PSP_CTRL_CIRCLE, 40);                   /* away again */
    printf("cc demo: %d frames\n", s_mf);
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
    g_cfg.brightness = -1;
    cc_toggle();
    frames(60);
    for (int i = 0; i < 3; i++) cc_input(PSP_CTRL_RIGHT); /* USB -> Capture -> Sleep -> Power */
    frames(20);
    shot("cc_1_power_tile");
    cc_input(PSP_CTRL_CROSS);                 /* the tile grows into the dialog */
    frames(4);
    shot("cc_2_growing");
    frames(6);
    shot("cc_3_growing_more");
    frames(60);
    shot("cc_4_open_cancel");
    for (int i = 0; i < 3; i++) cc_input(PSP_CTRL_LEFT);
    frames(60);
    shot("cc_5_shut_down");
    cc_input(PSP_CTRL_CIRCLE);                /* back into the tile */
    frames(6);
    shot("cc_6_shrinking");
    frames(60);
    shot("cc_7_closed_again");
    /* the USB mode screen, connected (its big symbol is drawn at its own size) */
    C.open = 0;
    spring_snap(&C.t, 0);
    spring_snap(&C.usb_t, FX(1));
    C.usb_connected = 1;
    gfx_begin(g_th->bg);
    cc_usb_draw(&s_st);
    gfx_end();
    save(s_dir, "cc_8_usb_connected");
    /* a video is playing: the music card explains itself and starts nothing */
    C.usb_connected = 0;
    spring_snap(&C.usb_t, 0);
    s_video_open = 1;
    cc_toggle();
    frames(60);
    C.row = ROW_MUSIC;
    frames(20);
    shot("cc_9_music_during_video");
    cc_input(PSP_CTRL_CROSS);
    cc_input(PSP_CTRL_TRIANGLE);
    if (s_music_started) { puts("cc_preview: music started during a video"); return 1; }
    /* AVLS on at 20: the slider looks as always, → stays at 20 and says why */
    s_video_open = 0;
    if (cc_is_open()) cc_toggle();
    frames(60);
    s_avls = 1;
    s_vol = 20;
    cc_toggle();
    frames(60);
    C.row = ROW_VOLUME;
    frames(20);
    shot("cc_10_avls_at_limit");
    s_toast = NULL;
    cc_input(PSP_CTRL_RIGHT);
    if (s_vol != 20 || !s_toast || strcmp(s_toast, AVLS_MESSAGE)) { puts("cc_preview: slider went past AVLS"); return 1; }
    cc_toggle();
    frames(60);
    /* ...and volume up on the button there: only the note, no pill */
    osd_init();
    s_toast = NULL;
    for (int i = 0; i < 30; i++) osd_step();
    s_kbtn = PSP_CTRL_VOLUP;
    for (int i = 0; i < 30; i++) osd_step();
    s_kbtn = 0;
    if (!s_toast || O.t.x) { puts("cc_preview: volume up at AVLS: no note, or the pill showed"); return 1; }
    puts("cc_preview: ok");
    return 0;
}
