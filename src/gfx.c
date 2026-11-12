/* XLT - sceGu renderer.
 *
 * VRAM layout (offsets from the start of eDRAM):
 *   0x000000  frame buffer 0   (512x272, 5650)
 *   0x044000  frame buffer 1
 *   0x088000  depth buffer     (unused: depth test and writes are off)
 *   0x0CC000  atlas            (256x256, 4444, swizzled)
 *
 * Every primitive is a pair of sprite vertices in one static buffer. The main
 * texture is the atlas, so a frame is mostly a single sceGuDrawArray call with
 * no state changes. Flat rectangles sample the atlas' solid white block.
 * Non-ASCII letters (Latin-1, Greek) come from a second atlas in main RAM,
 * bound only for the strings that use them.
 */
/* Parts of this file (the sceGu set-up) are adapted from psp5 by 08Weber,
 * https://github.com/08Weber/psp5, GPL-3.0. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <string.h>

#define ATLAS_DATA_IMPL
#include "gfx.h"

#define BUF_W 512
#define FB_BYTES (BUF_W * SCREEN_H * 2)
#define ATLAS_OFFSET (FB_BYTES * 3)
_Static_assert(ATLAS_OFFSET + ATLAS_W * ATLAS_H * 2 == GFX_VRAM_FREE, "VRAM layout");
_Static_assert(GFX_VRAM_VIEWER + GFX_VIEWER_BYTES <= 0x200000, "thumbnails + viewer exceed VRAM");

typedef struct {
    unsigned short u, v;
    u32 color;
    short x, y, z;
} Vtx;

#define VFMT (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define MAX_VERTS 8192

static unsigned int __attribute__((aligned(16))) s_list[32768];
static Vtx __attribute__((aligned(16))) s_vb[MAX_VERTS];
static int s_vb_start, s_vb_count;
static int s_opacity = 256;
static void *s_atlas;

/* The GE adds this to every pixel it writes (8-bit units), then truncates to
 * 5/6/5 bits; the default is -4..3. Every written pixel is dithered, also one a
 * transparent texel leaves unchanged, and -4..-1 then knock an already-stored
 * colour one step darker: the transparent corners of a sprite showed as a faint
 * darker box (the screensaver's moving lights made it visible). 0..3 leaves a
 * stored colour as it is (for green's 6 bits, and red/blue below ~0xA0, so the
 * whole dark UI) and still breaks up gradients. Alpha test also skips texels
 * that are fully transparent, for the bright cases. */
static const int gfx_dither_matrix[4][4] = {{0, 2, 0, 2}, {3, 1, 3, 1}, {0, 2, 0, 2}, {3, 1, 3, 1}};

static void flush(void)
{
    int n = s_vb_count - s_vb_start;
    if (n <= 0)
        return;
    sceKernelDcacheWritebackRange(&s_vb[s_vb_start], n * sizeof(Vtx));
    sceGuDrawArray(GU_SPRITES, VFMT, n, 0, &s_vb[s_vb_start]);
    s_vb_start = s_vb_count;
}

static inline u32 apply_opacity(u32 c)
{
    if (s_opacity >= 256)
        return c;
    u32 a = ((c >> 24) * (u32)s_opacity) >> 8;
    return (c & 0x00FFFFFFu) | (a << 24);
}

static void sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 c)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    if (s_vb_count + 2 > MAX_VERTS)
        return; /* frame is far beyond anything the UI draws; drop rather than overrun */
    c = apply_opacity(c);
    if ((c >> 24) == 0)
        return;
    Vtx *v = &s_vb[s_vb_count];
    v[0].u = u0; v[0].v = v0; v[0].color = c; v[0].x = x0; v[0].y = y0; v[0].z = 0;
    v[1].u = u1; v[1].v = v1; v[1].color = c; v[1].x = x1; v[1].y = y1; v[1].z = 0;
    s_vb_count += 2;
}

void gfx_init(void)
{
    /* Upload the pre-swizzled atlas once through the uncached mirror. */
    void *edram = sceGeEdramGetAddr();
    void *atlas = (void *)((u32)edram + ATLAS_OFFSET);
    s_atlas = atlas;
    memcpy((void *)((u32)atlas | 0x40000000u), atlas_pixels, sizeof(atlas_pixels));
    /* The extended glyphs are read by the GE straight from main RAM. */
    sceKernelDcacheWritebackRange(atlas_ext_pixels, sizeof(atlas_ext_pixels));

    sceGuInit();
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_W);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void *)FB_BYTES, BUF_W);
    sceGuDepthBuffer((void *)(FB_BYTES * 2), BUF_W);
    sceGuOffset(2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE); /* GU_TRUE disables depth writes */
    sceGuDisable(GU_CULL_FACE);
    ScePspIMatrix4 dither;
    int *dm = (int *)&dither;
    for (int i = 0; i < 16; i++)
        dm[i] = gfx_dither_matrix[i / 4][i % 4];
    sceGuSetDither(&dither);
    sceGuEnable(GU_DITHER);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, atlas);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void gfx_shutdown(void)
{
    sceGuTerm();
}

void gfx_begin(u32 clear_color)
{
    s_vb_start = s_vb_count = 0;
    s_opacity = 256;
    sceGuStart(GU_DIRECT, s_list);
    sceGuClearColor(clear_color);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

static u32 s_ge_wait_us;
u32 gfx_ge_wait_us(void) { return s_ge_wait_us; }

void gfx_end(void)
{
    flush();
    sceGuFinish();
#ifdef _PSP
    u32 t0 = sceKernelGetSystemTimeLow();
    sceGuSync(0, 0);
    s_ge_wait_us = sceKernelGetSystemTimeLow() - t0;
#else /* the PC previews */
    sceGuSync(0, 0);
#endif
    sceDisplayWaitVblankStartCB();
    sceGuSwapBuffers();
}

void gfx_set_opacity(int o)
{
    s_opacity = o < 0 ? 0 : o > 256 ? 256 : o;
}

u32 color_lerp(u32 a, u32 b, int t)
{
    if (t <= 0)
        return a;
    if (t >= 256)
        return b;
    u32 r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        r |= (u32)(ca + (((cb - ca) * t) >> 8)) << sh;
    }
    return r;
}

/* ------------------------------------------------------------------ shapes */

#define SU ATLAS_SOLID_U
#define SV ATLAS_SOLID_V

void gfx_rect(int x, int y, int w, int h, u32 c)
{
    sprite(x, y, x + w, y + h, SU, SV, SU, SV, c);
}

void gfx_rrect(int x, int y, int w, int h, int r, u32 c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        gfx_rect(x, y, w, h, c);
        return;
    }
    const int du = ATLAS_DISC_U, dv = ATLAS_DISC_V, q = ATLAS_DISC_SIZE / 2;
    int x1 = x + w, y1 = y + h;
    sprite(x, y, x + r, y + r, du, dv, du + q, dv + q, c);
    sprite(x1 - r, y, x1, y + r, du + q, dv, du + 2 * q, dv + q, c);
    sprite(x, y1 - r, x + r, y1, du, dv + q, du + q, dv + 2 * q, c);
    sprite(x1 - r, y1 - r, x1, y1, du + q, dv + q, du + 2 * q, dv + 2 * q, c);
    sprite(x + r, y, x1 - r, y1, SU, SV, SU, SV, c);
    sprite(x, y + r, x + r, y1 - r, SU, SV, SU, SV, c);
    sprite(x1 - r, y + r, x1, y1 - r, SU, SV, SU, SV, c);
}

void gfx_ring(int x, int y, int w, int h, u32 c)
{
    const int ru = ATLAS_RING_U, rv = ATLAS_RING_V, q = ATLAS_RING_SIZE / 2, s = ATLAS_RING_STROKE;
    int x1 = x + w, y1 = y + h;
    sprite(x, y, x + q, y + q, ru, rv, ru + q, rv + q, c);
    sprite(x1 - q, y, x1, y + q, ru + q, rv, ru + 2 * q, rv + q, c);
    sprite(x, y1 - q, x + q, y1, ru, rv + q, ru + q, rv + 2 * q, c);
    sprite(x1 - q, y1 - q, x1, y1, ru + q, rv + q, ru + 2 * q, rv + 2 * q, c);
    gfx_rect(x + q, y, w - 2 * q, s, c);
    gfx_rect(x + q, y1 - s, w - 2 * q, s, c);
    gfx_rect(x, y + q, s, h - 2 * q, c);
    gfx_rect(x1 - s, y + q, s, h - 2 * q, c);
}

void gfx_corners(int x, int y, int w, int h, int r, u32 c)
{
    const int u0 = ATLAS_CORNER_U, v0 = ATLAS_CORNER_V, u1 = u0 + ATLAS_CORNER_SIZE, v1 = v0 + ATLAS_CORNER_SIZE;
    int x1 = x + w, y1 = y + h;
    sprite(x, y, x + r, y + r, u0, v0, u1, v1, c);
    sprite(x1 - r, y, x1, y + r, u1, v0, u0, v1, c);   /* mirrored in u */
    sprite(x, y1 - r, x + r, y1, u0, v1, u1, v0, c);   /* mirrored in v */
    sprite(x1 - r, y1 - r, x1, y1, u1, v1, u0, v0, c);
}

void gfx_texture(const void *tex, int psm, int tw, int th, int tbw, int swizzled,
                 int uw, int vh, int x, int y, int w, int h)
{
    /* Flush what is queued, draw with this texture, switch back to the atlas. */
    flush();
    sceGuTexMode(psm, 0, 0, swizzled ? GU_TRUE : GU_FALSE);
    sceGuTexImage(0, tw, th, tbw, tex);
    sceGuTexFlush();
    sprite(x, y, x + w, y + h, 0, 0, uw, vh, 0xFFFFFFFFu); /* sprite() applies the opacity */
    flush();
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, s_atlas);
    sceGuTexFlush();
}

/* A picture in main RAM, RGBA 8888, of any width. Drawn in narrow vertical
 * strips: one sprite across the whole picture makes the GE's small texture cache
 * miss on nearly every pixel, costing tens of milliseconds a frame. */
void gfx_video_frame(const void *base, int tbw, int fw, int fh, int dx, int dy, int dw, int dh)
{
    enum { STRIP = 64, PAGE = 512 }; /* a texture is at most 512 wide */
    int filter = dw == fw && dh == fh ? GU_NEAREST : GU_LINEAR;
    flush();
    sceGuTexFilter(filter, filter);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB); /* alpha from the vertex only */
    for (int page = 0; page < fw; page += PAGE) {
        sceGuTexImage(0, PAGE, PAGE, tbw, (const u8 *)base + page * 4);
        sceGuTexFlush();
        int end = page + PAGE < fw ? page + PAGE : fw;
        for (int sx = page; sx < end; sx += STRIP) {
            int sw = sx + STRIP < end ? STRIP : end - sx;
            sprite(dx + sx * dw / fw, dy, dx + (sx + sw) * dw / fw, dy + dh,
                   sx - page, 0, sx - page + sw, fh, 0xFFFFFFFFu);
        }
        flush();
    }
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, s_atlas);
    sceGuTexFlush();
}

void gfx_bind(const void *tex, int psm, int tw, int th, int tbw, int swizzled)
{
    flush();
    sceGuTexMode(psm, 0, 0, swizzled ? GU_TRUE : GU_FALSE);
    sceGuTexImage(0, tw, th, tbw, tex);
    sceGuTexFlush();
}

void gfx_bind_atlas(void)
{
    gfx_bind(s_atlas, GU_PSM_4444, ATLAS_W, ATLAS_H, ATLAS_W, 1);
}

void gfx_sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 c)
{
    sprite(x0, y0, x1, y1, u0, v0, u1, v1, c);
}

void gfx_thumb(const void *tex, int x, int y, int w, int h)
{
    gfx_texture(tex, GU_PSM_5551, 256, 128, 144, 1, 143, 79, x, y, w, h);
}

void gfx_icon(int id, int x, int y, int w, int h, u32 c)
{
    const AtlasRect *r = &atlas_icons[id];
    sprite(x, y, x + w, y + h, r->x, r->y, r->x + r->w, r->y + r->h, c);
}

/* ------------------------------------------------------------------- text */

/* Decodes one UTF-8 character and advances *s. Returns its glyph; *ext is set
 * when the glyph lives in the extended atlas (Latin-1, Greek, punctuation). */
static const Glyph *glyph_of(int font, const char **s, int *ext)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp = *p++;
    *ext = 0;
    if (cp >= 0xC0) {
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
        cp &= more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F;
        while (more-- && (*p & 0xC0) == 0x80)
            cp = cp << 6 | (*p++ & 0x3F);
    } else if (cp >= 0x80) {
        cp = '?'; /* a stray continuation byte */
    }
    while ((*p & 0xC0) == 0x80) /* anything left of a malformed sequence */
        p++;
    *s = (const char *)p;
    const Glyph *gl = atlas_fonts[font].glyphs;
    if (cp >= 32 && cp < 127)
        return &gl[cp - 32];
    if (cp == 0xB7)
        return &gl[95];
    if (cp == '\t' || cp == 0xA0)
        return &gl[0];
    int lo = 0, hi = ATLAS_EXT_GLYPHS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (atlas_ext_cps[mid] == cp) {
            *ext = 1;
            return &atlas_ext_fonts[font][mid];
        }
        if (atlas_ext_cps[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return &gl['?' - 32];
}

int gfx_glyph_adv(int font, const char **s)
{
    int ext;
    return glyph_of(font, s, &ext)->adv;
}

int gfx_line_h(int font)
{
    return atlas_fonts[font].line_h;
}

/* Extended glyphs of one string are collected and drawn after it with the
 * extended atlas bound (from main RAM): two texture switches per string that
 * has any, none for plain ASCII. */
#define EXT_BATCH 128
static struct { short x, y; const Glyph *g; } s_ext[EXT_BATCH];
static int s_next;

static void ext_flush(u32 c)
{
    if (!s_next)
        return;
    flush();
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_EXT_W, ATLAS_EXT_H, ATLAS_EXT_W, atlas_ext_pixels);
    sceGuTexFlush();
    for (int i = 0; i < s_next; i++) {
        const Glyph *g = s_ext[i].g;
        int x = s_ext[i].x, y = s_ext[i].y;
        sprite(x + g->ox, y + g->oy, x + g->ox + g->w, y + g->oy + g->h, g->x, g->y, g->x + g->w, g->y + g->h, c);
    }
    flush();
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, s_atlas);
    sceGuTexFlush();
    s_next = 0;
}

/* Draws s up to end (or its terminator if end is NULL); returns the end x. */
static int draw_run(int font, int x, int y, const char *s, const char *end, u32 c)
{
    while (*s && (!end || s < end)) {
        int ext;
        const Glyph *g = glyph_of(font, &s, &ext);
        if (g->w) {
            if (ext) {
                if (s_next == EXT_BATCH)
                    ext_flush(c);
                s_ext[s_next].x = x;
                s_ext[s_next].y = y;
                s_ext[s_next].g = g;
                s_next++;
            } else {
                sprite(x + g->ox, y + g->oy, x + g->ox + g->w, y + g->oy + g->h,
                       g->x, g->y, g->x + g->w, g->y + g->h, c);
            }
        }
        x += g->adv;
    }
    ext_flush(c);
    return x;
}

int gfx_text(int font, int x, int y, const char *s, u32 c)
{
    return draw_run(font, x, y, s, NULL, c);
}

int gfx_text_n(int font, int x, int y, const char *s, int len, u32 c)
{
    return draw_run(font, x, y, s, s + len, c);
}

int gfx_text_width(int font, const char *s)
{
    int w = 0;
    while (*s)
        w += gfx_glyph_adv(font, &s);
    return w;
}

void gfx_text_center(int font, int cx, int y, const char *s, u32 c)
{
    gfx_text(font, cx - gfx_text_width(font, s) / 2, y, s, c);
}

void gfx_text_right(int font, int rx, int y, const char *s, u32 c)
{
    gfx_text(font, rx - gfx_text_width(font, s), y, s, c);
}

void gfx_text_fit(int font, int x, int y, int max_w, const char *s, u32 c)
{
    if (gfx_text_width(font, s) <= max_w) {
        gfx_text(font, x, y, s, c);
        return;
    }
    int dots = atlas_fonts[font].glyphs['.' - 32].adv * 3, w = 0;
    const char *cut = s;
    while (*cut) {
        const char *p = cut;
        int adv = gfx_glyph_adv(font, &p);
        if (w + adv + dots > max_w)
            break;
        w += adv;
        cut = p;
    }
    draw_run(font, x, y, s, cut, c);
    gfx_text(font, x + w, y, "...", c);
}
