/* Renders the Now Playing screen on the PC: the real nowplaying.c and gfx.c
 * through the software GE of saver_preview.c, with the player and library
 * stood in for. Writes PPM frames (playing, paused, carry on, nothing).
 *
 *   np_preview OUTDIR */
#define main saver_main
#include "saver_preview.c"
#undef main

#include "../src/music.h"
#include "../src/resume.h"
#include "../src/library.h"
#include "../src/bg.h"
#include "../src/imgdecode.h"

/* ------------------------------------------------------------ stand-ins */
static int s_state = MUSIC_PLAYING, s_shuffle = 1, s_repeat = REPEAT_ALL, s_gen, s_resume = 1;
static Entry s_song;

int music_state(void) { return s_state; }
int music_generation(void) { return s_gen; }
int music_now(MusicNow *o)
{
    if (s_state == MUSIC_STOPPED) return -1;
    o->title = s_song.title; o->artist = s_song.artist; o->path = s_song.path;
    static int t;
    extern int g_np_demo_ms;
    o->index = 2; o->count = 14; o->elapsed_ms = g_np_demo_ms ? g_np_demo_ms : 83000; o->duration_ms = 241000;
    (void)t;
    return 0;
}
int music_can_resume(const char **t, const char **a)
{
    if (s_state != MUSIC_STOPPED || !s_resume) return 0;
    if (t) *t = "\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 (Good Morning)";
    if (a) *a = "Some Band";
    return 1;
}
int music_resume(void) { return 0; }
void music_toggle(void) {}
void music_next(void) {}
void music_prev(void) {}
void music_seek(int d) { (void)d; }
void music_set_shuffle(int on) { s_shuffle = on; }
int music_shuffle(void) { return s_shuffle; }
void music_set_repeat(int m) { s_repeat = m; }
int music_repeat(void) { return s_repeat; }
static int s_sleep;
int music_sleep_cycle(void) { s_sleep = s_sleep ? 0 : 30; return s_sleep; }
int music_sleep_left(void) { return s_sleep ? 29 : 0; }
int music_peek(int ahead, const char **t, const char **a)
{
    (void)ahead;
    *t = "The Next One With A Rather Long Title That Will Not Fit";
    *a = "Another Artist";
    return 0;
}
int library_find(int v, const char *p) { (void)v; (void)p; return 0; }
const Entry *library_get(int v, int i) { (void)v; (void)i; return &s_song; }
void bg_post(void (*fn)(void *), const void *arg, int size) { (void)fn; (void)arg; (void)size; }
int img_decode(const char *p, int m, int w, int h, uint8_t *o, int *rw, int *rh) { (void)p; (void)m; (void)w; (void)h; (void)o; (void)rw; (void)rh; return -1; }
int img_decode_at(const char *p, uint32_t off, int m, int w, int h, uint8_t *o, int *rw, int *rh) { (void)off; return img_decode(p, m, w, h, o, rw, rh); }
void sfx_play(int id) { (void)id; }
void ui_toast(const char *m) { printf("toast: %s\n", m); }
void ui_open_music(void) {}
int g_np_demo_ms; /* demo mode: the time playing */
static const void *s_demo_cover; /* demo mode: a made-up cover (tests/make_demo_art.py) */
void ui_draw_cover(const Entry *e, int x, int y, int s, int r, u32 bg)
{
    (void)e; (void)bg;
    if (s_demo_cover) {
        gfx_texture(s_demo_cover, GU_PSM_5551, 256, 128, 144, 1, 79, 79, x, y, s, s);
        return;
    }
    gfx_rrect(x, y, s, s, r, RGB(0x6e40c9)); /* a stand-in cover */
    gfx_rrect(x + s / 4, y + s / 4, s / 2, s / 2, s / 4, RGB(0xf0b72f));
}

#define HINT_GAP 16
#define HINT_ICON 12
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * HINT_GAP;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? HINT_ICON : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    if (total > SCREEN_W - 16) printf("hints too wide: %d px\n", total);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, HINT_ICON, HINT_ICON, btn_tint(h->tint)); x += HINT_ICON + 4; }
        else x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + HINT_GAP;
    }
}
void ui_draw_status_bar(const char *title, const SysStatus *st)
{
    gfx_text(FONT_SMALL, 12, 5, title, C_MUTED);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", st->hour, st->minute);
    int x = SCREEN_W - 12 - gfx_text_width(FONT_SMALL, b);
    gfx_text(FONT_SMALL, x, 5, b, C_TEXT);
    ui_draw_battery(x - 30, 9, st);
}

#include "../src/nowplaying.c"

static SysStatus s_st = {21, 41, 76, 0, 1};

static void shot(const char *dir, const char *name)
{
    for (int i = 0; i < 90; i++) np_step();
    gfx_begin(g_th->bg);
    np_draw(&s_st);
    gfx_end();
    save(dir, name);
}

#include "../src/thumbconv.h"
/* XLT_DEMO=DIR: ~4 s of a song playing, every other frame saved; XLT_DEMO_ART: its cover
 * (album_low_tide.t16, written by ui_preview's demo); the song ui_preview shows as playing */
static int demo(const char *dir)
{
    static uint8_t cover[THUMB_BYTES] __attribute__((aligned(16)));
    char p[512];
    const char *art = getenv("XLT_DEMO_ART");
    snprintf(p, sizeof(p), "%s/album_low_tide.t16", art ? art : ".");
    FILE *f = art ? fopen(p, "rb") : NULL;
    if (f) {
        if (fread(cover, 1, THUMB_BYTES, f) == THUMB_BYTES) s_demo_cover = cover;
        fclose(f);
    }
    snprintf(s_song.title, sizeof(s_song.title), "One Long Evening (Official Audio)");
    snprintf(s_song.artist, sizeof(s_song.artist), "Mira Ostend");
    snprintf(s_song.album, sizeof(s_song.album), "Low Tide");
    s_state = MUSIC_PLAYING;
    np_open();
    int n = 0;
    for (int f = 0; f < 260; f++) {
        g_np_demo_ms = 12000 + f * 1000 / 60;
        np_step();
        if (f & 1) continue;
        gfx_begin(g_th->bg);
        np_draw(&s_st);
        gfx_end();
        char name[16];
        snprintf(name, sizeof(name), "f%05d", n++);
        save(dir, name);
    }
    printf("np demo: %d frames\n", n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = argv[1];
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(argc > 2 ? atoi(argv[2]) : 0); /* a theme by number, to look at each one */
    snprintf(s_song.path, sizeof(s_song.path), "ms0:/MUSIC/Band/Album/03 Song.mp3");
    s_song.kind = KIND_MUSIC;
    if (getenv("XLT_DEMO")) return demo(getenv("XLT_DEMO"));
    snprintf(s_song.title, sizeof(s_song.title), "\xCE\xA4\xCE\xB1\xCE\xBE\xCE\xAF\xCE\xB4\xCE\xB9 (Journey) and a long tail");
    snprintf(s_song.artist, sizeof(s_song.artist), "Some Band");
    snprintf(s_song.album, sizeof(s_song.album), "First Album");
    s_song.track = 3;
    s_song.kind = KIND_MUSIC;

    np_open();
    shot(dir, "np_1_playing");
    s_state = MUSIC_PAUSED; s_shuffle = 0; s_repeat = REPEAT_ONE; s_gen++;
    shot(dir, "np_2_paused");
    /* a file named the YouTube way: the artist again in front, no album */
    snprintf(s_song.path, sizeof(s_song.path), "ms0:/MUSIC/x.mp3");
    snprintf(s_song.title, sizeof(s_song.title), "Some Band - One More Night, A Rather Long Title (Official Audio)");
    s_song.album[0] = 0;
    s_state = MUSIC_PLAYING; s_repeat = REPEAT_OFF; s_gen++;
    shot(dir, "np_2b_youtube");
    s_sleep = 30; s_repeat = REPEAT_ALL;      /* the sleep timer's pill */
    shot(dir, "np_2c_sleep");
    s_sleep = 0;
    { /* lyrics (the song is 1:23 in) */
        static const char lyr[] = "[01:20.00]The line that is sung right now, a long one that has to be cut\n"
                                  "[01:26.00]and the next one, fainter\n[01:30.00]\n";
        lrc_parse(&N.lyr, lyr, sizeof(lyr) - 1);
        shot(dir, "np_2d_lyrics_long_title");  /* one line: the title took two */
        snprintf(s_song.title, sizeof(s_song.title), "Short Title");
        snprintf(s_song.album, sizeof(s_song.album), "First Album");
        N.e = s_song; /* the shown copy (no song change: the lyrics stay) */
        shot(dir, "np_2e_lyrics");             /* two lines */
    }
    s_state = MUSIC_STOPPED; s_gen++;
    shot(dir, "np_3_resume");
    s_resume = 0;
    shot(dir, "np_4_nothing");
    puts("np_preview: ok");
    return 0;
}
