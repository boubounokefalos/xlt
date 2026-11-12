/* Renders the screensaver on the PC: the real gfx.c and saverdraw.c, with the
 * GE replaced by a small software rasterizer (sprites, bilinear 4444/5551
 * textures, alpha blending, a 5650 frame buffer). Writes PPM frames.
 *
 *   saver_preview OUTDIR     -> PPM stills and an animation strip in OUTDIR
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.c"
#include "theme.c"
#include "saverdraw.c"

/* ---------------------------------------------------------------- GE stand-in */

static float fb[SCREEN_H][SCREEN_W][3]; /* what the 5650 buffer holds, expanded back to 0..1 */
static int alpha_test = 1; /* as gfx_init sets it: fragments with alpha 0 are not written */

/* One fragment, the way the GE writes it: blend in 8 bits, add the 4x4 dither
 * (-4..3), truncate to 5/6/5 bits. Every written fragment is dithered, even one
 * whose blend leaves the colour as it was. */
static const int dm_default[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
static const int (*dm)[4] = dm_default; /* gfx_init's matrix after main() sets it */

static void put(int x, int y, const float src[3], int a8)
{
    static const int bits[3] = {5, 6, 5};
    for (int k = 0; k < 3; k++) {
        int d8 = (int)(fb[y][x][k] * 255 + .5f), s8 = (int)(src[k] * 255 + .5f);
        int c = (s8 * a8 + d8 * (255 - a8)) / 255 + dm[y & 3][x & 3];
        c = c < 0 ? 0 : c > 255 ? 255 : c;
        int q = c >> (8 - bits[k]);
        int e = bits[k] == 5 ? (q << 3 | q >> 2) : (q << 2 | q >> 4);
        fb[y][x][k] = e / 255.f;
    }
}
static struct { const unsigned char *p; int psm, tbw, swz, w, h; } tex;
static int tcc_rgba = 1;

void sceGuTexMode(int psm, int maxmips, int a2, int swizzle) { tex.psm = psm; tex.swz = swizzle; }
void sceGuTexFunc(int tfx, int tcc) { tcc_rgba = tcc; }
void sceGuTexImage(int mip, int w, int h, int tbw, const void *tbp)
{
    tex.p = tbp ? tbp : atlas_pixels; /* s_atlas is NULL: gfx_init isn't run here */
    tex.w = w; tex.h = h; tex.tbw = tbw;
}
static unsigned clear_c;
void sceGuClearColor(unsigned c) { clear_c = c; }
void sceGuClear(int flags)
{
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            for (int k = 0; k < 3; k++)
            {
                int c = (clear_c >> (8 * k)) & 0xFF, q = k == 1 ? c >> 2 : c >> 3;
                fb[y][x][k] = (k == 1 ? (q << 2 | q >> 4) : (q << 3 | q >> 2)) / 255.f;
            }
}

static void texel(int x, int y, float out[4])
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tex.w) x = tex.w - 1;
    if (y >= tex.h) y = tex.h - 1;
    if (tex.psm == GU_PSM_8888) {
        const unsigned char *q = tex.p + (y * tex.tbw + x) * 4;
        for (int k = 0; k < 4; k++)
            out[k] = q[k] / 255.f;
        return;
    }
    int rowb = tex.tbw * 2, xb = x * 2, off;
    if (tex.swz)
        off = ((y / 8) * (rowb / 16) + xb / 16) * 128 + (y % 8) * 16 + xb % 16;
    else
        off = y * rowb + xb;
    unsigned v = tex.p[off] | tex.p[off + 1] << 8;
    if (tex.psm == GU_PSM_5650) { /* the web browser's pictures */
        out[0] = (v & 31) / 31.f;
        out[1] = ((v >> 5) & 63) / 63.f;
        out[2] = ((v >> 11) & 31) / 31.f;
        out[3] = 1;
        return;
    }
    if (tex.psm == GU_PSM_4444) {
        for (int k = 0; k < 4; k++)
            out[k] = ((v >> (4 * k)) & 15) / 15.f;
    } else { /* 5551 */
        out[0] = (v & 31) / 31.f;
        out[1] = ((v >> 5) & 31) / 31.f;
        out[2] = ((v >> 10) & 31) / 31.f;
        out[3] = (float)(v >> 15);
    }
}

static void sample(float u, float v, float out[4])
{
    u -= .5f; v -= .5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0, a[4], b[4], c[4], d[4];
    texel(x0, y0, a); texel(x0 + 1, y0, b); texel(x0, y0 + 1, c); texel(x0 + 1, y0 + 1, d);
    for (int k = 0; k < 4; k++)
        out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

void sceGuDrawArray(int prim, int vtype, int count, const void *ind, const void *vtx)
{
    const Vtx *vs = vtx;
    for (int i = 0; i + 1 < count; i += 2) {
        const Vtx *p = &vs[i], *q = &vs[i + 1];
        float vc[4];
        for (int k = 0; k < 4; k++)
            vc[k] = ((p->color >> (8 * k)) & 0xFF) / 255.f;
        for (int y = p->y < 0 ? 0 : p->y; y < q->y && y < SCREEN_H; y++)
            for (int x = p->x < 0 ? 0 : p->x; x < q->x && x < SCREEN_W; x++) {
                float u = p->u + (x + .5f - p->x) / (q->x - p->x) * (q->u - p->u);
                float v = p->v + (y + .5f - p->y) / (q->y - p->y) * (q->v - p->v);
                float t[4];
                sample(u, v, t);
                int ta8 = tcc_rgba ? (int)(t[3] * 255 + .5f) : 255;
                int a8 = ta8 * (int)(vc[3] * 255 + .5f) / 255;
                if (alpha_test && a8 == 0)
                    continue;
                float src[3] = {vc[0] * t[0], vc[1] * t[1], vc[2] * t[2]};
                put(x, y, src, a8);
            }
    }
}

/* ---------------------------------------------------------------- ui.c's battery */

void ui_draw_battery(int x, int y, const SysStatus *st)
{
    u32 c = st->charging ? C_CHARGE : st->battery < 15 ? C_LOW : C_MUTED;
    const int w = 20, h = 10;
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y + 1, 1, h - 2, c);
    gfx_rect(x + w - 1, y + 1, 1, h - 2, c);
    gfx_rect(x + w, y + 3, 2, h - 6, c);
    gfx_rect(x + 2, y + 2, (w - 4) * st->battery / 100, h - 4, c);
}

/* ---------------------------------------------------------------- output */

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            unsigned char px[3];
            for (int k = 0; k < 3; k++)
                px[k] = (unsigned char)(fb[y][x][k] * 255 + .5f);
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static void frame(const SaverView *v)
{
    gfx_begin(RGB(0x0d1117));
    saverdraw(v);
    gfx_end();
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    dm = gfx_dither_matrix;
    if (argc > 2 && !strcmp(argv[2], "old")) { /* how it was before: shows the double-dither rectangles */
        alpha_test = 0;
        dm = dm_default;
    }
    saverdraw_init();
    /* what gfx_init leaves bound */
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    SaverView v = {0};
    v.st = (SysStatus){.hour = 21, .minute = 41, .battery = 76, .charging = 0};
    v.wday = 5; v.day = 6; v.month = 11; v.year = 2026;
    if (getenv("XLT_DEMO")) { /* 4 s of the screensaver with music playing, every other frame saved */
        char name[16];
        v.date_fmt = SAVER_DMY;
        v.music = "One Long Evening" DOT "Mira Ostend";
        for (int f = 0, n = 0; f < 240; f++) {
            v.frame = 20000 + f;
            v.show = f < 30 ? f * 256 / 30 : 256;
            if (f & 1) continue;
            frame(&v);
            snprintf(name, sizeof(name), "f%05d", n++);
            save(getenv("XLT_DEMO"), name);
        }
        return 0;
    }
    v.date_fmt = SAVER_DMY;
    v.show = 256;
    v.hint = 256;
    v.frame = 12345;
    frame(&v);
    save(dir, "a_24h_hint");

    v.hint = 0;
    v.music = "One Long Evening" DOT "Mira Ostend";
    frame(&v);
    save(dir, "b_music");

    v.music_paused = 1;
    v.h12 = 1;
    v.date_fmt = SAVER_MDY;
    v.st.charging = 1;
    v.st.hour = 9; v.st.minute = 5;
    frame(&v);
    save(dir, "c_12h_paused_charging");

    v = (SaverView){0};
    v.st = (SysStatus){.hour = 0, .minute = 0, .battery = 9};
    v.wday = 3; v.day = 31; v.month = 12; v.date_fmt = SAVER_YMD; v.show = 256; v.frame = 99999;
    v.music = "A very long song title that will not fit in the pill at all" DOT "Some Artist";
    frame(&v);
    save(dir, "d_long_lowbatt");

    /* fading in and out, over a stand-in screen */
    v.music = NULL;
    v.st = (SysStatus){.hour = 21, .minute = 41, .battery = 76};
    for (int i = 0; i < 2; i++) {
        v.show = 128;
        v.leaving = i;
        v.hint = i ? 0 : 128;
        gfx_begin(RGB(0x0d1117));
        gfx_rrect(40, 60, 400, 120, 10, C_RAISED);
        gfx_text(FONT_BOLD, 60, 80, "(the screen underneath)", C_TEXT);
        saverdraw(&v);
        gfx_end();
        save(dir, i ? "e_fade_out" : "e_fade_in");
    }

    /* the animation: one frame every 2 s for 2 minutes */
    v = (SaverView){0};
    v.st = (SysStatus){.hour = 21, .minute = 41, .battery = 76};
    v.wday = 5; v.day = 6; v.month = 11; v.show = 256;
    for (int i = 0; i < 60; i++) {
        char name[32];
        v.frame = 1000 + i * 120 + (i & 1) * 60; /* odd frames land on the colon's dim half */
        frame(&v);
        snprintf(name, sizeof(name), "anim_%02d", i);
        save(dir, name);
    }
    return 0;
}
