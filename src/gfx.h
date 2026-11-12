/* XLT - sceGu renderer: one swizzled 4444 atlas, everything drawn as GU_SPRITES. */
#ifndef GFX_H
#define GFX_H

#include <psptypes.h>

#define SCREEN_W 480
#define SCREEN_H 272

/* 0xRRGGBB -> GE colour (0xAABBGGRR), opaque. */
#define RGB(h) (0xFF000000u | (((h) & 0xFFu) << 16) | ((h) & 0xFF00u) | (((h) >> 16) & 0xFFu))
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((u32)(a) << 24))

typedef struct { short x, y, w, h; } AtlasRect;
typedef struct { short x, y, w, h, ox, oy, adv; } Glyph;
typedef struct { const Glyph *glyphs; short line_h, ascent; } FontInfo;

#include "atlas_data.h"

/* eDRAM offset of the first byte not used by frame buffers or the atlas. */
#define GFX_VRAM_FREE 0x0EC000
/* After the 30 thumbnail slots: the picture viewer's 512x272 5650 image. */
#define GFX_VRAM_VIEWER (GFX_VRAM_FREE + 30 * 144 * 80 * 2)
#define GFX_VIEWER_BYTES (512 * 272 * 2)

void gfx_init(void);
void gfx_shutdown(void);

void gfx_begin(u32 clear_color);
void gfx_end(void); /* flush, wait for vblank, swap */

/* Global opacity (0..256) multiplied into every colour; used by screen fades. */
void gfx_set_opacity(int o);

void gfx_rect(int x, int y, int w, int h, u32 c);
void gfx_rrect(int x, int y, int w, int h, int r, u32 c);
/* 2px outline with radius-8 corners around (x,y,w,h). */
void gfx_ring(int x, int y, int w, int h, u32 c);
/* Rounds the corners of whatever is under (x,y,w,h) by painting `bg` outside radius r. */
void gfx_corners(int x, int y, int w, int h, int r, u32 bg);
/* Draws a 144x80 swizzled 5551 texture (a thumbnail) scaled into (x,y,w,h). */
void gfx_thumb(const void *tex, int x, int y, int w, int h);
/* Any texture: psm GU_PSM_*, tw/th power-of-two size, tbw buffer width in pixels;
 * draws texels (0,0)-(uw,vh) into (x,y,w,h). One texture switch. */
void gfx_texture(const void *tex, int psm, int tw, int th, int tbw, int swizzled,
                 int uw, int vh, int x, int y, int w, int h);
/* An RGBA 8888 picture in main RAM, any width, drawn so the GE stays fast. */
void gfx_video_frame(const void *base, int tbw, int fw, int fh, int dx, int dy, int dw, int dh);
/* How long the last frame's drawing kept the CPU waiting, in microseconds. */
u32 gfx_ge_wait_us(void);
/* Several tinted sprites from one other texture: bind it, draw, then bind the
 * atlas again before any gfx_rect/gfx_text. A texture in main RAM must have
 * been written back from the data cache. */
void gfx_bind(const void *tex, int psm, int tw, int th, int tbw, int swizzled);
void gfx_bind_atlas(void);
void gfx_sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 c);
void gfx_icon(int id, int x, int y, int w, int h, u32 c);

int gfx_text(int font, int x, int y, const char *s, u32 c); /* y = top of line; returns end x */
int gfx_text_n(int font, int x, int y, const char *s, int len, u32 c); /* the first len bytes */
int gfx_text_width(int font, const char *s);
/* Advance of the UTF-8 character at *s, which is moved past it. Pure: safe on any thread. */
int gfx_glyph_adv(int font, const char **s);
void gfx_text_center(int font, int cx, int y, const char *s, u32 c);
void gfx_text_right(int font, int rx, int y, const char *s, u32 c);
/* Draws s, cut with "..." if wider than max_w. */
void gfx_text_fit(int font, int x, int y, int max_w, const char *s, u32 c);
int gfx_line_h(int font);

u32 color_lerp(u32 a, u32 b, int t256);

#endif
