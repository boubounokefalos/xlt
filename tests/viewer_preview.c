/* Renders the picture viewer on the PC: the real viewer.c and imgdecode.c
 * (gfx.c through the software GE of saver_preview.c) on the test pictures:
 * fitted, zoomed (the sharp tiled copy), moved, turned, the slideshow.
 * Writes PPM frames.
 *
 *   viewer_preview PICTURES_DIR OUTDIR */
#define main saver_main
#include "saver_preview.c"
#undef main

static unsigned short s_vram[512 * 512];
#define VIEWER_VRAM() s_vram
#define VIEWER_VRAM_UNCACHED() s_vram
#include "../src/viewer.c"

/* ------------------------------------------------------------ stand-ins */
static unsigned s_ms;
unsigned sceKernelGetSystemTimeLow(void) { return s_ms * 1000u; }
int scePowerTick(int t) { (void)t; return 0; }
void bg_post(void (*fn)(void *), const void *arg, int size) { (void)size; fn((void *)arg); } /* at once */
void power_boost(void) {}
void power_idle(void) {}
int library_scanning(void) { return 0; }
int library_count(int v) { (void)v; return 0; }
const Entry *library_get(int v, int i) { (void)v; (void)i; return NULL; }
int library_find(int v, const char *p) { (void)v; (void)p; return -1; }
void opts_open(const Entry *e) { (void)e; }
void sfx_play(int id) { (void)id; }
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * 16;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? 12 : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    if (total > SCREEN_W - 16) printf("hints too wide: %d px\n", total);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, 12, 12, btn_tint(h->tint)); x += 16; }
        else x = gfx_text(FONT_SMALL, x, y, h->key, C_TEXT) + 4;
        x = gfx_text(FONT_SMALL, x, y, h->label, C_MUTED) + 16;
    }
}

static char s_pics[2][256];
static int src_count(void) { return 2; }
static int src_get(int i, Entry *e)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", s_pics[i]);
    const char *b = strrchr(s_pics[i], '/');
    snprintf(e->title, sizeof(e->title), "%s", b ? b + 1 : s_pics[i]);
    e->kind = KIND_PICTURE;
    return 0;
}
static const ViewerSource s_src = {src_count, src_get};

static void frames(int n) { for (int i = 0; i < n; i++) { s_ms += 16; viewer_step(); } }
static void shot(const char *dir, const char *name)
{
    frames(30);
    gfx_begin(RGB(0x000000));
    viewer_draw();
    gfx_end();
    save(dir, name);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(0);
    snprintf(s_pics[0], sizeof(s_pics[0]), "%s/photo.jpg", argv[1]); /* 1200x700 */
    snprintf(s_pics[1], sizeof(s_pics[1]), "%s/pic.png", argv[1]);   /* 300x200 */
    snprintf(s_rot_path, sizeof(s_rot_path), "/nonexistent");
    viewer_open_source(&s_src, 0);
    shot(argv[2], "pv_1_fit");
    viewer_input(PSP_CTRL_RTRIGGER);
    viewer_input(PSP_CTRL_RTRIGGER);          /* 225%: the sharp copy, in tiles */
    shot(argv[2], "pv_2_zoomed");
    viewer_hold(PSP_CTRL_RIGHT | PSP_CTRL_DOWN);
    frames(400);                              /* held long enough to reach the corner */
    viewer_hold(0);
    shot(argv[2], "pv_3_moved_to_corner");
    viewer_input(PSP_CTRL_CIRCLE);            /* back to fitted */
    viewer_input(PSP_CTRL_SQUARE);            /* a quarter turn */
    shot(argv[2], "pv_4_turned");
    viewer_input(PSP_CTRL_SQUARE);
    viewer_input(PSP_CTRL_SQUARE);
    viewer_input(PSP_CTRL_SQUARE);            /* upright again (forgotten) */

    if (pm_get(&s_rot, s_pics[0]) != 0) { puts("rotation not forgotten"); return 1; }
    viewer_input(PSP_CTRL_START);             /* slideshow: the next picture after 5 s */
    frames(5200 / 16);
    shot(argv[2], "pv_5_slideshow_next");
    puts("viewer_preview: ok");
    return 0;
}
