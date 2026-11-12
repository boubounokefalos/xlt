/* XLT - picture viewer.
 *
 * ←/→ step through the Pictures list (or the folder it was opened from),
 * R / L zoom in and out, the d-pad moves around a zoomed picture, □ turns it
 * a quarter (remembered per picture in cache/rotate.txt), START runs a
 * slideshow, ✕ hides or shows the info bars, △ opens the options sheet,
 * ○ zooms back out, then closes.
 *
 * The picture as shown at first is decoded on the background worker, already
 * fitted to 480x272, into a RAM staging buffer; the UI thread converts it to
 * 5650 straight into the viewer's VRAM area between frames (the GPU is idle
 * then). Zooming in decodes it again, up to 1024x1024, into a 5650 copy in
 * RAM, drawn in 512-pixel tiles (the GE's largest texture); until that copy
 * is ready the fitted one is stretched. The previous picture stays on screen
 * while the next one loads.
 */
#include <pspkernel.h>
#include <pspge.h>
#include <pspgu.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfx.h"
#include "viewer.h"
#include "ui_internal.h"
#include "anim.h"
#include "bg.h"
#include "imgdecode.h"
#include "library.h"
#include "opts.h"
#include "system.h"
#include "pathmap.h"

/* The viewer's area in VRAM, and the same through the uncached mirror (the
 * PC preview, tests/viewer_preview.c, puts its own memory here). */
#ifndef VIEWER_VRAM
#define VIEWER_VRAM() ((u16 *)((u32)sceGeEdramGetAddr() + GFX_VRAM_VIEWER))
#define VIEWER_VRAM_UNCACHED() ((u16 *)((u32)VIEWER_VRAM() | 0x40000000u))
#endif

#define VW 480
#define VH 272
#define TBW 512
#define HI_MAX 1024            /* the zoomed copy's longest side */
#define ZOOM_MAX (8 * 256)     /* 8x the fitted size */
#define SLIDE_MS 5000

enum { ST_NONE, ST_LOADING, ST_SHOWN, ST_FAILED };
enum { HI_NONE, HI_LOADING, HI_READY, HI_FAILED };

static struct {
    int open;
    Spring t;
    Entry e;        /* the picture being shown (a copy) */
    int idx;        /* in VIEW_PICTURES (or the source), -1 if not in it (e.g. a removed one) */
    const ViewerSource *src; /* NULL: the Pictures list */
    int state;
    int have_image; /* the VRAM area holds a decoded picture */
    int iw, ih;     /* its size */
    int info;       /* info bars shown */
    int gen;
    int rot;        /* quarter turns clockwise (0..3) */
    /* zoom: 256 = fitted; the centre of the view in the picture's own units (0..1 of w and h) */
    int zoom;
    float cx, cy;
    unsigned held;  /* buttons held this frame (moving around) */
    /* the zoomed copy */
    int hi_state, hi_gen;
    u16 *hi;
    int hw, hh, htbw;
    /* slideshow */
    int slides;
    u32 slide_at;
    char toast[32];
    int toast_left;
} V;

static PathMap s_rot;
static char s_rot_path[220];

/* Decode job: arguments, and the result handed back through `ready`. */
typedef struct {
    int gen, rot;
    char path[sizeof(((Entry *)0)->path)];
} DecodeArg;

static uint8_t s_rgba[VW * VH * 4] __attribute__((aligned(16)));
static struct {
    volatile int ready;
    int gen, ok, w, h, rot;
    int stride; /* pixels per row in s_rgba: the box it was fitted to */
} D;

/* The zoomed copy, built on the worker. */
static struct {
    volatile int ready;
    int gen;
    u16 *buf;
    int w, h, tbw;
} H;

static u32 now_ms(void) { return sceKernelGetSystemTimeLow() / 1000; }

static void job_decode(void *arg)
{
    const DecodeArg *a = arg;
    D.ready = 0;
    power_boost();
    int w = 0, h = 0, odd = a->rot & 1;
    /* turned a quarter, it must fit 480x272 the other way round */
    int ok = img_decode(a->path, IMG_FIT, odd ? VH : VW, odd ? VW : VH, s_rgba, &w, &h) == 0;
    if (!library_scanning())
        power_idle();
    D.gen = a->gen;
    D.ok = ok;
    D.w = w;
    D.h = h;
    D.rot = a->rot;
    D.stride = odd ? VH : VW; /* img_decode keeps the box's width per row */
    D.ready = 1;
}

static inline u16 to5650(const uint8_t *s) { return (u16)((s[2] >> 3) << 11 | (s[1] >> 2) << 5 | (s[0] >> 3)); }

/* RGBA w x h (stride sw pixels) into dst (stride dtbw), turned rot quarters clockwise. */
static void put_turned(u16 *dst, int dtbw, const uint8_t *src, int sw, int w, int h, int rot)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * sw * 4;
        for (int x = 0; x < w; x++, s += 4) {
            int dx, dy;
            switch (rot) {
            case 1: dx = h - 1 - y; dy = x; break;
            case 2: dx = w - 1 - x; dy = h - 1 - y; break;
            case 3: dx = y; dy = w - 1 - x; break;
            default: dx = x; dy = y; break;
            }
            dst[dy * dtbw + dx] = to5650(s);
        }
    }
}

static void job_decode_hi(void *arg)
{
    const DecodeArg *a = arg;
    H.ready = 0;
    H.buf = NULL;
    power_boost();
    int w = 0, h = 0;
    uint8_t *rgba = malloc((size_t)HI_MAX * HI_MAX * 4);
    if (rgba && img_decode(a->path, IMG_FIT, HI_MAX, HI_MAX, rgba, &w, &h) == 0 && w > 0 && h > 0) {
        int odd = a->rot & 1, ow = odd ? h : w, oh = odd ? w : h;
        /* 16-byte rows (the GE's rule), and one spare column and row copied from the
         * edge: filtering at a tile's edge reads one texel past it */
        int tbw = (ow + 1 + 7) & ~7;
        size_t bytes = (size_t)tbw * (oh + 1) * 2;
        u16 *buf = memalign(16, bytes);
        if (buf) {
            put_turned(buf, tbw, rgba, HI_MAX, w, h, a->rot); /* rows are the box's width apart */
            for (int y = 0; y < oh; y++)
                buf[y * tbw + ow] = buf[y * tbw + ow - 1];
            memcpy(buf + (size_t)oh * tbw, buf + (size_t)(oh - 1) * tbw, (size_t)tbw * 2);
            sceKernelDcacheWritebackRange(buf, bytes); /* the GE reads RAM directly */
            H.buf = buf;
            H.w = ow;
            H.h = oh;
            H.tbw = tbw;
        }
    }
    free(rgba);
    if (!library_scanning())
        power_idle();
    H.gen = a->gen;
    H.ready = 1;
}

static void drop_hi(void)
{
    free(V.hi);
    V.hi = NULL;
    V.hi_state = HI_NONE;
    V.zoom = 256;
    V.cx = V.cy = 0.5f;
}

static void load(const Entry *e)
{
    V.e = *e;
    V.state = ST_LOADING;
    V.rot = pm_get(&s_rot, e->path) & 3;
    drop_hi();
    static DecodeArg a;
    a.gen = ++V.gen;
    a.rot = V.rot;
    snprintf(a.path, sizeof(a.path), "%s", e->path);
    bg_post(job_decode, &a, sizeof(a));
}

static void want_hi(void)
{
    if (V.hi_state != HI_NONE)
        return;
    V.hi_state = HI_LOADING;
    static DecodeArg a;
    a.gen = V.hi_gen = V.gen;
    a.rot = V.rot;
    snprintf(a.path, sizeof(a.path), "%s", V.e.path);
    bg_post(job_decode_hi, &a, sizeof(a));
}

/* ------------------------------------------------------------- rotations */

typedef struct {
    char *text;
    int len;
    char path[220];
} RotArg;

static void job_save_rot(void *arg)
{
    const RotArg *a = arg;
    SceUID fd = sceIoOpen(a->path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, a->text, a->len);
        sceIoClose(fd);
    }
    free(a->text);
}

static void save_rot(void)
{
    RotArg a;
    int cap = PM_MAX * 210 + 1;
    a.text = malloc(cap);
    if (!a.text)
        return;
    a.len = pm_write(&s_rot, a.text, cap);
    if (a.len < 0) {
        free(a.text);
        return;
    }
    snprintf(a.path, sizeof(a.path), "%s", s_rot_path);
    bg_post(job_save_rot, &a, sizeof(a)); /* the worker writes, then frees the text */
}

void viewer_init(const char *app_dir)
{
    snprintf(s_rot_path, sizeof(s_rot_path), "%.190scache/rotate.txt", app_dir);
    SceUID fd = sceIoOpen(s_rot_path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    char *t = malloc(PM_MAX * 210);
    int n = t ? sceIoRead(fd, t, PM_MAX * 210) : -1;
    sceIoClose(fd);
    if (n > 0)
        pm_parse(&s_rot, t, n);
    free(t);
}

/* ------------------------------------------------------------------ open */

static void open_common(void)
{
    V.open = 1;
    V.info = 1;
    V.have_image = 0;
    V.slides = 0;
    spring_to(&V.t, FX(1));
}

void viewer_open(const Entry *e)
{
    V.src = NULL;
    open_common();
    V.idx = library_find(VIEW_PICTURES, e->path);
    load(e);
}

void viewer_open_source(const ViewerSource *src, int idx)
{
    Entry e;
    if (!src || src->get(idx, &e) != 0)
        return;
    V.src = src;
    open_common();
    V.idx = idx;
    load(&e);
}

static int count(void) { return V.src ? V.src->count() : library_count(VIEW_PICTURES); }

int viewer_is_open(void) { return V.open || V.t.x > 0; }
int viewer_covers(void) { return V.open && V.t.x >= FX(1); }
int viewer_slideshow(void) { return V.open && V.slides; }

void viewer_refresh(void)
{
    if (V.open && !V.src)
        V.idx = library_find(VIEW_PICTURES, V.e.path);
}

/* wrap: the slideshow goes round; stepping by hand stops at the ends, like the lists. */
static void go(int dir, int wrap)
{
    int n = count();
    if (V.idx < 0 || n == 0)
        return;
    int i = V.idx + dir;
    if (wrap) i = (i + n) % n;
    if (i < 0 || i >= n)
        return;
    V.idx = i;
    if (V.src) {
        Entry e;
        if (V.src->get(i, &e) == 0)
            load(&e);
    } else {
        load(library_get(VIEW_PICTURES, i));
    }
}

static void note(const char *s)
{
    snprintf(V.toast, sizeof(V.toast), "%s", s);
    V.toast_left = 90;
}

static void zoom_by(int up)
{
    int z = up ? V.zoom * 3 / 2 : V.zoom * 2 / 3;
    if (z <= 256) {
        if (V.zoom != 256) sfx_play(SFX_BACK);
        V.zoom = 256;
        V.cx = V.cy = 0.5f;
        return;
    }
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    if (z != V.zoom) sfx_play(SFX_MOVE);
    V.zoom = z;
    want_hi();
}

int viewer_input(unsigned int p)
{
    if (!V.open)
        return 0;
    int zoomed = V.zoom > 256;
    if (p & PSP_CTRL_CIRCLE) {
        if (zoomed) { /* all the way out first */
            sfx_play(SFX_BACK);
            V.zoom = 256;
            V.cx = V.cy = 0.5f;
        } else {
            V.open = 0;
            V.slides = 0;
            spring_to(&V.t, 0);
        }
    } else if (p & PSP_CTRL_RTRIGGER) {
        zoom_by(1);
    } else if (p & PSP_CTRL_LTRIGGER) {
        zoom_by(0);
    } else if (!zoomed && p & PSP_CTRL_LEFT) {
        go(-1, V.slides);
        V.slide_at = now_ms();
        sfx_play(SFX_CARD);
    } else if (!zoomed && p & PSP_CTRL_RIGHT) {
        go(1, V.slides);
        V.slide_at = now_ms();
        sfx_play(SFX_CARD);
    } else if (p & PSP_CTRL_SQUARE) { /* a quarter turn, kept for next time */
        int r = (V.rot + 1) & 3;
        pm_set(&s_rot, V.e.path, r);
        save_rot();
        sfx_play(SFX_MOVE);
        load(&V.e); /* decoded again the new way round */
    } else if (p & PSP_CTRL_START) {
        V.slides = !V.slides;
        V.slide_at = now_ms();
        if (V.slides) V.info = 0;
        sfx_play(V.slides ? SFX_ON : SFX_OFF);
        note(V.slides ? "Slideshow" : "Slideshow stopped");
    } else if (p & PSP_CTRL_CROSS) {
        V.info = !V.info;
    } else if (p & PSP_CTRL_TRIANGLE && !V.src) { /* the library's options; files have their own menu */
        V.slides = 0;
        opts_open(&V.e);
    }
    return 1;
}

void viewer_hold(unsigned int held) { V.held = held; }

int viewer_step(void)
{
    int old = V.t.x;
    spring_step(&V.t);
    int dirty = V.t.x != old;
    if (D.ready) {
        D.ready = 0;
        if (D.gen == V.gen && V.open) {
            if (D.ok) {
                /* RGBA -> 5650 into VRAM (turned as asked), through the uncached mirror */
                u16 *dst = VIEWER_VRAM_UNCACHED();
                put_turned(dst, TBW, s_rgba, D.stride, D.w, D.h, D.rot);
                V.iw = D.rot & 1 ? D.h : D.w;
                V.ih = D.rot & 1 ? D.w : D.h;
                V.have_image = 1;
                V.state = ST_SHOWN;
            } else {
                V.state = ST_FAILED;
            }
            dirty = 1;
        }
    }
    if (H.ready) {
        H.ready = 0;
        if (H.gen == V.hi_gen && H.gen == V.gen && V.open && V.hi_state == HI_LOADING) {
            V.hi = H.buf;
            V.hw = H.w;
            V.hh = H.h;
            V.htbw = H.tbw;
            V.hi_state = V.hi ? HI_READY : HI_FAILED;
            dirty = 1;
        } else {
            free(H.buf); /* for a picture no longer shown */
        }
    }
    if (V.open && V.zoom > 256 && V.held & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
        /* move about: 6 screen pixels a frame, whatever the zoom */
        float fit = V.iw && V.ih ? (float)V.iw : 1.0f, z = V.zoom / 256.0f;
        float sx = 6.0f / (fit * z), sy = 6.0f / ((V.ih ? V.ih : 1) * z);
        if (V.held & PSP_CTRL_LEFT) V.cx -= sx;
        if (V.held & PSP_CTRL_RIGHT) V.cx += sx;
        if (V.held & PSP_CTRL_UP) V.cy -= sy;
        if (V.held & PSP_CTRL_DOWN) V.cy += sy;
        dirty = 1;
    }
    if (V.open && V.zoom > 256 && V.iw && V.ih) { /* the view stays on the picture */
        float z = V.zoom / 256.0f;
        float hx = VW / 2.0f / (V.iw * z), hy = VH / 2.0f / (V.ih * z); /* half the view, in picture units */
        if (hx > 0.5f) hx = 0.5f; /* narrower than the screen: it stays centred */
        if (hy > 0.5f) hy = 0.5f;
        if (V.cx < hx) V.cx = hx;
        if (V.cx > 1 - hx) V.cx = 1 - hx;
        if (V.cy < hy) V.cy = hy;
        if (V.cy > 1 - hy) V.cy = 1 - hy;
    }
    if (V.open && V.slides && V.state != ST_LOADING && now_ms() - V.slide_at >= SLIDE_MS) {
        V.slide_at = now_ms();
        scePowerTick(PSP_POWER_TICK_DISPLAY); /* the screen stays on for the show */
        if (V.zoom == 256) go(1, 1);
        dirty = 1;
    }
    if (V.toast_left > 0 && --V.toast_left == 0)
        dirty = 1;
    return dirty;
}

/* ------------------------------------------------------------------ draw */

/* A picture tex (w x h, stride tbw) drawn so that its point (cx, cy) (0..1)
 * sits in the middle of the screen at scale s screen pixels per texel, in
 * tiles of at most 512 (the GE's largest texture). */
static void draw_scaled(const u16 *tex, int w, int h, int tbw, float cx, float cy, float s)
{
    float vx0 = cx * w - VW / 2.0f / s, vy0 = cy * h - VH / 2.0f / s; /* the view's corner, in texels */
    for (int ty = 0; ty < h; ty += 512)
        for (int tx = 0; tx < w; tx += 512) {
            int tw = w - tx < 512 ? w - tx : 512, th = h - ty < 512 ? h - ty : 512;
            int x0 = (int)((tx - vx0) * s), y0 = (int)((ty - vy0) * s);
            int x1 = (int)((tx + tw - vx0) * s), y1 = (int)((ty + th - vy0) * s);
            if (x1 <= 0 || y1 <= 0 || x0 >= VW || y0 >= VH)
                continue;
            gfx_bind(tex + (size_t)ty * tbw + tx, GU_PSM_5650, 512, 512, tbw, 0);
            gfx_sprite(x0, y0, x1, y1, 0, 0, tw, th, 0xFFFFFFFFu);
        }
    gfx_bind_atlas();
}

void viewer_draw(void)
{
    int t = V.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGB(0x000000));

    const u16 *vram = VIEWER_VRAM();
    if (V.state == ST_FAILED) {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, SCREEN_H / 2 - 8, "Can't show this picture", C_MUTED);
    } else if (V.have_image && V.zoom > 256 && V.hi_state == HI_READY) {
        /* the sharp copy: as many of its texels per screen pixel as the zoom asks */
        float fit = (float)V.iw / V.hw; /* screen pixels per texel when fitted */
        draw_scaled(V.hi, V.hw, V.hh, V.htbw, V.cx, V.cy, fit * V.zoom / 256.0f);
    } else if (V.have_image && V.zoom > 256) {
        draw_scaled(vram, V.iw, V.ih, TBW, V.cx, V.cy, V.zoom / 256.0f); /* stretched until it's ready */
    } else if (V.have_image) {
        gfx_texture(vram, GU_PSM_5650, 512, 512, TBW, 0, V.iw, V.ih,
                    (SCREEN_W - V.iw) / 2, (SCREEN_H - V.ih) / 2, V.iw, V.ih);
    } else {
        gfx_text_center(FONT_BODY, SCREEN_W / 2, SCREEN_H / 2 - 8, "Loading...", C_MUTED);
    }

    if (V.info) {
        char buf[32];
        gfx_rect(0, 0, SCREEN_W, 24, 0xA0000000u);
        int n = count();
        if (V.zoom > 256)
            snprintf(buf, sizeof(buf), "%d%%", V.zoom * 100 / 256);
        else if (V.idx >= 0)
            snprintf(buf, sizeof(buf), "%d / %d", V.idx + 1, n);
        else
            snprintf(buf, sizeof(buf), "Removed");
        int bw = gfx_text_width(FONT_SMALL, buf);
        gfx_text_fit(FONT_SMALL, 12, 5, SCREEN_W - 36 - bw, V.e.title, C_WHITE);
        gfx_text_right(FONT_SMALL, SCREEN_W - 12, 5, buf, RGB(0xc0c0c0));

        gfx_rect(0, 248, SCREEN_W, 24, 0xA0000000u);
        static const Hint flat_h[] = {
            {-1, "<-/->", 0, "Pictures"},
            {-1, "L/R", 0, "Zoom"},
            {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Turn"},
            {-1, "START", 0, "Slideshow"},
            {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Options"},
        };
        static const Hint zoom_h[] = {
            {-1, "D-pad", 0, "Move"},
            {-1, "L/R", 0, "Zoom"},
            {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Fit"},
        };
        if (V.zoom > 256) ui_draw_hints(zoom_h, COUNT(zoom_h), SCREEN_W / 2, 253);
        else ui_draw_hints(flat_h, COUNT(flat_h) - (V.src ? 1 : 0), SCREEN_W / 2, 253);
    }
    const char *pill = V.toast_left ? V.toast
                     : V.state == ST_LOADING && V.have_image ? "Loading..."
                     : V.zoom > 256 && V.hi_state == HI_LOADING ? "Sharpening..." : NULL;
    if (pill) {
        int w = gfx_text_width(FONT_SMALL, pill) + 20;
        gfx_rrect(SCREEN_W - w - 10, 30, w, 20, 10, 0xC0000000u);
        gfx_text(FONT_SMALL, SCREEN_W - w, 33, pill, C_WHITE);
    }
    gfx_set_opacity(256);
}
