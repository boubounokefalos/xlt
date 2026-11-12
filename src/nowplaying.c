/* XLT - Now Playing. See nowplaying.h.
 *
 * Slides up over everything (like the picture viewer fades in) and is opaque
 * once it is up. The cover is drawn big: decoded at 144x144 on the worker
 * (the same art the library found: embedded in the MP3, or cover/folder.jpg
 * next to it) into one of two buffers, so the one on screen is never written
 * while the GE reads it. Until then the small library thumbnail stands in.
 *
 * Buttons as on the control center's music card: ✕ play/pause, ←/→ previous
 * and next song, L/R seek 10 s; △ shuffle, □ repeat, ○ back.
 */
#include <pspkernel.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "audiofmt.h"
#include "nowplaying.h"
#include "ui_internal.h"
#include "anim.h"
#include "sfx.h"
#include "bg.h"
#include "music.h"
#include "resume.h"
#include "library.h"
#include "imgdecode.h"
#include "lrc.h"
#include "id3.h"
#include <pspiofilemgr.h>
#include <stdlib.h>

#define COVER 144                 /* decoded size */
#define COVER_X 28
#define COVER_Y 42
#define COVER_S 156               /* drawn size */
#define COL_X (COVER_X + COVER_S + 24)
#define COL_W (SCREEN_W - 24 - COL_X)
#define NP_HINT_Y 253
#define BTN 36                    /* the round play / pause button */

static struct {
    int open;
    Spring t;           /* 0 closed .. FX(1) up */
    int gen;            /* music_generation() last seen */
    char path[200];     /* the song shown */
    Entry e;            /* its library entry (tags, cover), if it has one */
    int have_e;
    int shown_sec;
    int cover_want;     /* the cover job asked for this song (cover_gen) */
    Lrc lyr;            /* its timed lyrics, if it has any */
    int lyr_idx;        /* the line showing */
} N;

/* The big cover: two buffers, the worker fills the one not shown. */
static uint8_t s_cover[2][COVER * COVER * 4] __attribute__((aligned(64)));
static int s_cover_shown = -1;   /* buffer on screen, -1 = none (UI thread) */
static int s_cover_gen;          /* bumped for each new song (UI thread) */
static volatile int s_cover_done, s_cover_done_gen, s_cover_ok;

typedef struct {
    char path[200];
    uint32_t icon_off;
    int gen, buf;
} CoverArg;

static void job_cover(void *arg)
{
    const CoverArg *a = arg;
    int w, h, r = -1;
    uint8_t *out = s_cover[a->buf];
    if (a->icon_off == ICON_FOLDER_ART) { /* a picture next to the song, as the library found it */
        static const char *names[] = {"cover.jpg", "folder.jpg", "cover.png", "folder.png"};
        const char *slash = strrchr(a->path, '/');
        int n = slash ? (int)(slash - a->path) : 0;
        char p[300];
        for (int i = 0; i < 4 && r != 0; i++) {
            snprintf(p, sizeof(p), "%.*s/%s", n, a->path, names[i]);
            r = img_decode(p, IMG_COVER, COVER, COVER, out, &w, &h);
        }
    } else {
        r = img_decode_at(a->path, a->icon_off, IMG_COVER, COVER, COVER, out, &w, &h);
    }
    s_cover_ok = r == 0;
    s_cover_done_gen = a->gen;
    s_cover_done = 1;
}

/* Timed lyrics: a .lrc with the song's name next to it, else the ones in its tag. */
static void load_lyrics(const char *path)
{
    lrc_free(&N.lyr);
    N.lyr_idx = -2;
    if (!path[0])
        return;
    char p[220];
    const char *dot = strrchr(path, '.');
    snprintf(p, sizeof(p), "%.*s.lrc", dot ? (int)(dot - path) : (int)strlen(path), path);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
        sceIoLseek32(fd, 0, PSP_SEEK_SET);
        char *b = size > 0 && size < 256 * 1024 ? malloc(size) : NULL;
        int n = b ? sceIoRead(fd, b, size) : -1;
        sceIoClose(fd);
        if (n > 0) lrc_parse(&N.lyr, b, n);
        free(b);
    }
    if (!N.lyr.n) {
        char *b = malloc(16384);
        if (b && audio_lyrics(path, b, 16384) == 0)
            lrc_parse(&N.lyr, b, (int)strlen(b));
        free(b);
    }
}

/* The playing song changed (or the screen opened): its entry, and its cover asked for. */
static void follow_song(void)
{
    MusicNow now;
    const char *path = music_now(&now) == 0 ? now.path : "";
    if (!strcmp(path, N.path))
        return;
    snprintf(N.path, sizeof(N.path), "%s", path);
    load_lyrics(path);
    int i = path[0] ? library_find(VIEW_MUSIC, path) : -1;
    N.have_e = i >= 0;
    if (N.have_e)
        N.e = *library_get(VIEW_MUSIC, i);
    s_cover_shown = -1;
    s_cover_gen++;
    if (N.have_e && N.e.icon_size) {
        static CoverArg a;
        snprintf(a.path, sizeof(a.path), "%s", path);
        a.icon_off = N.e.icon_off;
        a.gen = s_cover_gen;
        a.buf = 0; /* nothing is shown now, so either buffer is free; 0 it is */
        bg_post(job_cover, &a, sizeof(a));
    }
}

void np_open(void)
{
    N.open = 1;
    N.path[0] = 0;
    N.gen = -1;
    spring_to(&N.t, FX(1));
}

int np_is_open(void) { return N.open || N.t.x > 0; }
int np_covers(void) { return N.open && N.t.x >= FX(1); }

static void close_np(void)
{
    N.open = 0;
    spring_to(&N.t, 0);
}

int np_input(unsigned int p)
{
    if (!N.open)
        return 0;
    int st = music_state();
    if (p & PSP_CTRL_CIRCLE) {
        close_np();
        return 1;
    }
    if (st == MUSIC_STOPPED) {
        if (p & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            if (music_can_resume(NULL, NULL)) {
                if (music_resume() != 0) ui_toast("The songs from last time aren't there any more");
            } else {
                close_np();
                ui_open_music();
            }
        }
    } else if (p & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        int m = music_sleep_cycle();
        char msg[48];
        snprintf(msg, sizeof(msg), m ? "Music stops in %d minutes" : "Sleep timer off", m);
        ui_toast(msg);
    } else if (p & (PSP_CTRL_CROSS | PSP_CTRL_START)) music_toggle();
    else if (p & PSP_CTRL_LEFT) music_prev();
    else if (p & PSP_CTRL_RIGHT) music_next();
    else if (p & PSP_CTRL_LTRIGGER) music_seek(-10000);
    else if (p & PSP_CTRL_RTRIGGER) music_seek(10000);
    if (p & PSP_CTRL_TRIANGLE) {
        music_set_shuffle(!music_shuffle());
        sfx_play(music_shuffle() ? SFX_ON : SFX_OFF);
        ui_toast(music_shuffle() ? "Shuffle on" : "Shuffle off");
    } else if (p & PSP_CTRL_SQUARE) {
        int r = (music_repeat() + 1) % REPEAT_COUNT;
        music_set_repeat(r);
        sfx_play(r ? SFX_ON : SFX_OFF);
        ui_toast(r == REPEAT_ALL ? "Repeat the list" : r == REPEAT_ONE ? "Repeat this song" : "Repeat off");
    }
    return 1;
}

int np_step(void)
{
    int old = N.t.x;
    spring_step(&N.t);
    int dirty = N.t.x != old;
    if (!np_is_open())
        return dirty;
    if (N.gen != music_generation()) {
        N.gen = music_generation();
        follow_song();
        dirty = 1;
    }
    if (s_cover_done) {
        s_cover_done = 0;
        if (s_cover_done_gen == s_cover_gen && s_cover_ok) {
            sceKernelDcacheWritebackRange(s_cover[0], sizeof(s_cover[0]));
            s_cover_shown = 0;
            dirty = 1;
        }
    }
    MusicNow now;
    if (music_state() == MUSIC_PLAYING && music_now(&now) == 0 && now.elapsed_ms / 1000 != N.shown_sec) {
        N.shown_sec = now.elapsed_ms / 1000;
        dirty = 1;
    }
    if (N.lyr.n && music_now(&now) == 0) { /* the next lyric line comes in */
        int k = lrc_index(&N.lyr, now.elapsed_ms);
        if (k != N.lyr_idx) { N.lyr_idx = k; dirty = 1; }
    }
    return dirty;
}

/* ------------------------------------------------------------------ draw */

static void time_str(char *b, int n, int sec)
{
    if (sec >= 3600) snprintf(b, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(b, n, "%d:%02d", sec / 60, sec % 60);
}

/* Triangles from 1px rows: pointing right (play, next) or left (previous). */
static void tri(int x, int y, int h, int left, u32 c)
{
    int wmax = 1 + (h / 2) * 7 / 4;
    for (int i = 0; i < h; i++) {
        int d = i <= h / 2 ? i : h - 1 - i, w = 1 + d * 7 / 4;
        gfx_rect(left ? x + wmax - w : x, y + i, w, 1, c);
    }
}

static int tri_w(int h) { return 1 + (h / 2) * 7 / 4; }

/* ⏮ ⏯ ⏭ centred on cx. */
static void draw_transport(int cx, int y, int playing, int active)
{
    const int b = BTN, ih = 13;
    u32 fg = active ? C_TEXT : C_MUTED;
    /* previous: a bar and a triangle pointing left */
    int px = cx - b / 2 - 22 - tri_w(ih) - 3, py = y + (b - ih) / 2;
    gfx_rect(px, py, 2, ih, fg);
    tri(px + 3, py, ih, 1, fg);
    /* play / pause in a round button */
    gfx_rrect(cx - b / 2, y, b, b, b / 2, active ? C_ACCENT : C_RAISED);
    if (playing) {
        gfx_rect(cx - 6, y + (BTN - 14) / 2, 4, 14, C_WHITE);
        gfx_rect(cx + 2, y + (BTN - 14) / 2, 4, 14, C_WHITE);
    } else {
        tri(cx - 4, y + (BTN - 16) / 2, 16, 0, C_WHITE);
    }
    /* next: a triangle pointing right and a bar */
    int nx = cx + b / 2 + 22;
    tri(nx, py, ih, 0, fg);
    gfx_rect(nx + tri_w(ih) + 1, py, 2, ih, fg);
}

/* A small rounded label, lit when on; returns the x after it. */
static int pill(int x, int y, const char *s, int on)
{
    int w = gfx_text_width(FONT_SMALL, s) + 16, h = gfx_line_h(FONT_SMALL) + 6;
    gfx_rrect(x, y, w, h, h / 2, on ? C_ACCENT : C_RAISED);
    gfx_text(FONT_SMALL, x + 8, y + 3, s, on ? C_WHITE : C_MUTED);
    return x + w + 6;
}

/* The title without a leading "Artist - " (files named the YouTube way repeat
 * the artist that is shown right under it). */
static const char *bare_title(const char *title, const char *artist)
{
    int n = (int)strlen(artist);
    if (n == 0 || strncasecmp(title, artist, n))
        return title;
    const char *p = title + n;
    while (*p == ' ') p++;
    if (*p == '-') p++;
    else if (!strncmp(p, "\xE2\x80\x93", 3)) p += 3; /* an en dash */
    else return title;
    while (*p == ' ') p++;
    return *p ? p : title;
}

/* s in at most two lines of width w, broken at a space, the second cut with "...".
 * Returns the y under it. */
static int text2(int font, int x, int y, int w, const char *s, u32 c)
{
    int lh = gfx_line_h(font);
    if (gfx_text_width(font, s) <= w) {
        gfx_text(font, x, y, s, c);
        return y + lh;
    }
    const char *p = s, *brk = NULL;
    int width = 0;
    while (*p) {
        const char *q = p;
        int adv = gfx_glyph_adv(font, &q);
        if (width + adv > w)
            break;
        width += adv;
        p = q;
        if (*p == ' ') brk = p;
    }
    const char *end = brk && brk > s ? brk : p; /* no space to break at: cut the word */
    gfx_text_n(font, x, y, s, (int)(end - s), c);
    while (*end == ' ') end++;
    gfx_text_fit(font, x, y + lh, w, end, c);
    return y + 2 * lh;
}

void np_draw(const SysStatus *st)
{
    int t = N.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    int oy = (256 - t) * 60 >> 8; /* rises into place as it fades in */
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_draw_status_bar("Now Playing", st);

    MusicNow now;
    int state = music_state();
    int on = state != MUSIC_STOPPED && music_now(&now) == 0;
    const char *rt = NULL, *ra = NULL;
    int resumable = !on && music_can_resume(&rt, &ra);

    /* The cover on the left; on the right the words at its top edge, then
     * the time, the buttons, and shuffle/repeat ending at its bottom edge. */
    const int y0 = COVER_Y + oy, s = COVER_S, bottom = y0 + s;
    if (s_cover_shown >= 0 && on) { /* the big one once decoded, the library's small one until then */
        gfx_texture(s_cover[s_cover_shown], GU_PSM_8888, 256, 256, COVER, 0, COVER, COVER, COVER_X, y0, s, s);
        gfx_corners(COVER_X, y0, s, s, 12, C_BG);
    } else if (N.have_e && on) {
        ui_draw_cover(&N.e, COVER_X, y0, s, 12, C_BG);
    } else {
        gfx_rrect(COVER_X, y0, s, s, 12, C_SURFACE);
        gfx_icon(ICON_MUSIC, COVER_X + (s - 64) / 2, y0 + (s - 64) / 2, 64, 64, C_MUTED);
    }

    const int x = COL_X, w = COL_W, fh = gfx_line_h(FONT_SMALL);
    int pill_y = bottom - (fh + 6);    /* shuffle / repeat, flush with the cover's bottom */
    int btn_y = pill_y - 8 - BTN;      /* the round play button */
    int time_y = btn_y - 8 - fh;       /* 0:08 ━━━━ 2:49 */
    int y = y0 - 4;
    char line[200], t1[16], t2[16];
    if (!on) {
        if (resumable) {
            y = text2(FONT_BOLD, x, y, w, rt[0] ? rt : "Your music", C_TEXT) + 2;
            gfx_text_fit(FONT_BODY, x, y, w, ra[0] ? ra : "Unknown artist", C_MUTED);
            gfx_text_fit(FONT_SMALL, x, time_y, w, "Stopped here last time. Carry on?", C_MUTED);
        } else {
            gfx_text_fit(FONT_BOLD, x, y, w, "Nothing playing", C_TEXT);
            y += gfx_line_h(FONT_BOLD) + 2;
            gfx_text_fit(FONT_BODY, x, y, w, "Pick a song or a playlist in Music", C_MUTED);
        }
        draw_transport(x + w / 2, btn_y, 0, resumable);
    } else {
        const char *artist = N.have_e ? N.e.artist : now.artist;
        const char *title = bare_title(N.have_e ? N.e.title : now.title, artist);
        y = text2(FONT_BOLD, x, y, w, title, C_TEXT) + 2;
        const char *album = N.have_e ? N.e.album : "";
        if (artist[0] && album[0]) snprintf(line, sizeof(line), "%s" DOT "%s", artist, album);
        else snprintf(line, sizeof(line), "%s", artist[0] ? artist : album[0] ? album : "Unknown artist");
        gfx_text_fit(FONT_BODY, x, y, w, line, C_MUTED);
        y += gfx_line_h(FONT_BODY);
        if (y + 8 > time_y) { /* a two-line title: the rest moves down below it */
            int d = y + 8 - time_y;
            time_y += d;
            btn_y += d;
            pill_y += d;
        }

        /* the time: fixed slots for the two times, so the bar never moves */
        int el = now.elapsed_ms / 1000, d = now.duration_ms / 1000, paused = state == MUSIC_PAUSED;
        if (d > 0 && el > d) el = d;
        time_str(t1, sizeof(t1), el);
        if (d > 0) time_str(t2, sizeof(t2), d);
        else snprintf(t2, sizeof(t2), "--:--");
        int slot = gfx_text_width(FONT_SMALL, d >= 3600 ? "0:00:00" : "00:00");
        int bx = x + slot + 10, bw = x + w - slot - 10 - bx, by = time_y + fh / 2 - 2;
        gfx_text(FONT_SMALL, x, time_y, t1, C_MUTED);
        gfx_text_right(FONT_SMALL, x + w, time_y, t2, C_MUTED);
        gfx_rrect(bx, by, bw, 4, 2, C_RAISED);
        int fill = d > 0 ? (int)((long long)bw * el / d) : 0;
        if (fill >= 4)
            gfx_rrect(bx, by, fill, 4, 2, paused ? C_MUTED : C_FOCUS);
        if (d > 0)
            gfx_rrect(bx + fill - 5, by - 3, 10, 10, 5, paused ? C_MUTED : C_TEXT);

        draw_transport(x + w / 2, btn_y, !paused, 1);
    }

    /* shuffle and repeat, and where in the list */
    int px = pill(x, pill_y, "Shuffle", music_shuffle());
    int r = music_repeat();
    px = pill(px, pill_y, r == REPEAT_ALL ? "Repeat all" : r == REPEAT_ONE ? "Repeat one" : "Repeat", r != REPEAT_OFF);
    int sl = music_sleep_left();
    if (sl) { /* the sleep timer (Up/Down) */
        char z[24];
        snprintf(z, sizeof(z), "Sleep %dm", sl);
        pill(px, pill_y, z, 1);
    }
    if (on && !sl) { /* (the sleep pill takes its place) */
        snprintf(line, sizeof(line), "%d of %d", now.index + 1, now.count);
        gfx_text_right(FONT_SMALL, x + w, pill_y + 3, line, C_MUTED);
    }

    /* what comes next, under the cover */
    const char *nt, *na;
    int ny = bottom + 12 > pill_y + fh + 6 + 10 ? bottom + 12 : pill_y + fh + 6 + 10;
    if (on && N.lyr.n) { /* the lyrics: the line now, and the next one fainter */
        int k = lrc_index(&N.lyr, now.elapsed_ms);
        const char *cur = k >= 0 ? N.lyr.line[k].text : "";
        const char *next = k + 1 < N.lyr.n ? N.lyr.line[k + 1].text : "";
        int lw = SCREEN_W - 2 * COVER_X;
        ny -= 8; /* closer under the cover than "Up next": two lines must fit */
        int two = ny + gfx_line_h(FONT_BODY) + gfx_line_h(FONT_SMALL) <= 247; /* room above the hints? */
        if (cur[0]) {
            gfx_text_fit(FONT_BODY, COVER_X, ny, lw, cur, C_TEXT);
            if (next[0] && two) gfx_text_fit(FONT_SMALL, COVER_X, ny + gfx_line_h(FONT_BODY), lw, next, C_MUTED);
        } else if (next[0]) { /* a pause (or before the first line): what's coming */
            gfx_text_fit(FONT_BODY, COVER_X, ny, lw, next, C_MUTED);
        }
    } else if (on && r == REPEAT_ONE) {
        int lx = gfx_text(FONT_SMALL, COVER_X, ny, "On repeat", C_FOCUS) + 8;
        gfx_text_fit(FONT_SMALL, lx, ny, SCREEN_W - 24 - lx, "This song plays again (-> for the next one)", C_TEXT);
    } else if (on && music_peek(1, &nt, &na) == 0) {
        int lx = gfx_text(FONT_SMALL, COVER_X, ny, "Up next", C_FOCUS) + 8;
        nt = bare_title(nt, na);
        snprintf(line, sizeof(line), na[0] ? "%s" DOT "%s" : "%s", nt, na);
        gfx_text_fit(FONT_SMALL, lx, ny, SCREEN_W - 24 - lx, line, C_TEXT);
    } else if (on) {
        gfx_text(FONT_SMALL, COVER_X, ny, "Last song in the list", C_MUTED);
    }

    gfx_rect(0, 248, SCREEN_W, 1, C_LINE);
    static const Hint play_h[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Pause"},
        {-1, "<-/->", 0, "Song"},
        {-1, "L/R", 0, "Seek"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Shuffle"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Repeat"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    Hint h[6];
    int n = 0;
    if (on) {
        memcpy(h, play_h, sizeof(play_h));
        n = COUNT(play_h);
        if (state == MUSIC_PAUSED) h[0].label = "Play";
    } else {
        h[n++] = (Hint){ICON_BTN_CROSS, NULL, C_BTN_CROSS, resumable ? "Resume" : "Open Music"};
        h[n++] = play_h[3];
        h[n++] = play_h[4];
        h[n++] = play_h[5];
    }
    ui_draw_hints(h, n, SCREEN_W / 2, NP_HINT_Y);
    gfx_set_opacity(256);
}
