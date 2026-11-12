/* Renders the web browser on the PC: the real web.c, html.c, weblay.c, gfx.c through the
 * software GE of saver_preview.c, with the engine (webfetch.c) replaced by one that reads
 * a local HTML file and makes stand-in pictures. Writes PPM frames.
 *
 *   web_preview PAGE.html OUTDIR */
#define main saver_main
#include "saver_preview.c"
#undef main

#include <stdarg.h>
static unsigned long long s_time;
static unsigned long long sceKernelGetSystemTimeWide(void) { return s_time += 16000; }
static unsigned sceKernelTotalFreeMemSize(void) { return 20u << 20; }
#define PSP_CTRL_ANY 0





/* ------------------------------------------------------------ stand-ins */
#include "../src/webfetch.h"
#include "../src/net.h"
#include "../src/menu.h"
#include "../src/osk.h"
#include "../src/bg.h"

static const char *s_file;
static WebResult s_q[256];
static int s_qn;
static WebLayCfg s_wcfg;
static int s_fail_next;

static void q_push(WebResult r) { if (s_qn < 256) s_q[s_qn++] = r; }
int wf_poll(WebResult *o)
{
    if (!s_qn) return 0;
    *o = s_q[0];
    memmove(s_q, s_q + 1, --s_qn * sizeof(WebResult));
    return 1;
}
void wf_set_layout(const WebLayCfg *c) { s_wcfg = *c; }
void wf_init(const char *d) { (void)d; }
void wf_navigate(int gen, const char *url, const char *post, const char *ref)
{
    (void)post; (void)ref;
    WebPage *p = calloc(1, sizeof(WebPage));
    snprintf(p->url, sizeof(p->url), "%s", url);
    if (s_fail_next) {
        s_fail_next = 0;
        p->kind = WP_ERROR;
        snprintf(p->err, sizeof(p->err), "Couldn't find example.invalid");
    } else {
        FILE *f = fopen(s_file, "rb");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *b = malloc(n + 1);
        if (fread(b, 1, n, f) != (size_t)n) exit(1);
        fclose(f);
        p->kind = WP_HTML;
        p->doc = html_parse(b, (int)n);
        free(b);
        p->imgs = calloc(p->doc->nimgs + 1, sizeof(WebImgState));
        WebLayCfg c = s_wcfg;
        c.imgs = p->imgs;
        p->lay = weblay_run(p->doc, &c);
    }
    WebResult r = {0};
    r.kind = WR_PAGE;
    r.gen = gen;
    r.page = p;
    q_push(r);
}
void wf_image(int gen, int img, const char *url, int max_w, const char *ref)
{
    (void)url; (void)ref;
    int w = max_w < 220 ? max_w : 220, h = w * 9 / 16;
    if (w < 16) w = 16;
    if (h < 12) h = 12;
    int tbw = (w + 8) & ~7;
    unsigned short *t = calloc(tbw * (h + 1), 2);
    for (int y = 0; y <= h; y++)
        for (int x = 0; x < tbw; x++) { /* a sky-to-sea gradient with a sun */
            int dx = x - w * 2 / 3, dy = y - h / 3;
            int sun = dx * dx + dy * dy < (h / 6) * (h / 6);
            int r = sun ? 31 : 6 + 10 * y / (h + 1), g = sun ? 56 : 20 + 30 * y / (h + 1), b = sun ? 10 : 31 - 8 * y / (h + 1);
            t[y * tbw + x] = (unsigned short)((b << 11) | (g << 5) | r);
        }
    WebResult r = {0};
    r.kind = WR_IMG; r.gen = gen; r.img = img; r.ok = 1; r.tex = t; r.psm = GU_PSM_5650;
    r.tw = 256; r.th = 256; r.tbw = tbw; r.w = w; r.h = h;
    q_push(r);
}
void wf_relayout(int gen, const HtmlDoc *d, const WebImgState *imgs, int n)
{
    (void)n;
    WebLayCfg c = s_wcfg;
    c.imgs = imgs;
    WebResult r = {0};
    r.kind = WR_LAYOUT; r.gen = gen; r.lay = weblay_run(d, &c);
    q_push(r);
}
void wf_cancel(int gen) { (void)gen; }
void wf_free_page(WebPage *p) { if (!p) return; html_free(p->doc); weblay_free(p->lay); free(p->imgs); free(p); }
int wf_busy(void) { return 0; }
static int s_phase = WF_IDLE;
void wf_progress(int *ph, int64_t *g, int64_t *t) { *ph = s_phase; *g = 180 << 10; *t = 0; }
void wf_use_profile(int id) { (void)id; }
void wf_disconnect(void) {}
void wf_shutdown(void) {}
void wf_download(const char *u, const char *r, const char *p) { (void)u; (void)r; (void)p; }
void wf_download_stop(void) {}
int wf_download_progress(int64_t *g, int64_t *t, int *k) { *g = *t = 0; *k = 0; return 0; }
void library_rescan(void) {}

int net_profiles(int *ids, char (*names)[64], int max) { (void)max; ids[0] = 1; snprintf(names[0], 64, "Home"); return 1; }
int net_state(void) { return NET_JOINING; }
const char *net_step(void) { return ""; }
const char *net_profile_name(void) { return "Home"; }
void net_set_preferred(int id) { (void)id; }
int net_preferred(void) { return 1; }

void bg_post(void (*fn)(void *), const void *arg, int size) { (void)fn; (void)arg; (void)size; }
int library_scanning(void) { return 0; }
void power_boost(void) {}
void power_idle(void) {}
void sfx_play(int id) { (void)id; }
void ui_toast(const char *m) { printf("toast: %s\n", m); }
void menu_open(const char *t, const char *s, const MenuItem *it, int n, MenuFn fn) { (void)t; (void)s; (void)it; (void)n; (void)fn; }
static MenuItem s_mi[MENU_MAX];
MenuItem *menu_items(int *n) { *n = 0; return s_mi; }
void menu_set_count(int n) { (void)n; }
void menu_close(void) {}
void osk_open(const char *t, const char *i, int m, OskFn fn) { (void)i; (void)m; (void)fn; printf("osk: %s\n", t); }

#define HINT_GAP 16
#define HINT_ICON 12
void ui_draw_hints(const Hint *hints, int n, int cx, int y)
{
    int total = (n - 1) * HINT_GAP;
    for (int i = 0; i < n; i++)
        total += (hints[i].icon >= 0 ? HINT_ICON : gfx_text_width(FONT_SMALL, hints[i].key)) + 4 + gfx_text_width(FONT_SMALL, hints[i].label);
    int x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        const Hint *h = &hints[i];
        if (h->icon >= 0) { gfx_icon(h->icon, x, y + 3, HINT_ICON, HINT_ICON, h->tint); x += HINT_ICON + 4; }
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
    gfx_icon(ICON_WIFI, x - 52, 6, 14, 14, C_MUTED);
}

#include "../src/web.c"

/* ---------------------------------------------------------------- frames */

static SysStatus s_st = {21, 41, 76, 0, 1};

static void settle(int frames)
{
    for (int i = 0; i < frames; i++) web_step();
}

static void shot(const char *dir, const char *name)
{
    gfx_begin(g_th->bg);
    web_draw(&s_st);
    gfx_end();
    save(dir, name);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    s_file = argv[1];
    const char *dir = argv[2];
    dm = gfx_dither_matrix;
    sceGuTexMode(GU_PSM_4444, 0, 0, GU_TRUE);
    sceGuTexImage(0, ATLAS_W, ATLAS_H, ATLAS_W, NULL);
    theme_set(0);

    web_init("/nowhere");
    W.last_url[0] = 0;
    web_open(NULL);           /* nothing yet: the empty screen */
    settle(60);
    shot(dir, "0_empty");

    s_phase = WF_WIFI;
    go("https://en.m.wikipedia.org/wiki/PlayStation_Portable", NULL, 1);
    W.loading = 1;
    s_qn = 0;                 /* still "joining Wi-Fi" */
    settle(20);
    shot(dir, "1_wifi");
    s_phase = WF_RECEIVING;
    wf_navigate(W.gen, "https://en.m.wikipedia.org/wiki/PlayStation_Portable", NULL, NULL);
    settle(1);
    settle(60);
    shot(dir, "2_page_top");
    s_ntabs = 3; /* the tab count in the address bar */
    shot(dir, "2b_tabs");
    s_ntabs = 1;
    for (int i = 0; i < 6; i++) { web_input(PSP_CTRL_RIGHT); settle(30); }
    shot(dir, "3_scrolled");
    for (int i = 0; i < 4; i++) { web_input(PSP_CTRL_DOWN); settle(20); }
    shot(dir, "4_link_focus");
    for (int i = 0; i < 12; i++) { web_input(PSP_CTRL_RIGHT); settle(25); }
    for (int i = 0; i < 2; i++) { web_input(PSP_CTRL_DOWN); settle(20); }
    shot(dir, "5_further");
    s_fail_next = 1;
    go("https://example.invalid/", NULL, 1);
    settle(40);
    shot(dir, "6_error");
    printf("web_preview: %d spans, height %d\n", W.page && W.page->lay ? W.page->lay->nspans : 0,
           W.page && W.page->lay ? W.page->lay->height : 0);
    web_shutdown();
    return 0;
}
