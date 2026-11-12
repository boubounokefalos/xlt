/* Renders the launcher's lists and sheets on the PC: the real ui.c, opts.c and
 * menu.c (with gfx.c through the software GE of saver_preview.c), driven by
 * button presses like on the PSP, over a made-up library of games, songs and
 * playlists. Everything else is stood in for by tests/host/ui_stubs.c.
 * Writes PPM frames; look at them before a hardware test.
 *
 *   ui_preview OUTDIR */
#define main saver_main
#define ui_draw_battery saver_draw_battery /* ui.c has the real one */
extern int g_rec_preview;
#include "../src/safestart.h"
#include "saver_preview.c"
#undef ui_draw_battery
#undef main

#include "../src/ui.h"
#include "../src/library.h"
#include "../src/music.h"
#include "../src/playlists.h"
#include "../src/gamecfg.h"
#include "../src/history.h"
#include "../src/bg.h"
#include "../src/sfx.h"
#include "../src/thumbs.h"
#include "../src/imgdecode.h"

/* ------------------------------------------------------------ the library */

static Entry s_games[9], s_songs[12];
static const int NG = 9, NS = 12;

static void add(Entry *e, int kind, const char *path, const char *title, const char *artist, const char *album, int track)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->artist, sizeof(e->artist), "%s", artist);
    snprintf(e->album, sizeof(e->album), "%s", album);
    e->track = (uint16_t)track;
    e->kind = (uint8_t)kind;
    e->size = 1234567890ull / (1 + track);
    e->key = (uint32_t)track * 2654435761u;
}

static void make_library(void)
{
    /* all made up: these frames also become the XMB background and preview (tools/make_xmb_media.sh) */
    static const char *games[] = {"Astral Drift", "Bramble", "Citadel of Echoes: The Long Night", "Glowline",
                                  "Hollow Peaks Expedition Unlimited", "Kite Racer 2", "Moonlit Arcade",
                                  "Paper Racers 2", "Quiet Orbit"};
    char p[64];
    for (int i = 0; i < NG; i++) {
        snprintf(p, sizeof(p), "ms0:/ISO/game%d.iso", i);
        add(&s_games[i], KIND_ISO, p, games[i], "", "", i + 1);
        snprintf(s_games[i].id, sizeof(s_games[i].id), "ULUS1%04d", 100 + (i & 15));
    }
    static const char *songs[][3] = {
        {"Afternoon Light", "June Harbor", "Small Hours"}, {"\xce\xa4\xce\xb1\xce\xbe\xce\xaf\xce\xb4\xce\xb9 (Journey)", "Some Band", "First Album"},
        {"One Long Evening (Official Audio)", "Mira Ostend", "Low Tide"}, {"Glass Garden", "Mira Ostend", "Low Tide"},
        {"Northbound (Lyrics/Letra)", "Rio Vale", ""}, {"Static on the Radio", "Tallow & Finch", "Last Stop"},
        {"Song for Later", "Pale Mint", "Pale Mint"}, {"Slow Satellites", "Ocean Fold", "Undertow"}, {"Lanterns", "Ocean Fold", "Undertow"},
        {"voice memo 0412", "", ""}, {"Clockwork Summer", "Field Static", "Blue Hours"}, {"Weather Report", "Field Static", "Blue Hours"}};
    for (int i = 0; i < NS; i++) {
        snprintf(p, sizeof(p), "ms0:/MUSIC/song%02d.mp3", i);
        add(&s_songs[i], KIND_MUSIC, p, songs[i][0], songs[i][1], songs[i][2], i % 3 + 1);
    }
}

/* only in demo mode (XLT_DEMO): a few made-up videos and pictures, so those cards aren't empty */
static Entry s_videos[4], s_pics[6];
static int NV, NP;
static void make_demo_media(void)
{
    static const char *v[] = {"Harbour at Dusk", "Mountain Trail", "City Lights Timelapse", "Garden Rain"};
    static const char *len[] = {"3:12", "12:40", "1:05", "6:58"};
    static const char *pics[] = {"Sunset Dunes", "Blue Hour Lake", "Forest Path", "Desert Moon", "Neon Street", "Spring Field"};
    char p[64];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "ms0:/VIDEO/video%d.mp4", i);
        add(&s_videos[i], KIND_VIDEO, p, v[i], "", "", i + 1);
        snprintf(s_videos[i].id, sizeof(s_videos[i].id), "480x272");
        snprintf(s_videos[i].sysver, sizeof(s_videos[i].sysver), "%s", len[i]);
    }
    for (int i = 0; i < 6; i++) {
        snprintf(p, sizeof(p), "ms0:/PICTURE/pic%d.jpg", i);
        add(&s_pics[i], KIND_PICTURE, p, pics[i], "", "", i + 1);
        s_pics[i].icon_size = 1;
    }
    NV = 4;
    NP = 6;
}

static Entry *view(int v, int *n)
{
    *n = v == VIEW_GAMES ? NG : v == VIEW_MUSIC ? NS : v == VIEW_VIDEO ? NV : v == VIEW_PICTURES ? NP : 0;
    return v == VIEW_GAMES ? s_games : v == VIEW_VIDEO ? s_videos : v == VIEW_PICTURES ? s_pics : s_songs;
}
int library_count(int v) { int n; view(v, &n); return n; }
const Entry *library_get(int v, int i) { int n; return &view(v, &n)[i]; }
int library_find(int v, const char *path)
{
    int n;
    Entry *e = view(v, &n);
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].path, path)) return i;
    return -1;
}
int library_recent_count(int v) { int n = library_count(v) - 2; return n < 0 ? 0 : n < 3 ? n : 3; } /* items 2.. */
const Entry *library_recent(int v, int i) { return library_get(v, i + 2); }
int library_poll(void) { return 0; }
void library_resort(void) {}
int library_scanning(void) { return 0; }
const char *library_self(void) { return ""; }
void library_forget(const char *path) { (void)path; }

/* ------------------------------------------------------------ playlists */

static int s_open = -1;
static int s_order[4] = {0, 1, 2, 3}; /* the open playlist's order, as moves change it */
static const char *s_pl_names[] = {"Late night", "Road trip"};
int pl_count(void) { return 2; }
const char *pl_name(int i) { return s_pl_names[i]; }
const char *pl_path(int i) { static char p[64]; snprintf(p, sizeof(p), "ms0:/MUSIC/PLAYLISTS/%s.m3u8", s_pl_names[i]); return p; }
int pl_songs(int i) { return i ? 0 : 4; }
int pl_open(int i) { s_open = i; return 0; }
void pl_close(void) { s_open = -1; }
int pl_open_index(void) { return s_open; }
int pl_item_count(void) { return s_open == 0 ? 4 : 0; }
const Entry *pl_item(int k) { return k >= 0 && k < pl_item_count() ? &s_songs[s_order[k] * 3 % NS] : NULL; }
void pl_play(int k) { (void)k; }
int pl_remove_item(int k) { (void)k; return 0; }
int pl_move_item(int k, int dir)
{
    int j = k + dir;
    if (k < 0 || k >= 4 || j < 0 || j >= 4) return -1;
    int t = s_order[k]; s_order[k] = s_order[j]; s_order[j] = t;
    return j;
}
int pl_save_open(void) { return 0; }
void pl_refresh(void) {}
int pl_poll(int *err) { if (err) *err = 0; return 0; }
int pl_add(int i, const Entry *e) { (void)i; (void)e; return 0; }
int pl_add_many(int i, const Entry *l, int n, int *added) { (void)i; (void)l; *added = n; return 0; }
int pl_create(const char *n, const Entry *e) { (void)n; (void)e; return 0; }
int pl_rename(int i, const char *n) { (void)n; return i; }
void pl_delete(int i) { (void)i; }

/* ------------------------------------------------------------ the player */

int music_state(void) { return MUSIC_PLAYING; }
int music_generation(void) { return 0; }
int music_now(MusicNow *o)
{
    const Entry *e = &s_songs[2];
    o->title = e->title; o->artist = e->artist; o->path = e->path;
    o->index = 2; o->count = NS; o->elapsed_ms = 83000; o->duration_ms = 233000;
    return 0;
}
int music_can_resume(const char **t, const char **a) { (void)t; (void)a; return 0; }
int music_resume(void) { return -1; }
void music_play_view(int i) { (void)i; }
void music_play_entries(const Entry *l, int n, int i) { (void)l; (void)n; (void)i; }
void music_set_shuffle(int on) { (void)on; }
int music_shuffle(void) { return 0; }
unsigned int sceKernelGetSystemTimeLow(void) { return 12345; }
void music_poll(void) {}
void music_toggle(void) {}

/* ------------------------------------------------------------ the rest */

GlobalCfg g_cfg;
int favorite_get(const char *path) { return !strcmp(path, s_games[4].path) || !strcmp(path, s_songs[7].path); }
void favorite_set(const char *path, int on) { (void)path; (void)on; }
int hidden_get(const char *path) { (void)path; return 0; }
int hidden_set(const char *path, int on) { (void)path; (void)on; return 0; }
void history_describe(const char *path, char *out, int n) { (void)path; snprintf(out, n, "Played yesterday"); }
void bg_post(void (*fn)(void *), const void *arg, int size) { (void)size; fn((void *)arg); } /* at once */
/* demo mode (XLT_DEMO): made-up artwork from tests/make_demo_art.py as the thumbnails */
#include "../src/thumbconv.h"
static int s_art;
static uint8_t s_game_tex[9][THUMB_BYTES] __attribute__((aligned(16)));
static uint8_t s_album_tex[8][THUMB_BYTES] __attribute__((aligned(16)));
static uint8_t s_video_tex[4][THUMB_BYTES] __attribute__((aligned(16)));
static uint8_t s_pic_tex[6][THUMB_BYTES] __attribute__((aligned(16)));
static const char *const s_albums[8] = {"Small Hours", "First Album", "Low Tide", "Last Stop", "Pale Mint",
                                        "Undertow", "Blue Hours", NULL};
static void load_art(const char *dir)
{
    char p[512];
    for (int i = 0; i < 9; i++) {
        snprintf(p, sizeof(p), "%s/game_%d.png", dir, i);
        if (thumb_from_picture(p, s_game_tex[i]) != 0) { printf("no art: %s\n", p); return; }
    }
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s/video_%d.png", dir, i);
        if (thumb_from_picture(p, s_video_tex[i]) != 0) { printf("no art: %s\n", p); return; }
    }
    for (int i = 0; i < 6; i++) {
        snprintf(p, sizeof(p), "%s/pic_%d.png", dir, i);
        if (thumb_from_picture(p, s_pic_tex[i]) != 0) { printf("no art: %s\n", p); return; }
    }
    for (int i = 0; s_albums[i]; i++) {
        char slug[64];
        int k = 0;
        for (const char *s = s_albums[i]; *s && k < 63; s++)
            slug[k++] = (*s >= 'A' && *s <= 'Z') ? *s + 32 : ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) ? *s : '_';
        slug[k] = 0;
        snprintf(p, sizeof(p), "%s/album_%s.png", dir, slug);
        if (thumb_from_picture(p, s_album_tex[i]) != 0) { printf("no art: %s\n", p); return; }
        snprintf(p, sizeof(p), "%s/album_%s.t16", dir, slug); /* for the other demos, which have no decoder */
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(s_album_tex[i], 1, THUMB_BYTES, f); fclose(f); }
    }
    s_art = 1;
}
const void *thumbs_get(const Entry *e)
{
    if (!s_art || !e) return NULL;
    if (e->kind == KIND_ISO) {
        const char *d = strstr(e->path, "game");
        int i = d ? d[4] - '0' : -1;
        return i >= 0 && i < 9 ? s_game_tex[i] : NULL;
    }
    if (e->kind == KIND_VIDEO || e->kind == KIND_PICTURE) {
        const char *d = strrchr(e->path, '/');
        int i = d ? d[strlen(d) - 5] - '0' : -1;
        if (e->kind == KIND_VIDEO) return i >= 0 && i < 4 ? s_video_tex[i] : NULL;
        return i >= 0 && i < 6 ? s_pic_tex[i] : NULL;
    }
    for (int i = 0; s_albums[i]; i++)
        if (e->album[0] && !strcmp(e->album, s_albums[i])) return s_album_tex[i];
    return NULL;
}
/* img_format_name: the real one, from imgdecode.c (linked for the demo artwork) */
void format_clock(char *b, int n, int h, int m) { snprintf(b, n, "%02d:%02d", h, m); }
void sfx_play(int id) { (void)id; }
void sfx_commit(void) {}

/* ---------------------------------------------------------------- frames */

static SysStatus s_st = {21, 41, 76, 0, 1};
static const char *s_dir;
static Input s_in;

/* demo mode: every other frame (30 fps) saved as DIR/fNNNNN.ppm */
static const char *s_movie;
static int s_tick, s_mframe;
static void step(unsigned int b)
{
    s_in.pressed = b;
    ui_update(&s_in, 0);
    if (s_movie && (++s_tick & 1) == 0) {
        char name[16];
        gfx_begin(g_th->bg);
        ui_draw(&s_st);
        gfx_end();
        snprintf(name, sizeof(name), "f%05d", s_mframe++);
        save(s_movie, name);
    }
}

static void frames(int n)
{
    for (int i = 0; i < n; i++)
        step(0);
}

static void press(unsigned int b)
{
    step(b);
    frames(45); /* the springs settle */
}

/* a press, then n frames to watch it */
static void tap(unsigned int b, int n)
{
    step(b);
    frames(n);
}

/* The demo reel's walk through the lists: XLT_DEMO=FRAMES_DIR, XLT_DEMO_ART=ART_DIR. */
static int demo(void)
{
    frames(70);                                        /* home, on Games, the recent row */
    tap(PSP_CTRL_RIGHT, 22);
    tap(PSP_CTRL_RIGHT, 22);
    for (int i = 0; i < 4; i++) tap(PSP_CTRL_RTRIGGER, 24); /* the categories */
    for (int i = 0; i < 4; i++) tap(PSP_CTRL_LTRIGGER, 14);
    tap(PSP_CTRL_RIGHT, 14);                           /* All */
    tap(PSP_CTRL_CROSS, 45);                           /* the games list */
    for (int i = 0; i < 4; i++) tap(PSP_CTRL_DOWN, 20);
    tap(PSP_CTRL_RTRIGGER, 50);                        /* as a grid */
    tap(PSP_CTRL_RIGHT, 18);
    tap(PSP_CTRL_DOWN, 24);
    tap(PSP_CTRL_TRIANGLE, 75);                        /* the options sheet */
    tap(PSP_CTRL_DOWN, 22);
    tap(PSP_CTRL_DOWN, 22);
    tap(PSP_CTRL_CIRCLE, 26);
    tap(PSP_CTRL_LTRIGGER, 20);                        /* back to the list */
    tap(PSP_CTRL_CIRCLE, 40);                          /* home */
    tap(PSP_CTRL_RTRIGGER, 40);                        /* Music */
    for (int i = 0; i < 3; i++) tap(PSP_CTRL_RIGHT, 10);
    tap(PSP_CTRL_CROSS, 40);                           /* the Music list */
    tap(PSP_CTRL_DOWN, 22);
    tap(PSP_CTRL_CROSS, 40);                           /* Albums */
    tap(PSP_CTRL_RTRIGGER, 50);                        /* the covers */
    tap(PSP_CTRL_RIGHT, 18);
    tap(PSP_CTRL_RIGHT, 18);
    tap(PSP_CTRL_CROSS, 45);                           /* one album */
    tap(PSP_CTRL_CROSS, 55);                           /* play */
    tap(PSP_CTRL_CIRCLE, 24);
    tap(PSP_CTRL_CIRCLE, 24);
    tap(PSP_CTRL_CIRCLE, 50);                          /* home, music playing */
    printf("ui demo: %d frames\n", s_mframe);
    return 0;
}

static void shot(const char *name)
{
    gfx_begin(g_th->bg);
    ui_draw(&s_st);
    gfx_end();
    save(s_dir, name);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_dir = argv[1];
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(argc > 2 ? atoi(argv[2]) : 0); /* a theme by number, to look at each one */
    make_library();
    if (getenv("XLT_DEMO_ART")) load_art(getenv("XLT_DEMO_ART"));
    if (getenv("XLT_DEMO")) make_demo_media();
    s_movie = getenv("XLT_DEMO");

    ui_init();
    if (s_movie) return demo();
    frames(60);
    press(PSP_CTRL_RTRIGGER);                 /* Games -> Music */
    shot("ui_01_home_music");
    g_safe_start = SAFE_HELD; /* the status bar says so */
    shot("ui_01a_safe_start");
    g_safe_start = SAFE_NO;
    g_rec_preview = 1; /* recording: the light and its time */
    shot("ui_01b_recording");
    g_rec_preview = 0;
    for (int i = 0; i < 3; i++) press(PSP_CTRL_RIGHT);
    press(PSP_CTRL_CROSS);                    /* All: the Music list: Artists, Albums, the playlists, the songs */
    shot("ui_02_music_list");
    press(PSP_CTRL_CROSS);                    /* Artists */
    shot("ui_02a_artists");
    for (int i = 0; i < 3; i++) press(PSP_CTRL_DOWN);
    shot("ui_02b_artist_focused");
    press(PSP_CTRL_CROSS);                    /* one artist's songs */
    shot("ui_02c_artist_songs");
    press(PSP_CTRL_CIRCLE);
    shot("ui_02d_back_on_artist");
    press(PSP_CTRL_CIRCLE);                   /* back on the Artists row */
    press(PSP_CTRL_DOWN);
    press(PSP_CTRL_CROSS);                    /* Albums */
    press(PSP_CTRL_DOWN);
    shot("ui_02e_albums");
    press(PSP_CTRL_RTRIGGER);                 /* as a grid of covers */
    shot("ui_02f_albums_grid");
    press(PSP_CTRL_RTRIGGER);
    press(PSP_CTRL_TRIANGLE);                 /* an album's menu */
    shot("ui_02f2_album_menu");
    for (int i = 0; i < 3; i++) press(PSP_CTRL_DOWN);
    press(PSP_CTRL_CROSS);                    /* Add to playlist... */
    shot("ui_02f3_album_add_to_playlist");
    press(PSP_CTRL_CROSS);                    /* into the first playlist */
    shot("ui_02f4_album_added");
    press(PSP_CTRL_CROSS);                    /* one album */
    shot("ui_02g_album_songs");
    press(PSP_CTRL_TRIANGLE);                 /* a song's options, inside the album */
    shot("ui_02g2_album_song_sheet");
    press(PSP_CTRL_CIRCLE);
    press(PSP_CTRL_CROSS);                    /* play it */
    press(PSP_CTRL_CIRCLE);
    press(PSP_CTRL_CIRCLE);                   /* back on the Albums row */
    shot("ui_02h_back_on_albums");
    press(PSP_CTRL_UP);
    for (int i = 0; i < 5; i++) press(PSP_CTRL_DOWN); /* the song that's playing */
    shot("ui_03_music_playing_row");
    press(PSP_CTRL_TRIANGLE);                 /* its options */
    shot("ui_04_song_sheet");
    press(PSP_CTRL_CIRCLE);
    for (int i = 0; i < 3; i++) press(PSP_CTRL_UP);
    press(PSP_CTRL_TRIANGLE);                 /* a playlist's menu */
    shot("ui_05_playlist_menu");
    press(PSP_CTRL_CIRCLE);
    press(PSP_CTRL_CROSS);                    /* into it */
    shot("ui_06_in_playlist");
    press(PSP_CTRL_TRIANGLE);
    shot("ui_07_playlist_song_sheet");
    for (int i = 0; i < 4; i++) press(PSP_CTRL_DOWN); /* Move in playlist */
    press(PSP_CTRL_CROSS);
    press(PSP_CTRL_DOWN);                     /* carried down one */
    shot("ui_07b_moving");
    press(PSP_CTRL_CROSS);                    /* put down */
    press(PSP_CTRL_CIRCLE);                   /* out of the playlist */
    press(PSP_CTRL_RTRIGGER);                 /* the grid */
    shot("ui_08_music_grid");
    press(PSP_CTRL_RTRIGGER);
    press(PSP_CTRL_CIRCLE);                   /* home */
    for (int i = 0; i < 3; i++) press(PSP_CTRL_RTRIGGER); /* Music -> Video -> Pictures -> Files */
    shot("ui_08b_home_files");
    for (int i = 0; i < 3; i++) press(PSP_CTRL_LTRIGGER);
    press(PSP_CTRL_LTRIGGER);                 /* Games */
    for (int i = 0; i < 3; i++) press(PSP_CTRL_RIGHT);
    press(PSP_CTRL_CROSS);
    shot("ui_09_games_list");
    press(PSP_CTRL_TRIANGLE);
    shot("ui_10_game_sheet");
    puts("ui_preview: ok");
    return 0;
}
