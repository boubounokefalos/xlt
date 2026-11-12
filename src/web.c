/* XLT - the web browser screen. See web.h.
 *
 * The page stays on screen while the next one loads (a thin bar under the
 * address shows the progress); when it arrives it replaces the old one in
 * one frame. Pictures are fetched after the page, in reading order, within
 * a memory budget; a picture whose size the page didn't give makes the page
 * be laid out again (on the worker), keeping what you are reading in place. */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "audiofmt.h"
#include "web.h"
#include "webfetch.h"
#include "ui_internal.h"
#include "anim.h"
#include "bg.h"
#include "library.h"
#include "menu.h"
#include "net.h"
#include "osk.h"
#include "sfx.h"
#include "url.h"
#include "webicon_data.h"

#define BAR_Y 26
#define BAR_H 22
#define FOOTER_Y 248
#define CONTENT_X 10
#define CONTENT_W (SCREEN_W - 2 * CONTENT_X - 4)
#define HIST_MAX 40
#define IMG_INFLIGHT 3
#define SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="
#define RELAYOUT_MS 350

typedef struct {
    char url[URL_MAX];
    char title[96];
    int scroll;
} Hist;

typedef struct {
    void *tex;
    int psm, tw, th, tbw, w, h;
    int requested;
} Tex;

static struct {
    int open;
    Spring t;
    int gen;
    WebPage *page;
    int loading;            /* a page is on its way */
    char loading_url[URL_MAX];
    int load_from_hist;     /* the scroll of hist[cur] comes back when it arrives */
    char want_frag[160];    /* jump there when it arrives */
    Hist hist[HIST_MAX];
    int nhist, cur;

    Spring scroll;          /* px, Q8 */
    Spring bar;             /* the address bar: FX(1) shown, 0 tucked away */
    int focus;              /* in page->lay->focus, -1 */

    Tex *tex;
    int img_next, img_inflight;
    int img_mem, img_budget;
    int relayout_pending, relayout_inflight;
    unsigned relayout_at;

    char **vals;            /* per control: the text typed, or NULL for the page's own */
    unsigned char *chk;
    short *sel;
    int edit_ctrl;          /* the control the keyboard is for */
    int menu_ctrl;          /* the select the menu is for */

    int small_text, pictures; /* options */
    int boosted;
    int anim;               /* frames, for the progress bar */
    char last_url[URL_MAX], last_title[96];
    int profile_ids[NET_PROFILES];
    char profile_names[NET_PROFILES][64];
    int nprofiles;
} W;

static char s_cfg_path[256];

/* ------------------------------------------------------------- settings */

typedef struct { int small_text, pictures, profile; char url[URL_MAX]; char title[96]; } SavedCfg;

static void job_save(void *arg)
{
    const SavedCfg *c = arg;
    SceUID fd = sceIoOpen(s_cfg_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    char b[URL_MAX + 256];
    int n = snprintf(b, sizeof(b), "small_text=%d\npictures=%d\nwifi=%d\nlast_url=%s\nlast_title=%s\n",
                     c->small_text, c->pictures, c->profile, c->url, c->title);
    sceIoWrite(fd, b, n);
    sceIoClose(fd);
}

static void save_cfg(void)
{
    SavedCfg c;
    c.small_text = W.small_text;
    c.pictures = W.pictures;
    c.profile = net_preferred();
    snprintf(c.url, sizeof(c.url), "%s", W.last_url);
    snprintf(c.title, sizeof(c.title), "%s", W.last_title);
    bg_post(job_save, &c, sizeof(c));
}

static void load_cfg(void)
{
    W.pictures = 1;
    SceUID fd = sceIoOpen(s_cfg_path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    char b[URL_MAX + 256];
    int n = sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *line = b; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "small_text=", 11)) W.small_text = atoi(line + 11) != 0;
        else if (!strncmp(line, "pictures=", 9)) W.pictures = atoi(line + 9) != 0;
        else if (!strncmp(line, "wifi=", 5)) net_set_preferred(atoi(line + 5));
        else if (!strncmp(line, "last_url=", 9)) snprintf(W.last_url, sizeof(W.last_url), "%s", line + 9);
        else if (!strncmp(line, "last_title=", 11)) snprintf(W.last_title, sizeof(W.last_title), "%s", line + 11);
        line = nl ? nl + 1 : NULL;
    }
}

/* --------------------------------------------------------------- layout */

static WebLayCfg s_lay;

static int line_gap(void) { return 3; }

static void set_layout(void)
{
    memset(&s_lay, 0, sizeof(s_lay));
    s_lay.width = CONTENT_W;
    s_lay.font_body = W.small_text ? FONT_SMALL : FONT_BODY;
    s_lay.font_bold = FONT_BOLD;
    s_lay.font_small = FONT_SMALL;
    for (int f = 0; f < FONT_COUNT && f < 8; f++)
        s_lay.line_h[f] = gfx_line_h(f) + line_gap();
    s_lay.adv = gfx_glyph_adv;
    wf_set_layout(&s_lay);
}

static int view_top(void) { return BAR_Y + (BAR_H + 4) * W.bar.x / 256; }
static int view_h_full(void) { return FOOTER_Y - BAR_Y; } /* without the bar */

static WebLayout *lay(void) { return W.page ? W.page->lay : NULL; }

static int max_scroll(void)
{
    WebLayout *l = lay();
    if (!l) return 0;
    int m = l->height - (FOOTER_Y - BAR_Y);
    return m > 0 ? m : 0;
}

static int scroll_target(void) { return W.scroll.target >> 8; }

static void scroll_to(int y, int snap)
{
    if (y > max_scroll()) y = max_scroll();
    if (y < 0) y = 0;
    if (snap) spring_snap(&W.scroll, FX(y));
    else spring_to(&W.scroll, FX(y));
    spring_to(&W.bar, y <= 0 || W.loading ? FX(1) : 0);
}

/* ------------------------------------------------------------- the page */

static void free_page_state(void)
{
    if (W.tex) {
        for (int i = 0; W.page && W.page->doc && i < W.page->doc->nimgs; i++)
            free(W.tex[i].tex);
        free(W.tex);
        W.tex = NULL;
    }
    if (W.vals) {
        for (int i = 0; W.page && W.page->doc && i < W.page->doc->nctrls; i++)
            free(W.vals[i]);
        free(W.vals);
        W.vals = NULL;
    }
    free(W.chk);
    W.chk = NULL;
    free(W.sel);
    W.sel = NULL;
    if (W.page)
        wf_free_page(W.page);
    W.page = NULL;
    W.focus = -1;
    W.img_next = W.img_inflight = W.img_mem = 0;
    W.relayout_pending = W.relayout_inflight = 0;
}

static const char *page_base(char *out, int n)
{
    if (!W.page) return "";
    HtmlDoc *d = W.page->doc;
    if (d && d->base >= 0 && url_resolve(W.page->url, HTML_STR(d, d->base), out, n) == 0)
        return out;
    return W.page->url;
}

static const char *ctrl_value(int c)
{
    HtmlDoc *d = W.page->doc;
    if (W.vals && W.vals[c]) return W.vals[c];
    return HTML_STR(d, d->ctrls[c].value);
}

static int s_redirects;       /* meta refreshes followed in a row */
static int s_following;       /* the navigation under way is one of them */
static void go(const char *url, const char *post, int push);
static int s_ntabs;
static void tab_close(void);
static void close_browser(void);
static void remember_visit(const char *url, const char *title);

static void adopt_page(WebPage *p)
{
    free_page_state();
    W.page = p;
    W.loading = 0;
    W.focus = -1;
    HtmlDoc *d = p->doc;
    if (d) {
        W.tex = calloc(d->nimgs > 0 ? d->nimgs : 1, sizeof(Tex));
        W.vals = calloc(d->nctrls > 0 ? d->nctrls : 1, sizeof(char *));
        W.chk = calloc(d->nctrls > 0 ? d->nctrls : 1, 1);
        W.sel = calloc(d->nctrls > 0 ? d->nctrls : 1, sizeof(short));
        for (int i = 0; i < d->nctrls; i++) {
            W.chk[i] = (unsigned char)d->ctrls[i].checked;
            W.sel[i] = (short)d->ctrls[i].sel;
        }
        for (int i = 0; i < d->nimgs; i++) {
            const char *src = HTML_STR(d, d->imgs[i].src);
            if (!W.pictures || !src[0]) p->imgs[i].state = WI_NONE;
        }
        W.img_budget = 4 << 20; /* of the 18 MB heap (main.c); the free memory the kernel reports is outside it */
    }
    /* the history entry is this page now */
    Hist *h = &W.hist[W.cur];
    snprintf(h->url, sizeof(h->url), "%s", p->url);
    if (d && d->title[0]) snprintf(h->title, sizeof(h->title), "%s", d->title);
    else url_pretty(p->url, h->title, sizeof(h->title));
    int y = 0;
    if (W.load_from_hist) y = h->scroll;
    if (W.want_frag[0] && d && p->lay) {
        for (int i = 0; i < d->nanchors; i++)
            if (!strcmp(HTML_STR(d, d->anchors[i].name), W.want_frag)) { y = p->lay->anchor_y[i] - 4; break; }
    }
    W.want_frag[0] = 0;
    W.load_from_hist = 0;
    spring_snap(&W.bar, FX(1));
    scroll_to(y, 1);
    /* <meta http-equiv="refresh">: a redirect page (search click-throughs, moved pages) */
    if (d && d->refresh >= 0 && d->refresh_sec <= 5 && s_redirects < 5) {
        char to[URL_MAX];
        if (url_resolve(p->url, HTML_STR(d, d->refresh), to, sizeof(to)) == 0 && !url_same_page(to, p->url)) {
            s_redirects++;
            s_following = 1;
            go(to, NULL, 0); /* in place of this entry: Back skips the redirect page */
            return;
        }
    }
    if (p->kind != WP_ERROR) {
        snprintf(W.last_url, sizeof(W.last_url), "%s", p->url);
        snprintf(W.last_title, sizeof(W.last_title), "%s", h->title);
        save_cfg();
        if (p->kind != WP_FILE) remember_visit(p->url, h->title);
    }
}

static void go(const char *url, const char *post, int push)
{
    char unwrapped[URL_MAX];
    if (!post && url_unwrap_redirect(url, unwrapped, sizeof(unwrapped)))
        url = unwrapped; /* a search result's click-through: straight to the site */
    if (!s_following) s_redirects = 0;
    s_following = 0;
    char sc[16];
    url_scheme(url, sc, sizeof(sc));
    if (!strcmp(sc, "javascript")) { ui_toast("This link needs JavaScript"); sfx_play(SFX_ERROR); return; }
    if (!strcmp(sc, "mailto")) { ui_toast("E-mail links can't be opened on the PSP"); sfx_play(SFX_ERROR); return; }
    if (!strcmp(sc, "tel") || !strcmp(sc, "sms")) { ui_toast("Phone links can't be opened"); sfx_play(SFX_ERROR); return; }
    if (!url_is_web(url)) { ui_toast("This kind of address can't be opened"); sfx_play(SFX_ERROR); return; }

    /* a jump within the page that is shown */
    if (!post && W.page && W.page->doc && url_same_page(url, W.page->url) && url_fragment(url)[0] && !W.loading) {
        const char *frag = url_fragment(url);
        HtmlDoc *d = W.page->doc;
        for (int i = 0; i < d->nanchors; i++) {
            if (!strcmp(HTML_STR(d, d->anchors[i].name), frag)) {
                if (push && W.nhist < HIST_MAX) { /* back returns to here */
                    W.hist[W.cur].scroll = scroll_target();
                    W.cur++;
                    W.nhist = W.cur + 1;
                    W.hist[W.cur] = W.hist[W.cur - 1];
                    snprintf(W.hist[W.cur].url, sizeof(W.hist[W.cur].url), "%s", url);
                }
                W.focus = -1;
                scroll_to(W.page->lay->anchor_y[i] - 4, 0);
                return;
            }
        }
    }

    const char *referer = W.page ? W.page->url : NULL;
    if (push && W.nhist > 0) /* (history_go and reload keep the scroll themselves) */
        W.hist[W.cur].scroll = scroll_target();
    if (push) {
        W.load_from_hist = 0;
        if (W.nhist == 0) {
            W.cur = 0;
        } else if (W.cur + 1 >= HIST_MAX) { /* full: forget the oldest */
            memmove(&W.hist[0], &W.hist[1], sizeof(Hist) * (HIST_MAX - 1));
        } else {
            W.cur++;
        }
        W.nhist = W.cur + 1;
        snprintf(W.hist[W.cur].url, sizeof(W.hist[W.cur].url), "%s", url);
        url_pretty(url, W.hist[W.cur].title, sizeof(W.hist[W.cur].title));
        W.hist[W.cur].scroll = 0;
    }
    snprintf(W.want_frag, sizeof(W.want_frag), "%s", url_fragment(url));
    W.gen++;
    wf_cancel(W.gen);
    W.img_inflight = 0; /* those answers will come with an old generation */
    W.relayout_inflight = 0;
    W.loading = 1;
    snprintf(W.loading_url, sizeof(W.loading_url), "%s", url);
    char no_frag[URL_MAX];
    snprintf(no_frag, sizeof(no_frag), "%s", url);
    char *hash = strchr(no_frag, '#');
    if (hash) *hash = 0;
    wf_navigate(W.gen, no_frag, post, referer);
    spring_to(&W.bar, FX(1));
}

static void go_typed(const char *text)
{
    if (!text) {
        if (!W.page && !W.loading) { /* cancelled with nothing open: that tab (or the browser) goes */
            if (s_ntabs > 1) tab_close();
            else close_browser();
        }
        return;
    }
    char url[URL_MAX];
    if (url_from_input(text, SEARCH_PREFIX, url, sizeof(url)) < 0)
        return;
    go(url, NULL, 1);
}

static void open_address(int empty)
{
    const char *cur = !empty && W.page && W.page->kind != WP_ERROR ? W.page->url : "";
    osk_open("Search or type an address", cur, OSK_URL, go_typed);
}

static void history_go(int d)
{
    int to = W.cur + d;
    if (to < 0 || to >= W.nhist) return;
    W.hist[W.cur].scroll = scroll_target();
    W.cur = to;
    W.load_from_hist = 1;
    go(W.hist[W.cur].url, NULL, 0);
    W.load_from_hist = 1; /* go() doesn't touch it, but keep the intent obvious */
}

static void reload(void)
{
    if (W.nhist == 0) return;
    W.hist[W.cur].scroll = scroll_target();
    W.load_from_hist = 1;
    go(W.hist[W.cur].url, NULL, 0);
}

static void stop_loading(void)
{
    if (!W.loading) return;
    W.gen++;
    wf_cancel(W.gen);
    W.loading = 0;
    W.img_inflight = 0;
    if (W.page && W.nhist > 0) { /* still showing the old page: the history goes back to it */
        for (int i = 0; i < W.nhist; i++)
            if (!strcmp(W.hist[i].url, W.page->url)) W.cur = i;
    }
    ui_toast("Stopped");
}

/* ------------------------------------------------------------- forms */

static void submit(int form, int button)
{
    HtmlDoc *d = W.page->doc;
    char body[4096] = "";
    for (int i = 0; i < d->nctrls; i++) {
        HtmlCtrl *c = &d->ctrls[i];
        if (c->form != form) continue;
        const char *name = HTML_STR(d, c->name);
        if (!name[0]) continue;
        const char *v = NULL;
        switch (c->type) {
        case HC_TEXT: case HC_PASSWORD: case HC_TEXTAREA: case HC_HIDDEN: v = ctrl_value(i); break;
        case HC_CHECKBOX: case HC_RADIO: if (W.chk[i]) v = HTML_STR(d, c->value); break;
        case HC_SELECT:
            if (W.sel[i] >= 0 && W.sel[i] < c->opt_n) v = HTML_STR(d, d->opts[c->opt_first + W.sel[i]].value);
            break;
        case HC_SUBMIT: if (i == button) v = HTML_STR(d, c->value); break;
        }
        if (!v) continue;
        if (body[0]) strncat(body, "&", sizeof(body) - strlen(body) - 1);
        url_encode_append(body, sizeof(body), name);
        strncat(body, "=", sizeof(body) - strlen(body) - 1);
        url_encode_append(body, sizeof(body), v);
    }
    if (button >= 0 && d->ctrls[button].type == HC_IMAGE) { /* where on the picture: the middle */
        const char *name = HTML_STR(d, d->ctrls[button].name);
        char xy[200];
        snprintf(xy, sizeof(xy), "%s%s%s.x=1&%s%s.y=1", body[0] ? "&" : "", name, name[0] ? "" : "", name,
                 name[0] ? "" : "");
        if (name[0]) strncat(body, xy, sizeof(body) - strlen(body) - 1);
    }
    char base[URL_MAX], action[URL_MAX];
    const char *b = page_base(base, sizeof(base));
    const char *act = form >= 0 ? HTML_STR(d, d->forms[form].action) : "";
    if (url_resolve(b, act, action, sizeof(action)) < 0) return;
    int post = form >= 0 && d->forms[form].post;
    if (post) {
        go(action, body, 1);
    } else {
        char *q = strchr(action, '?');
        if (q) *q = 0;
        char *h = strchr(action, '#');
        if (h) *h = 0;
        char full[URL_MAX];
        snprintf(full, sizeof(full), "%s?%s", action, body);
        go(full, NULL, 1);
    }
}

static int text_fields_in_form(int form)
{
    HtmlDoc *d = W.page->doc;
    int n = 0;
    for (int i = 0; i < d->nctrls; i++) {
        int t = d->ctrls[i].type;
        if (d->ctrls[i].form == form && (t == HC_TEXT || t == HC_PASSWORD || t == HC_TEXTAREA)) n++;
    }
    return n;
}

static void on_field(const char *text)
{
    int c = W.edit_ctrl;
    W.edit_ctrl = -1;
    if (!text || !W.page || !W.page->doc || c < 0 || c >= W.page->doc->nctrls) return;
    free(W.vals[c]);
    W.vals[c] = strdup(text);
    HtmlCtrl *k = &W.page->doc->ctrls[c];
    /* a search box (the only field of its form): typing it is asking for it */
    if (k->type != HC_TEXTAREA && text[0] && text_fields_in_form(k->form) == 1)
        submit(k->form, -1);
}

static void select_cb(int id, int dir)
{
    (void)dir;
    int c = W.menu_ctrl;
    if (W.page && W.page->doc && c >= 0 && c < W.page->doc->nctrls)
        W.sel[c] = (short)id;
    menu_close();
}

static void use_ctrl(int c)
{
    HtmlDoc *d = W.page->doc;
    HtmlCtrl *k = &d->ctrls[c];
    switch (k->type) {
    case HC_TEXT: case HC_PASSWORD: case HC_TEXTAREA: {
        W.edit_ctrl = c;
        const char *lab = HTML_STR(d, k->label);
        const char *nm = HTML_STR(d, k->name);
        osk_open(lab[0] ? lab : nm[0] ? nm : "Text", ctrl_value(c), OSK_TEXT, on_field);
        break;
    }
    case HC_CHECKBOX:
        W.chk[c] = !W.chk[c];
        sfx_play(W.chk[c] ? SFX_ON : SFX_OFF);
        break;
    case HC_RADIO:
        for (int i = 0; i < d->nctrls; i++)
            if (d->ctrls[i].type == HC_RADIO && d->ctrls[i].form == k->form &&
                !strcmp(HTML_STR(d, d->ctrls[i].name), HTML_STR(d, k->name)))
                W.chk[i] = 0;
        W.chk[c] = 1;
        sfx_play(SFX_ON);
        break;
    case HC_SELECT: {
        static MenuItem it[MENU_MAX];
        int n = 0;
        for (int i = 0; i < k->opt_n && n < MENU_MAX; i++) {
            memset(&it[n], 0, sizeof(it[n]));
            snprintf(it[n].label, sizeof(it[n].label), "%s", HTML_STR(d, d->opts[k->opt_first + i].label));
            if (i == W.sel[c]) snprintf(it[n].value, sizeof(it[n].value), "\xE2\x80\xA2");
            it[n].id = i;
            n++;
        }
        W.menu_ctrl = c;
        menu_open("Choose", NULL, it, n, select_cb);
        break;
    }
    case HC_SUBMIT: case HC_IMAGE:
        submit(k->form, c);
        break;
    default:
        ui_toast("This button needs JavaScript");
        sfx_play(SFX_ERROR);
        break;
    }
}

/* ------------------------------------------------------------- focus */

static int focus_visible(int f, int top, int bottom)
{
    WebFocus *fc = &lay()->focus[f];
    return fc->y0 >= top && fc->y1 <= bottom;
}

static void show_focus(int f)
{
    WebFocus *fc = &lay()->focus[f];
    int top = scroll_target(), h = view_h_full() - 6;
    if (fc->y1 - fc->y0 > h || fc->y0 < top) scroll_to(fc->y0 - 6, 0);
    else if (fc->y1 > top + h) scroll_to(fc->y1 - h, 0);
}

static void move_focus(int dir)
{
    WebLayout *l = lay();
    if (!l) return;
    int top = scroll_target(), bottom = top + view_h_full() - 4;
    int step = view_h_full() * 2 / 3;
    int f = W.focus;
    if (f < 0 || f >= l->nfocus || !focus_visible(f, top - 2, bottom + 2)) {
        /* nothing focused on screen: the first (or last) one that is */
        int pick = -1;
        if (dir > 0) {
            for (int i = 0; i < l->nfocus; i++)
                if (focus_visible(i, top, bottom)) { pick = i; break; }
        } else {
            for (int i = l->nfocus - 1; i >= 0; i--)
                if (focus_visible(i, top, bottom)) { pick = i; break; }
        }
        if (pick >= 0) {
            W.focus = pick;
            sfx_play(SFX_MOVE);
        } else {
            scroll_to(top + dir * step, 0);
            W.focus = -1;
        }
        return;
    }
    int next = f + dir;
    if (next >= 0 && next < l->nfocus) {
        WebFocus *n = &l->focus[next];
        int near = dir > 0 ? n->y0 < bottom + 40 : n->y1 > top - 40;
        if (near) {
            W.focus = next;
            show_focus(next);
            sfx_play(SFX_MOVE);
            return;
        }
    }
    /* the next one is far away (or there is none): scroll a bit */
    int to = top + dir * step;
    if ((dir > 0 && top >= max_scroll()) || (dir < 0 && top <= 0))
        return;
    scroll_to(to, 0);
    int ntop = scroll_target(), nbottom = ntop + view_h_full() - 4;
    if (!focus_visible(f, ntop, nbottom)) W.focus = -1;
}

static void page_by(int dir)
{
    int top = scroll_target();
    scroll_to(top + dir * (view_h_full() - 36), 0);
    WebLayout *l = lay();
    if (l && W.focus >= 0 && !focus_visible(W.focus, scroll_target(), scroll_target() + view_h_full()))
        W.focus = -1;
}

static void activate(void)
{
    WebLayout *l = lay();
    if (!l || W.focus < 0 || W.focus >= l->nfocus) {
        if (W.page && W.page->kind == WP_ERROR) reload();
        return;
    }
    WebFocus *f = &l->focus[W.focus];
    if (f->ctrl >= 0) {
        use_ctrl(f->ctrl);
        return;
    }
    HtmlDoc *d = W.page->doc;
    char base[URL_MAX], to[URL_MAX];
    const char *b = page_base(base, sizeof(base));
    if (url_resolve(b, HTML_STR(d, d->links[f->link].href), to, sizeof(to)) < 0) {
        ui_toast("This link is too long");
        return;
    }
    go(to, NULL, 1);
}

/* ---------------------------------------------------- bookmarks, history */

#define BM_MAX 40
#define VH_MAX 30

typedef struct { char url[URL_MAX]; char title[96]; } Place;
static Place s_bm[BM_MAX], s_vh[VH_MAX];
static int s_nbm, s_nvh;
static char s_bm_path[256], s_vh_path[256];

typedef struct { char path[256]; char *buf; int len; } SaveJob;

static void job_write_list(void *arg)
{
    SaveJob *j = arg;
    SceUID fd = sceIoOpen(j->path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, j->buf, j->len);
        sceIoClose(fd);
    }
    free(j->buf);
}

/* One line per place: url, a tab, the title. */
static void save_list(const char *path, const Place *p, int n)
{
    SaveJob j;
    snprintf(j.path, sizeof(j.path), "%s", path);
    j.buf = malloc((size_t)n * (URL_MAX + 100) + 1);
    if (!j.buf) return;
    j.len = 0;
    for (int i = 0; i < n; i++)
        j.len += sprintf(j.buf + j.len, "%s\t%s\n", p[i].url, p[i].title);
    bg_post(job_write_list, &j, sizeof(j));
}

static int load_list(const char *path, Place *p, int max)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int cap = max * (URL_MAX + 100);
    char *b = malloc(cap + 1);
    int n = 0;
    if (b) {
        int len = sceIoRead(fd, b, cap);
        if (len > 0) {
            b[len] = 0;
            for (char *line = b; line && *line && n < max;) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                char *tab = strchr(line, '\t');
                if (tab) {
                    *tab = 0;
                    if (url_is_web(line)) {
                        snprintf(p[n].url, sizeof(p[n].url), "%s", line);
                        snprintf(p[n].title, sizeof(p[n].title), "%s", tab + 1);
                        n++;
                    }
                }
                line = nl ? nl + 1 : NULL;
            }
        }
        free(b);
    }
    sceIoClose(fd);
    return n;
}

static int place_find(const Place *p, int n, const char *url)
{
    for (int i = 0; i < n; i++)
        if (url_same_page(p[i].url, url)) return i;
    return -1;
}

/* Puts url first in the list (moving it up if it is there), dropping the last if full. */
static void place_front(Place *p, int *n, int max, const char *url, const char *title)
{
    int at = place_find(p, *n, url);
    if (at < 0) at = *n < max ? (*n)++ : max - 1;
    memmove(&p[1], &p[0], sizeof(Place) * at);
    snprintf(p[0].url, sizeof(p[0].url), "%s", url);
    snprintf(p[0].title, sizeof(p[0].title), "%s", title);
}

static void place_remove(Place *p, int *n, int at)
{
    if (at < 0 || at >= *n) return;
    memmove(&p[at], &p[at + 1], sizeof(Place) * (*n - at - 1));
    (*n)--;
}

static void lists_init(const char *app_dir)
{
    snprintf(s_bm_path, sizeof(s_bm_path), "%.200scache/bookmarks.txt", app_dir);
    snprintf(s_vh_path, sizeof(s_vh_path), "%.200scache/webhist.txt", app_dir);
    s_nbm = load_list(s_bm_path, s_bm, BM_MAX);
    if (s_nbm < 0) { /* the first time: the places the Web row always had */
        s_nbm = 0;
        place_front(s_bm, &s_nbm, BM_MAX, "http://68k.news/", "News (68k.news)");
        place_front(s_bm, &s_nbm, BM_MAX, "https://en.m.wikipedia.org/", "Wikipedia");
    }
    s_nvh = load_list(s_vh_path, s_vh, VH_MAX);
    if (s_nvh < 0) s_nvh = 0;
}

static void remember_visit(const char *url, const char *title)
{
    place_front(s_vh, &s_nvh, VH_MAX, url, title);
    save_list(s_vh_path, s_vh, s_nvh);
}

static int page_bookmarked(void)
{
    return W.page && W.page->kind != WP_ERROR && place_find(s_bm, s_nbm, W.page->url) >= 0;
}

static void toggle_bookmark(void)
{
    if (!W.page || W.page->kind == WP_ERROR || W.page->kind == WP_FILE) return;
    int at = place_find(s_bm, s_nbm, W.page->url);
    if (at >= 0) {
        place_remove(s_bm, &s_nbm, at);
        ui_toast("Bookmark removed");
        sfx_play(SFX_OFF);
    } else {
        place_front(s_bm, &s_nbm, BM_MAX, W.page->url, W.nhist ? W.hist[W.cur].title : W.page->url);
        ui_toast(s_nbm == BM_MAX ? "Bookmarked (the oldest one was dropped)" : "Bookmarked");
        sfx_play(SFX_ON);
    }
    save_list(s_bm_path, s_bm, s_nbm);
}

enum { L_BOOKMARK = 1000, L_HISTORY = 2000 };

static void list_cb(int id, int dir)
{
    (void)dir;
    menu_close();
    const Place *p = id >= L_HISTORY ? &s_vh[id - L_HISTORY] : &s_bm[id - L_BOOKMARK];
    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s", p->url);
    go(url, NULL, 1);
}

static void open_list(int history)
{
    static MenuItem it[MENU_MAX];
    const Place *p = history ? s_vh : s_bm;
    int n = history ? s_nvh : s_nbm;
    if (n > MENU_MAX) n = MENU_MAX;
    if (n == 0) {
        ui_toast(history ? "No pages visited yet" : "No bookmarks yet: Menu > Bookmark this page");
        return;
    }
    for (int i = 0; i < n; i++) {
        memset(&it[i], 0, sizeof(it[i]));
        snprintf(it[i].label, sizeof(it[i].label), "%s", p[i].title[0] ? p[i].title : p[i].url);
        it[i].id = (history ? L_HISTORY : L_BOOKMARK) + i;
    }
    menu_open(history ? "History" : "Bookmarks", history ? "Pages visited, newest first" : NULL, it, n, list_cb);
}

/* ------------------------------------------------------------ downloads */

static char s_dev[8] = "ms0:";      /* the device XLT runs from */
static struct { char url[URL_MAX], referer[URL_MAX], name[128], folder[16]; int64_t size; } s_dl;

/* Where a file goes by its kind: the library's folders, else DOWNLOADS. */
static const char *dl_folder(const char *name)
{
    const char *dot = strrchr(name, '.');
    const char *e = dot ? dot + 1 : "";
    if (!strcasecmp(e, "iso") || !strcasecmp(e, "cso") || !strcasecmp(e, "zso")) return "ISO";
    if (audio_format(name) != AUDIO_NONE) return "MUSIC";
    if (!strcasecmp(e, "mp4") || !strcasecmp(e, "pmf")) return "VIDEO";
    if (!strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "png") || !strcasecmp(e, "gif") ||
        !strcasecmp(e, "bmp"))
        return "PICTURE";
    return "DOWNLOADS";
}

static int path_exists(const char *p)
{
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* dev:/FOLDER/name, or "name (2).ext" and so on if it is taken. */
static void dl_path(char *out, int n)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%s", s_dev, s_dl.folder);
    sceIoMkdir(dir, 0777);
    snprintf(out, n, "%s/%s", dir, s_dl.name);
    const char *dot = strrchr(s_dl.name, '.');
    int base = dot ? (int)(dot - s_dl.name) : (int)strlen(s_dl.name);
    for (int k = 2; k < 100 && path_exists(out); k++)
        snprintf(out, n, "%s/%.*s (%d)%s", dir, base, s_dl.name, k, dot ? dot : "");
}

static void format_bytes(char *b, int n, int64_t v)
{
    if (v >= 1 << 30) snprintf(b, n, "%.2f GB", v / 1073741824.0);
    else if (v >= 1 << 20) snprintf(b, n, "%.1f MB", v / 1048576.0);
    else snprintf(b, n, "%d KB", (int)((v + 1023) / 1024));
}

enum { D_SAVE = 1, D_CANCEL };

static void dl_cb(int id, int dir)
{
    (void)dir;
    menu_close();
    if (id != D_SAVE) return;
    char path[256];
    dl_path(path, sizeof(path));
    wf_download(s_dl.url, s_dl.referer, path);
    sfx_play(SFX_SELECT);
}

/* A link that is a file: ask, over the page that stays shown. */
static void offer_download(const WebPage *p, const char *referer)
{
    snprintf(s_dl.url, sizeof(s_dl.url), "%s", p->url);
    snprintf(s_dl.referer, sizeof(s_dl.referer), "%s", referer ? referer : "");
    snprintf(s_dl.name, sizeof(s_dl.name), "%s", p->filename[0] ? p->filename : "download");
    snprintf(s_dl.folder, sizeof(s_dl.folder), "%s", dl_folder(s_dl.name));
    s_dl.size = p->size;
    int64_t g, t;
    int k;
    if (wf_download_progress(&g, &t, &k)) {
        ui_toast("A download is already running");
        return;
    }
    static MenuItem it[2];
    memset(it, 0, sizeof(it));
    snprintf(it[0].label, sizeof(it[0].label), "Save to %s", s_dl.folder);
    it[0].id = D_SAVE;
    snprintf(it[1].label, sizeof(it[1].label), "Cancel");
    it[1].id = D_CANCEL;
    char sub[200], sz[24] = "";
    if (p->size > 0) format_bytes(sz, sizeof(sz), p->size);
    snprintf(sub, sizeof(sub), "%.150s%s%s", s_dl.name, sz[0] ? DOT : "", sz);
    menu_open("Download", sub, it, 2, dl_cb);
}

/* ----------------------------------------------------------------- tabs */

/* The tab in use lives in W; the others are parked here. A parked tab keeps its page
 * (parsed and laid out), history and scroll, but not its pictures (memory): they are
 * fetched again when it comes back. A page still loading stops, and loads again then. */
#define TAB_MAX 4

typedef struct {
    WebPage *page;
    Hist hist[HIST_MAX];
    int nhist, cur;
    int scroll, focus;
    int was_loading;
    Tex *tex;
    char **vals;
    unsigned char *chk;
    short *sel;
} TabSave;

static TabSave s_tabs[TAB_MAX];
static int s_ntabs = 1, s_tab;

static void close_browser(void)
{
    W.open = 0;
    spring_to(&W.t, 0);
}

static void tab_park(TabSave *t)
{
    W.gen++;
    wf_cancel(W.gen); /* this tab's page and pictures stop */
    t->was_loading = W.loading;
    if (W.page && W.page->doc && W.tex)
        for (int i = 0; i < W.page->doc->nimgs; i++) {
            free(W.tex[i].tex);
            W.tex[i].tex = NULL;
            W.tex[i].requested = 0;
        }
    t->page = W.page;
    memcpy(t->hist, W.hist, sizeof(Hist) * W.nhist);
    t->nhist = W.nhist;
    t->cur = W.cur;
    if (W.nhist > 0) W.hist[W.cur].scroll = scroll_target();
    if (t->nhist > 0) t->hist[t->cur].scroll = scroll_target();
    t->scroll = scroll_target();
    t->focus = W.focus;
    t->tex = W.tex;
    t->vals = W.vals;
    t->chk = W.chk;
    t->sel = W.sel;
    W.page = NULL;
    W.tex = NULL;
    W.vals = NULL;
    W.chk = NULL;
    W.sel = NULL;
    W.nhist = W.cur = 0;
    W.loading = 0;
    W.focus = -1;
    W.img_next = W.img_inflight = W.img_mem = 0;
    W.relayout_pending = W.relayout_inflight = 0;
    W.edit_ctrl = W.menu_ctrl = -1;
    W.want_frag[0] = 0;
}

static void reload(void);

static void tab_restore(TabSave *t)
{
    W.page = t->page;
    memcpy(W.hist, t->hist, sizeof(Hist) * t->nhist);
    W.nhist = t->nhist;
    W.cur = t->cur;
    W.focus = t->focus;
    W.tex = t->tex;
    W.vals = t->vals;
    W.chk = t->chk;
    W.sel = t->sel;
    W.img_next = 0; /* its pictures come again */
    int again = t->was_loading || !W.page;
    memset(t, 0, sizeof(*t));
    spring_snap(&W.bar, FX(1));
    scroll_to(W.nhist ? W.hist[W.cur].scroll : 0, 1);
    if (W.nhist && again)
        reload();
}

static void tab_free(TabSave *t)
{
    if (t->page && t->page->doc) {
        for (int i = 0; t->tex && i < t->page->doc->nimgs; i++) free(t->tex[i].tex);
        for (int i = 0; t->vals && i < t->page->doc->nctrls; i++) free(t->vals[i]);
    }
    free(t->tex);
    free(t->vals);
    free(t->chk);
    free(t->sel);
    if (t->page) wf_free_page(t->page);
    memset(t, 0, sizeof(*t));
}

static const char *tab_title(int i)
{
    if (i == s_tab) return W.nhist ? W.hist[W.cur].title : "New tab";
    return s_tabs[i].nhist ? s_tabs[i].hist[s_tabs[i].cur].title : "New tab";
}

static void open_address(int empty);

/* A new tab after the others; url NULL: the address bar comes up in it. */
static void tab_new(const char *url)
{
    if (s_ntabs >= TAB_MAX) {
        ui_toast("4 tabs at most: close one first");
        sfx_play(SFX_ERROR);
        return;
    }
    char u[URL_MAX] = "";
    if (url) snprintf(u, sizeof(u), "%s", url); /* it may point into the page being parked */
    tab_park(&s_tabs[s_tab]);
    s_tab = s_ntabs++;
    spring_snap(&W.bar, FX(1));
    scroll_to(0, 1);
    if (u[0]) go(u, NULL, 1);
    else open_address(1);
}

static void tab_switch(int i)
{
    if (i == s_tab || i < 0 || i >= s_ntabs) return;
    tab_park(&s_tabs[s_tab]);
    s_tab = i;
    tab_restore(&s_tabs[i]);
}

/* Closes the tab in use; the last one closes the browser instead. */
static void tab_close(void)
{
    if (s_ntabs <= 1) {
        close_browser();
        return;
    }
    W.gen++;
    wf_cancel(W.gen);
    free_page_state();
    W.nhist = W.cur = 0;
    W.loading = 0;
    for (int i = s_tab; i < s_ntabs - 1; i++) s_tabs[i] = s_tabs[i + 1];
    memset(&s_tabs[s_ntabs - 1], 0, sizeof(TabSave));
    s_ntabs--;
    if (s_tab >= s_ntabs) s_tab = s_ntabs - 1;
    tab_restore(&s_tabs[s_tab]);
}

enum { T_TAB = 3000 };

static void tabs_cb(int id, int dir)
{
    (void)dir;
    menu_close();
    tab_switch(id - T_TAB);
}

static void open_tabs(void)
{
    static MenuItem it[TAB_MAX];
    for (int i = 0; i < s_ntabs; i++) {
        memset(&it[i], 0, sizeof(it[i]));
        snprintf(it[i].label, sizeof(it[i].label), "%s", tab_title(i));
        if (i == s_tab) snprintf(it[i].value, sizeof(it[i].value), "Open");
        it[i].id = T_TAB + i;
    }
    menu_open("Tabs", NULL, it, s_ntabs, tabs_cb);
}

/* The link the focus is on, resolved; 0 if none. */
static int focused_link(char *out, int n)
{
    WebLayout *l = lay();
    if (!l || W.focus < 0 || W.focus >= l->nfocus || l->focus[W.focus].link < 0) return 0;
    char base[URL_MAX];
    HtmlDoc *d = W.page->doc;
    return url_resolve(page_base(base, sizeof(base)), HTML_STR(d, d->links[l->focus[W.focus].link].href), out, n) == 0;
}

/* ---------------------------------------------------------------- menu */

enum { M_RELOAD = 1, M_FORWARD, M_TOP, M_ADDRESS, M_BOOKMARK, M_BOOKMARKS, M_HISTORY, M_TEXT, M_PICTURES, M_WIFI,
       M_CLOSE, M_STOP_DL, M_LINK_TAB, M_NEW_TAB, M_TABS, M_CLOSE_TAB };

static void fill_menu(MenuItem *it, int *n);

static int profile_index(void)
{
    for (int i = 0; i < W.nprofiles; i++)
        if (W.profile_ids[i] == net_preferred()) return i;
    return 0;
}

static void menu_cb(int id, int dir)
{
    switch (id) {
    case M_RELOAD:
        menu_close();
        if (W.loading) stop_loading();
        else reload();
        return;
    case M_FORWARD: menu_close(); history_go(1); return;
    case M_TOP: menu_close(); W.focus = -1; scroll_to(0, 0); return;
    case M_ADDRESS: menu_close(); open_address(0); return;
    case M_BOOKMARK: toggle_bookmark(); break;
    case M_BOOKMARKS: menu_close(); open_list(0); return;
    case M_HISTORY: menu_close(); open_list(1); return;
    case M_TEXT:
        W.small_text = !W.small_text;
        set_layout();
        if (W.page && W.page->doc) {
            wf_relayout(W.gen, W.page->doc, W.page->imgs, W.page->doc->nimgs);
            W.relayout_inflight = 1;
        }
        save_cfg();
        break;
    case M_PICTURES:
        W.pictures = !W.pictures;
        save_cfg();
        if (!W.pictures) ui_toast("Pictures are off from the next page");
        else ui_toast("Pictures are on from the next page");
        break;
    case M_WIFI:
        if (W.nprofiles > 1) {
            int k = (profile_index() + (dir < 0 ? W.nprofiles - 1 : 1)) % W.nprofiles;
            net_set_preferred(W.profile_ids[k]);
            wf_use_profile(W.profile_ids[k]);
            wf_disconnect();
            save_cfg();
        } else if (dir == 0) {
            ui_toast(W.nprofiles ? "This is the only saved connection" : "No saved Wi-Fi connection");
        }
        break;
    case M_LINK_TAB: {
        char to[URL_MAX];
        menu_close();
        if (focused_link(to, sizeof(to))) tab_new(to);
        return;
    }
    case M_NEW_TAB: menu_close(); tab_new(NULL); return;
    case M_TABS: menu_close(); open_tabs(); return;
    case M_CLOSE_TAB: menu_close(); tab_close(); return;
    case M_STOP_DL:
        menu_close();
        wf_download_stop();
        return;
    case M_CLOSE:
        menu_close();
        W.open = 0;
        spring_to(&W.t, 0);
        return;
    }
    int n;
    MenuItem *items = menu_items(&n);
    fill_menu(items, &n);
    menu_set_count(n);
}

static void fill_menu(MenuItem *it, int *n)
{
    int k = 0;
    memset(it, 0, sizeof(MenuItem) * 20);
    char link[URL_MAX];
    if (focused_link(link, sizeof(link))) {
        snprintf(it[k].label, sizeof(it[k].label), "Open the link in a new tab");
        it[k++].id = M_LINK_TAB;
    }
    snprintf(it[k].label, sizeof(it[k].label), "%s", W.loading ? "Stop loading" : "Reload");
    it[k++].id = M_RELOAD;
    if (W.cur + 1 < W.nhist) {
        snprintf(it[k].label, sizeof(it[k].label), "Forward");
        it[k++].id = M_FORWARD;
    }
    snprintf(it[k].label, sizeof(it[k].label), "Top of the page");
    it[k++].id = M_TOP;
    snprintf(it[k].label, sizeof(it[k].label), "Edit the address");
    it[k++].id = M_ADDRESS;
    if (W.page && W.page->kind != WP_ERROR && W.page->kind != WP_FILE) {
        snprintf(it[k].label, sizeof(it[k].label), "%s", page_bookmarked() ? "Remove the bookmark" : "Bookmark this page");
        it[k].gap = 1;
        it[k++].id = M_BOOKMARK;
    }
    snprintf(it[k].label, sizeof(it[k].label), "Bookmarks");
    if (!(W.page && W.page->kind != WP_ERROR && W.page->kind != WP_FILE)) it[k].gap = 1;
    it[k++].id = M_BOOKMARKS;
    snprintf(it[k].label, sizeof(it[k].label), "History");
    it[k++].id = M_HISTORY;
    snprintf(it[k].label, sizeof(it[k].label), "Text size");
    snprintf(it[k].value, sizeof(it[k].value), "%s", W.small_text ? "Small" : "Normal");
    it[k].gap = 1;
    it[k++].id = M_TEXT;
    snprintf(it[k].label, sizeof(it[k].label), "Pictures");
    snprintf(it[k].value, sizeof(it[k].value), "%s", W.pictures ? "On" : "Off");
    it[k++].id = M_PICTURES;
    snprintf(it[k].label, sizeof(it[k].label), "Wi-Fi");
    snprintf(it[k].value, sizeof(it[k].value), "%s", W.nprofiles ? W.profile_names[profile_index()] : "None saved");
    it[k++].id = M_WIFI;
    int64_t dg, dt;
    int dk;
    if (wf_download_progress(&dg, &dt, &dk)) {
        snprintf(it[k].label, sizeof(it[k].label), "Stop the download");
        it[k].gap = 1;
        it[k++].id = M_STOP_DL;
    }
    snprintf(it[k].label, sizeof(it[k].label), "New tab");
    it[k].gap = 1;
    it[k++].id = M_NEW_TAB;
    if (s_ntabs > 1) {
        snprintf(it[k].label, sizeof(it[k].label), "Tabs");
        snprintf(it[k].value, sizeof(it[k].value), "%d", s_ntabs);
        it[k++].id = M_TABS;
        snprintf(it[k].label, sizeof(it[k].label), "Close this tab");
        it[k++].id = M_CLOSE_TAB;
    }
    snprintf(it[k].label, sizeof(it[k].label), "Close the browser");
    it[k].gap = 1;
    it[k++].id = M_CLOSE;
    *n = k;
}

static void open_menu(void)
{
    W.nprofiles = net_profiles(W.profile_ids, W.profile_names, NET_PROFILES);
    MenuItem it[20];
    int n;
    fill_menu(it, &n);
    char host[96] = "";
    if (W.page) url_host(W.page->url, host, sizeof(host));
    menu_open("Browser", host[0] ? host : NULL, it, n, menu_cb);
}

/* ---------------------------------------------------------------- API */

void web_init(const char *app_dir)
{
    snprintf(s_cfg_path, sizeof(s_cfg_path), "%.200scache/web.txt", app_dir); /* app_dir ends with '/' */
    lists_init(app_dir);
    const char *colon = strchr(app_dir, ':');
    if (colon && colon - app_dir < (int)sizeof(s_dev) - 1)
        snprintf(s_dev, sizeof(s_dev), "%.*s", (int)(colon - app_dir + 1), app_dir);
    load_cfg();
    wf_init(app_dir);
    W.focus = -1;
    W.edit_ctrl = W.menu_ctrl = -1;
}

void web_open(const char *url)
{
    set_layout();
    if (!W.open) {
        W.open = 1;
        spring_to(&W.t, FX(1));
    }
    if (url) {
        go(url, NULL, 1);
    } else if (!W.page && !W.loading) {
        if (W.last_url[0]) go(W.last_url, NULL, 1);
        else open_address(1);
    }
}

void web_open_search(void)
{
    set_layout();
    if (!W.open) {
        W.open = 1;
        spring_to(&W.t, FX(1));
    }
    open_address(1);
}

int web_is_open(void) { return W.open || W.t.x > 0; }
int web_covers(void) { return W.open && W.t.x >= FX(1); }

int web_input(unsigned int p)
{
    if (!W.open)
        return 0;
    if (p & PSP_CTRL_CIRCLE) {
        if (W.loading && W.page) { /* ○ while loading stops it, the page shown stays */
            stop_loading();
            return 1;
        }
        if (W.loading) stop_loading();
        if (W.cur > 0 && W.page) history_go(-1);
        else tab_close(); /* the tab's first page: the tab goes (the last one closes the browser) */
        return 1;
    }
    if (p & PSP_CTRL_TRIANGLE) { open_menu(); return 1; }
    if (p & PSP_CTRL_SQUARE) { open_address(0); return 1; }
    if (p & PSP_CTRL_LTRIGGER) {
        if (W.cur > 0) history_go(-1);
        else sfx_play(SFX_ERROR);
        return 1;
    }
    if (p & PSP_CTRL_RTRIGGER) {
        if (W.cur + 1 < W.nhist) history_go(1);
        else sfx_play(SFX_ERROR);
        return 1;
    }
    if (p & PSP_CTRL_START) { W.focus = -1; scroll_to(0, 0); return 1; }
    if (p & PSP_CTRL_UP) move_focus(-1);
    else if (p & PSP_CTRL_DOWN) move_focus(1);
    else if (p & PSP_CTRL_LEFT) page_by(-1);
    else if (p & PSP_CTRL_RIGHT) page_by(1);
    else if (p & PSP_CTRL_CROSS) activate();
    return 1;
}

/* Keeps the reading position when a new layout replaces the old one. */
static void adopt_layout(WebLayout *nl)
{
    WebLayout *ol = lay();
    int top = scroll_target();
    int anchor_a = -1, anchor_dy = 0, focus_link = -1, focus_ctrl = -1;
    if (ol) {
        for (int i = weblay_span_at(ol, top); i < ol->nspans; i++) {
            if (ol->spans[i].kind == WS_TEXT && ol->spans[i].y >= top) {
                anchor_a = ol->spans[i].a;
                anchor_dy = ol->spans[i].y - top;
                break;
            }
        }
        if (W.focus >= 0 && W.focus < ol->nfocus) {
            focus_link = ol->focus[W.focus].link;
            focus_ctrl = ol->focus[W.focus].ctrl;
        }
    }
    W.page->lay = nl;
    weblay_free(ol);
    int y = top;
    if (anchor_a >= 0 && top > 0) {
        for (int i = 0; i < nl->nspans; i++) {
            if (nl->spans[i].kind == WS_TEXT && nl->spans[i].a <= anchor_a && anchor_a < nl->spans[i].a + nl->spans[i].b) {
                y = nl->spans[i].y - anchor_dy;
                break;
            }
        }
    }
    W.focus = -1;
    if (focus_link >= 0 || focus_ctrl >= 0)
        for (int i = 0; i < nl->nfocus; i++)
            if (nl->focus[i].link == focus_link && nl->focus[i].ctrl == focus_ctrl) { W.focus = i; break; }
    int d = y - top;
    W.scroll.x += FX(d); /* the whole spring moves: no jump, no animation */
    W.scroll.target += FX(d);
    scroll_to(scroll_target(), 0);
}

static void request_images(void)
{
    if (!W.page || !W.page->doc || !W.pictures || W.loading)
        return;
    HtmlDoc *d = W.page->doc;
    char base[URL_MAX];
    const char *b = page_base(base, sizeof(base));
    while (W.img_inflight < IMG_INFLIGHT && W.img_next < d->nimgs) {
        int i = W.img_next++;
        int st = W.page->imgs[i].state;
        if (st != WI_PENDING && !(st == WI_READY && !W.tex[i].tex)) continue; /* (READY without a texture: a tab back) */
        if (W.img_mem > W.img_budget) {
            W.page->imgs[i].state = WI_NONE;
            W.relayout_pending = 1;
            continue;
        }
        char abs[URL_MAX];
        const char *src = HTML_STR(d, d->imgs[i].src);
        if (url_is_svg(src)) { /* no SVG decoder: its alt text shows */
            W.page->imgs[i].state = WI_NONE;
            W.relayout_pending = 1;
            continue;
        }
        if (strncasecmp(src, "data:", 5) && url_resolve(b, src, abs, sizeof(abs)) < 0) {
            W.page->imgs[i].state = WI_NONE;
            continue;
        }
        int lw, lh;
        weblay_img_size(d, i, &s_lay, CONTENT_W, &lw, &lh);
        int mw = d->imgs[i].w > 0 && d->imgs[i].w < CONTENT_W ? d->imgs[i].w : CONTENT_W;
        wf_image(W.gen, i, strncasecmp(src, "data:", 5) ? abs : src, mw, W.page->url);
        W.tex[i].requested = 1;
        W.img_inflight++;
    }
}

static void take_results(void)
{
    WebResult r;
    while (wf_poll(&r)) {
        if (r.kind == WR_PAGE) {
            if (r.gen != W.gen || !r.page) {
                if (r.page) wf_free_page(r.page);
                continue;
            }
            if (r.page->kind == WP_FILE) { /* a file: the page shown stays, the history goes back to it */
                const char *ref = W.page ? W.page->url : NULL;
                offer_download(r.page, ref);
                W.loading = 0;
                if (W.page && W.nhist > 0) {
                    for (int i = 0; i < W.nhist; i++)
                        if (!strcmp(W.hist[i].url, W.page->url)) W.cur = i;
                    W.nhist = W.cur + 1;
                } else if (!W.page) {
                    W.nhist = W.cur = 0;
                }
                wf_free_page(r.page);
                continue;
            }
            adopt_page(r.page);
            if (r.page->kind == WP_ERROR) sfx_play(SFX_ERROR);
        } else if (r.kind == WR_IMG) {
            if (r.gen != W.gen || !W.page || !W.page->doc || r.img < 0 || r.img >= W.page->doc->nimgs) {
                free(r.tex);
                continue;
            }
            W.img_inflight--;
            WebImgState *st = &W.page->imgs[r.img];
            int lw, lh;
            weblay_img_size(W.page->doc, r.img, &s_lay, CONTENT_W, &lw, &lh);
            if (r.ok) {
                Tex *t = &W.tex[r.img];
                free(t->tex);
                t->tex = r.tex;
                t->psm = r.psm;
                t->tw = r.tw;
                t->th = r.th;
                t->tbw = r.tbw;
                t->w = r.w;
                t->h = r.h;
                W.img_mem += r.tbw * (r.h + 1) * 2;
                st->state = WI_READY;
                st->w = (short)r.w;
                st->h = (short)r.h;
                int nw, nh;
                weblay_img_size(W.page->doc, r.img, &s_lay, CONTENT_W, &nw, &nh);
                if (nw != lw || nh != lh || lw == 0) W.relayout_pending = 1;
                else {
                    /* same box: just drop the placeholder flag in the spans */
                    WebLayout *l = lay();
                    for (int i = 0; l && i < l->nspans; i++)
                        if (l->spans[i].kind == WS_IMG && l->spans[i].a == r.img) l->spans[i].flags &= ~WSF_PLACEHOLDER;
                }
            } else {
                st->state = WI_NONE;
                W.relayout_pending = 1; /* its alt text shows instead */
            }
        } else if (r.kind == WR_DOWNLOAD) {
            if (r.ok) {
                const char *rel = strstr(r.msg, ":/");
                char m[200];
                snprintf(m, sizeof(m), "Saved to %.180s", rel ? rel + 2 : r.msg);
                ui_toast(m);
                sfx_play(SFX_ON);
                library_rescan(); /* games, music, videos and pictures show up in their tabs */
            } else {
                ui_toast(r.msg);
                sfx_play(SFX_ERROR);
            }
        } else if (r.kind == WR_LAYOUT) {
            W.relayout_inflight = 0;
            if (r.gen != W.gen || !W.page || !r.lay) {
                weblay_free(r.lay);
                continue;
            }
            adopt_layout(r.lay);
        }
    }
}

int web_step(void)
{
    int old = W.t.x;
    spring_step(&W.t);
    int dirty = W.t.x != old;
    if (!W.open && W.t.x <= 0) {
        if (W.boosted) {
            W.boosted = 0;
            if (!library_scanning()) power_idle();
        }
        take_results(); /* answers still coming in are dropped */
        return dirty;
    }
    int had_page = W.page != NULL, was_loading = W.loading;
    take_results();
    if (W.page != NULL && !had_page) dirty = 1;
    if (was_loading != W.loading) dirty = 1;
    request_images();
    unsigned now = (unsigned)(sceKernelGetSystemTimeWide() / 1000);
    if (W.relayout_pending && !W.relayout_inflight && W.page && W.page->doc &&
        (now - W.relayout_at > RELAYOUT_MS || W.img_inflight == 0)) {
        wf_relayout(W.gen, W.page->doc, W.page->imgs, W.page->doc->nimgs);
        W.relayout_pending = 0;
        W.relayout_inflight = 1;
        W.relayout_at = now;
    }
    /* The full clock for as long as the browser is open: TLS and decoding want it, and the
     * clock must not change while the Wi-Fi modules start. */
    if (!W.boosted) {
        W.boosted = 1;
        power_boost();
    }
    int64_t dg, dt;
    int dk;
    static unsigned dl_drawn;
    if (wf_download_progress(&dg, &dt, &dk) && now - dl_drawn >= 250) { /* the download line, 4 times a second */
        dl_drawn = now;
        dirty = 1;
    }
    if (W.loading || W.img_inflight > 0 || W.relayout_inflight) { /* the progress line moves */
        W.anim++;
        dirty = 1;
    }
    old = W.scroll.x;
    spring_step(&W.scroll);
    dirty |= W.scroll.x != old;
    old = W.bar.x;
    spring_step(&W.bar);
    dirty |= W.bar.x != old;
    return dirty;
}

void web_shutdown(void)
{
    for (int i = 0; i < s_ntabs; i++)
        if (i != s_tab) tab_free(&s_tabs[i]);
    s_ntabs = 1;
    s_tab = 0;
    free_page_state();
    W.gen++;
    wf_cancel(W.gen);
    W.loading = 0;
    W.open = 0;
    spring_snap(&W.t, 0);
    wf_shutdown();
}

/* ---------------------------------------------------------------- draw */

_Static_assert((int)WICON_POWER == (int)WEB_ICON_POWER && (int)WICON_CANCEL == (int)WEB_ICON_CANCEL &&
                   (int)WICON_GLOBE_S == (int)WEB_ICON_GLOBE_S && (int)WICON_USB == (int)WEB_ICON_USB,
               "web.h's icon numbers follow tools/bake_webicons.py");

void web_draw_icon(int id, int x, int y, int w, int h, unsigned int c)
{
    if (id < 0 || id >= WICON_COUNT) return;
    const short *r = webicon_rects[id];
    gfx_bind(webicon_pixels, GU_PSM_4444, WEBICON_W, WEBICON_H, WEBICON_W, 1);
    gfx_sprite(x, y, x + w, y + h, r[0], r[1], r[0] + r[2], r[1] + r[2], c);
    gfx_bind_atlas();
}

static u32 span_color(int c)
{
    switch (c) {
    case WC_LINK: return C_FOCUS;
    case WC_MUTED: return C_MUTED;
    case WC_HEAD: return C_TEXT; /* bold already; white would vanish on a light theme */
    }
    return C_TEXT;
}

static void draw_ctrl(const WebSpan *s, int x, int y, int focused)
{
    HtmlDoc *d = W.page->doc;
    int c = s->a;
    HtmlCtrl *k = &d->ctrls[c];
    int w = s->w, h = s->ih;
    int font = s_lay.font_body;
    int ty = y + (h - gfx_line_h(font)) / 2;
    switch (k->type) {
    case HC_TEXT: case HC_PASSWORD: case HC_TEXTAREA: {
        gfx_rrect(x, y, w, h, 6, C_RAISED);
        gfx_rect(x + 6, y + h - 1, w - 12, 1, focused ? C_FOCUS : C_LINE);
        const char *v = ctrl_value(c);
        if (v[0]) {
            if (k->type == HC_PASSWORD) {
                char dots[96];
                int n = (int)strlen(v), o = 0;
                for (int i = 0; i < n && o < (int)sizeof(dots) - 4; i++) {
                    memcpy(dots + o, "\xE2\x80\xA2", 3);
                    o += 3;
                }
                dots[o] = 0;
                gfx_text_fit(font, x + 8, k->type == HC_TEXTAREA ? y + 4 : ty, w - 16, dots, C_TEXT);
            } else {
                gfx_text_fit(font, x + 8, k->type == HC_TEXTAREA ? y + 4 : ty, w - 16, v, C_TEXT);
            }
        } else {
            const char *ph = HTML_STR(d, k->label);
            gfx_text_fit(font, x + 8, k->type == HC_TEXTAREA ? y + 4 : ty, w - 16, ph[0] ? ph : "", C_MUTED);
        }
        break;
    }
    case HC_CHECKBOX: case HC_RADIO: {
        int bs = 14, bx = x + 2, by = y + (h - bs) / 2;
        int on = W.chk[c];
        gfx_rrect(bx, by, bs, bs, k->type == HC_RADIO ? 7 : 3, on ? C_ACCENT : C_RAISED);
        if (on && k->type == HC_CHECKBOX) gfx_icon(ICON_CHECK, bx, by, bs, bs, C_WHITE);
        if (on && k->type == HC_RADIO) gfx_rrect(bx + 4, by + 4, 6, 6, 3, C_WHITE);
        break;
    }
    case HC_SELECT: {
        gfx_rrect(x, y, w, h, 6, C_RAISED);
        const char *lab = W.sel[c] >= 0 && W.sel[c] < k->opt_n ? HTML_STR(d, d->opts[k->opt_first + W.sel[c]].label) : "";
        gfx_text_fit(font, x + 8, ty, w - 30, lab, C_TEXT);
        int ax = x + w - 16, ay = y + h / 2 - 2; /* a small down arrow */
        gfx_rect(ax, ay, 7, 1, C_MUTED);
        gfx_rect(ax + 1, ay + 1, 5, 1, C_MUTED);
        gfx_rect(ax + 2, ay + 2, 3, 1, C_MUTED);
        gfx_rect(ax + 3, ay + 3, 1, 1, C_MUTED);
        break;
    }
    default: {
        int js = k->type == HC_BUTTON;
        gfx_rrect(x, y, w, h, h / 2, focused ? C_ACCENT : C_PILL);
        const char *lab = HTML_STR(d, k->label);
        int bty = y + (h - gfx_line_h(FONT_BOLD)) / 2;
        int tw = gfx_text_width(FONT_BOLD, lab);
        u32 tc = focused ? C_WHITE : js ? C_MUTED : C_TEXT;
        if (tw > w - 16) gfx_text_fit(FONT_BOLD, x + 8, bty, w - 16, lab, tc);
        else gfx_text(FONT_BOLD, x + (w - tw) / 2, bty, lab, tc);
        break;
    }
    }
    if (focused && k->type != HC_SUBMIT && k->type != HC_BUTTON && k->type != HC_IMAGE)
        gfx_ring(x - 3, y - 3, w + 6, h + 6, C_FOCUS);
}

static void draw_page(void)
{
    WebLayout *l = lay();
    HtmlDoc *d = W.page->doc;
    int top = spring_int(&W.scroll), vt = view_top();
    int y_end = top + (FOOTER_Y - vt);
    int ox = CONTENT_X, oy = vt - top;
    for (int i = weblay_span_at(l, top); i < l->nspans; i++) {
        const WebSpan *s = &l->spans[i];
        if (s->y >= y_end) break;
        int x = ox + s->x, y = oy + s->y;
        int focused = s->focus >= 0 && s->focus == W.focus;
        switch (s->kind) {
        case WS_TEXT: case WS_ALT: case WS_BULLET: {
            const char *t = s->kind == WS_ALT ? d->strs + s->a : d->text + s->a;
            int ty = y + s->h - gfx_line_h(s->font) - line_gap() + 1;
            if (focused)
                gfx_rrect(x - 2, ty - 1, s->w + 4, gfx_line_h(s->font) + 2, 4, C_ACCENT);
            else if (s->flags & WSF_CODE)
                gfx_rrect(x - 1, ty, s->w + 2, gfx_line_h(s->font), 3, C_SURFACE);
            u32 c = focused ? C_WHITE : span_color(s->color);
            gfx_text_n(s->font, x, ty, t, s->b, c);
            if (s->flags & WSF_STRIKE)
                gfx_rect(x, ty + gfx_line_h(s->font) / 2 + 1, s->w, 1, c);
            break;
        }
        case WS_IMG: {
            int iy = y + s->h - s->ih;
            Tex *t = W.tex ? &W.tex[s->a] : NULL;
            if (t && t->tex && !(s->flags & WSF_PLACEHOLDER)) {
                gfx_texture(t->tex, t->psm, t->tw, t->th, t->tbw, 0, t->w, t->h, x, iy, s->w, s->ih);
            } else {
                gfx_rrect(x, iy, s->w, s->ih, s->ih > 12 ? 4 : 2, C_SURFACE);
                if (s->w >= 24 && s->ih >= 20)
                    gfx_icon(ICON_PICTURE, x + (s->w - 16) / 2, iy + (s->ih - 16) / 2, 16, 16, C_LINE);
            }
            if (focused) gfx_ring(x - 3, iy - 3, s->w + 6, s->ih + 6, C_FOCUS);
            break;
        }
        case WS_CTRL:
            draw_ctrl(s, x, y + s->h - s->ih, focused);
            break;
        case WS_RULE:
            gfx_rect(x, y, s->w, 1, C_LINE);
            break;
        case WS_PREBG:
            gfx_rect(x, y, s->w, s->h + 1, C_SURFACE);
            break;
        case WS_QUOTEBAR:
            gfx_rect(x, y, s->w, s->h + 1, C_LINE);
            break;
        }
    }
    /* the scroll bar */
    if (l->height > FOOTER_Y - vt) {
        int track = FOOTER_Y - vt - 8;
        int th = track * (FOOTER_Y - vt) / l->height;
        if (th < 16) th = 16;
        int ms = l->height - (FOOTER_Y - vt);
        int ty = vt + 4 + (ms > 0 ? (track - th) * top / ms : 0);
        gfx_rrect(SCREEN_W - 6, ty, 3, th, 1, C_LINE);
    }
}

static void draw_message(const char *title, const char *sub, int icon)
{
    int cy = (view_top() + FOOTER_Y) / 2;
    if (icon >= 0) web_draw_icon(icon, SCREEN_W / 2 - 20, cy - 52, 40, 40, C_MUTED);
    gfx_text_center(FONT_BOLD, SCREEN_W / 2, cy - 4, title, C_TEXT);
    if (sub && sub[0]) {
        /* wrap the explanation into two lines at most */
        char a[160];
        snprintf(a, sizeof(a), "%s", sub);
        if (gfx_text_width(FONT_SMALL, a) > SCREEN_W - 60) {
            int cut = (int)strlen(a) / 2;
            while (cut > 0 && a[cut] != ' ') cut--;
            if (cut > 0) {
                a[cut] = 0;
                gfx_text_center(FONT_SMALL, SCREEN_W / 2, cy + 18, a, C_MUTED);
                gfx_text_center(FONT_SMALL, SCREEN_W / 2, cy + 34, a + cut + 1, C_MUTED);
                return;
            }
        }
        gfx_text_center(FONT_SMALL, SCREEN_W / 2, cy + 18, a, C_MUTED);
    }
}

static const char *phase_text(void)
{
    int ph;
    int64_t got, total;
    wf_progress(&ph, &got, &total);
    static char b[96];
    if (ph == WF_WIFI) {
        int st = net_state();
        const char *name = net_profile_name();
        if (st == NET_GETTING_IP) snprintf(b, sizeof(b), "Getting an address from %s...", name[0] ? name : "Wi-Fi");
        else if (st == NET_STARTING) snprintf(b, sizeof(b), "Starting Wi-Fi (%s)...", net_step());
        else snprintf(b, sizeof(b), "Connecting to %s...", name[0] ? name : "Wi-Fi");
        return b;
    }
    if (ph == WF_CONNECTING) {
        char host[80];
        url_host(W.loading_url, host, sizeof(host));
        snprintf(b, sizeof(b), "Connecting to %s...", host);
        return b;
    }
    if (ph == WF_RECEIVING) {
        if (got >= 1 << 20) snprintf(b, sizeof(b), "%.1f MB", got / 1048576.0);
        else snprintf(b, sizeof(b), "%d KB", (int)(got >> 10));
        return b;
    }
    if (ph == WF_WORKING) return "Laying out...";
    return "Loading...";
}

static void draw_bar(void)
{
    int a = W.bar.x;
    if (a <= 8) {
        gfx_rect(0, 0, SCREEN_W, BAR_Y, C_BG); /* under the status bar */
        return;
    }
    if (a > 256) a = 256;
    int y = BAR_Y - (BAR_H + 4) * (256 - a) / 256;
    gfx_rect(0, 0, SCREEN_W, y + BAR_H + 4, C_BG);
    gfx_rrect(CONTENT_X - 2, y, SCREEN_W - 2 * (CONTENT_X - 2), BAR_H, BAR_H / 2, C_SURFACE);
    const char *url = W.loading ? W.loading_url : W.page ? W.page->url : "";
    int x = CONTENT_X + 8;
    int https = !strncmp(url, "https://", 8);
    if (https) {
        web_draw_icon(WICON_LOCK, x, y + 5, 12, 12, C_MUTED);
        x += 17;
    } else if (url[0]) {
        web_draw_icon(WICON_GLOBE_S, x - 1, y + 3, 15, 15, C_MUTED);
        x += 17;
    }
    char pretty[URL_MAX];
    url_pretty(url, pretty, sizeof(pretty));
    if (!url[0]) snprintf(pretty, sizeof(pretty), "Search or type an address");
    int right = SCREEN_W - CONTENT_X - 10;
    if (s_ntabs > 1) { /* how many tabs: a small square at the end, like a phone browser's */
        char n[4];
        snprintf(n, sizeof(n), "%d", s_ntabs);
        int bx = SCREEN_W - CONTENT_X - 24;
        gfx_rrect(bx, y + 4, 15, 14, 3, C_MUTED);
        gfx_rrect(bx + 1, y + 5, 13, 12, 3, C_SURFACE);
        gfx_text_center(FONT_SMALL, bx + 8, y + 3, n, C_TEXT);
        right -= 22;
    }
    const char *ph = NULL;
    if (W.loading) {
        ph = phase_text();
        right -= gfx_text_width(FONT_SMALL, ph) + 10;
        gfx_text_right(FONT_SMALL, right + gfx_text_width(FONT_SMALL, ph) + 10, y + 4, ph, C_FOCUS);
    }
    /* host bright, the rest dim */
    char host[128];
    url_host(url, host, sizeof(host));
    const char *h = host[0] ? strstr(pretty, host + (strncmp(host, "www.", 4) ? 0 : 4)) : NULL;
    if (h == pretty && host[0]) {
        int hl = (int)strlen(host + (strncmp(host, "www.", 4) ? 0 : 4));
        int hw = 0;
        for (const char *p = pretty; p < pretty + hl;) hw += gfx_glyph_adv(FONT_SMALL, &p);
        if (x + hw > right) {
            gfx_text_fit(FONT_SMALL, x, y + 4, right - x, pretty, C_TEXT);
        } else {
            gfx_text_n(FONT_SMALL, x, y + 4, pretty, hl, C_TEXT);
            gfx_text_fit(FONT_SMALL, x + hw, y + 4, right - x - hw, pretty + hl, C_MUTED);
        }
    } else {
        gfx_text_fit(FONT_SMALL, x, y + 4, right - x, pretty, url[0] ? C_TEXT : C_MUTED);
    }
    /* progress: a moving segment while there is no size, else a filling line */
    if (W.loading || W.img_inflight > 0) {
        int ph2;
        int64_t got, total;
        wf_progress(&ph2, &got, &total);
        int bx = CONTENT_X + 8, bw = SCREEN_W - 2 * CONTENT_X - 16, by = y + BAR_H - 2;
        if (W.loading && ph2 == WF_RECEIVING && total > 0) {
            int fw = (int)(bw * (got > total ? total : got) / total);
            gfx_rect(bx, by, fw, 2, C_FOCUS);
        } else if (W.loading) {
            int seg = bw / 4, pos = (W.anim * 4) % (bw + seg) - seg;
            int s0 = pos < 0 ? 0 : pos, s1 = pos + seg > bw ? bw : pos + seg;
            if (s1 > s0) gfx_rect(bx + s0, by, s1 - s0, 2, C_FOCUS);
        } else {
            int seg = bw / 8, pos = (W.anim * 2) % (bw + seg) - seg; /* pictures: quieter */
            int s0 = pos < 0 ? 0 : pos, s1 = pos + seg > bw ? bw : pos + seg;
            if (s1 > s0) gfx_rect(bx + s0, by, s1 - s0, 1, C_LINE);
        }
    }
}

void web_draw(const SysStatus *st)
{
    int t = W.t.x;
    if (t <= 0)
        return;
    if (t > 256) t = 256;
    gfx_set_opacity(t);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);

    if (W.page && W.page->lay && W.page->doc) {
        draw_page();
    } else if (W.page && (W.page->kind == WP_ERROR || W.page->kind == WP_FILE)) {
        draw_message(W.page->kind == WP_FILE ? "Can't open this" : "Couldn't open the page", W.page->err, WICON_GLOBE);
    } else if (W.loading) {
        int ph;
        int64_t g, tt;
        wf_progress(&ph, &g, &tt);
        draw_message(ph == WF_WIFI ? "Wi-Fi" : "Loading", phase_text(), WICON_GLOBE);
    } else {
        draw_message("Browser", NULL, WICON_GLOBE);
        static const Hint h[] = {{ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Search or type an address"}};
        ui_draw_hints(h, 1, SCREEN_W / 2, (view_top() + FOOTER_Y) / 2 + 16);
    }

    draw_bar();
    /* the status bar over everything */
    char title[160];
    const char *tt = W.page && W.nhist ? W.hist[W.cur].title : "Browser";
    snprintf(title, sizeof(title), "%s", tt);
    if (gfx_text_width(FONT_SMALL, title) > 220) {
        int n = (int)strlen(title);
        while (n > 0 && gfx_text_width(FONT_SMALL, title) + gfx_text_width(FONT_SMALL, "...") > 220) {
            do n--; while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80);
            title[n] = 0;
        }
        strcat(title, "...");
    }
    ui_draw_status_bar(title, st);

    /* where the focused link goes */
    WebLayout *l = lay();
    if (l && W.focus >= 0 && W.focus < l->nfocus && l->focus[W.focus].link >= 0 && !W.loading) {
        HtmlDoc *d = W.page->doc;
        char base[URL_MAX], to[URL_MAX], pretty[URL_MAX];
        if (url_resolve(page_base(base, sizeof(base)), HTML_STR(d, d->links[l->focus[W.focus].link].href), to, sizeof(to)) == 0) {
            if (url_same_page(to, W.page->url) && url_fragment(to)[0]) snprintf(pretty, sizeof(pretty), "#%s", url_fragment(to));
            else url_pretty(to, pretty, sizeof(pretty));
            int w = gfx_text_width(FONT_SMALL, pretty) + 16;
            if (w > 300) w = 300;
            gfx_rrect(6, FOOTER_Y - 24, w, 20, 10, WITH_ALPHA(C_RAISED, 0xF0));
            gfx_text_fit(FONT_SMALL, 14, FOOTER_Y - 22, w - 16, pretty, C_MUTED);
        }
    }

    int64_t dg, dt;
    int dk;
    if (wf_download_progress(&dg, &dt, &dk)) { /* the download, bottom right */
        char a[24], line[200];
        format_bytes(a, sizeof(a), dg);
        if (dt > 0)
            snprintf(line, sizeof(line), "Downloading %.60s" DOT "%d%%" DOT "%d KB/s", s_dl.name, (int)(dg * 100 / dt), dk);
        else
            snprintf(line, sizeof(line), "Downloading %.60s" DOT "%s" DOT "%d KB/s", s_dl.name, a, dk);
        int w = gfx_text_width(FONT_SMALL, line) + 16;
        if (w > 330) w = 330;
        int x = SCREEN_W - 6 - w;
        gfx_rrect(x, FOOTER_Y - 24, w, 20, 10, WITH_ALPHA(C_RAISED, 0xF0));
        if (dt > 0) gfx_rect(x + 10, FOOTER_Y - 7, (int)((w - 20) * (dg > dt ? dt : dg) / dt), 2, C_FOCUS);
        gfx_text_fit(FONT_SMALL, x + 8, FOOTER_Y - 23, w - 16, line, C_TEXT);
    }
    gfx_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_BG);
    gfx_rect(0, FOOTER_Y, SCREEN_W, 1, C_LINE);
    static const Hint hints[] = {
        {ICON_BTN_CROSS, NULL, C_BTN_CROSS, "Open"},
        {ICON_BTN_SQUARE, NULL, C_BTN_SQUARE, "Address"},
        {-1, "L/R", 0, "Back / Fwd"},
        {ICON_BTN_TRIANGLE, NULL, C_BTN_TRIANGLE, "Menu"},
        {ICON_BTN_CIRCLE, NULL, C_BTN_CIRCLE, "Back"},
    };
    ui_draw_hints(hints, COUNT(hints), SCREEN_W / 2, FOOTER_Y + 5);
    gfx_set_opacity(256);
}

/* ------------------------------------------------------------ home row */

/* Search, then the first bookmarks, then the browser (Continue / an address). */
#define ROW_BOOKMARKS 2

int web_tile_count(void) { return 1 + (s_nbm < ROW_BOOKMARKS ? s_nbm : ROW_BOOKMARKS); }

const char *web_tile_name(int i)
{
    return i == 0 ? "Search" : s_bm[i - 1].title[0] ? s_bm[i - 1].title : s_bm[i - 1].url;
}

const char *web_tile_sub(int i)
{
    if (i == 0) return "DuckDuckGo";
    static char b[URL_MAX];
    url_host(s_bm[i - 1].url, b, sizeof(b));
    if (!strncmp(b, "www.", 4)) memmove(b, b + 4, strlen(b + 4) + 1);
    return b;
}

const char *web_last_title(void) { return W.page && W.nhist ? W.hist[W.cur].title : W.last_title; }

void web_tile_open(int i)
{
    if (i < 0 || i >= web_tile_count()) {
        web_open(NULL);
        return;
    }
    if (i == 0) web_open_search();
    else web_open(s_bm[i - 1].url);
}
