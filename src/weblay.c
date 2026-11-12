/* XLT - web page layout. See weblay.h.
 *
 * Inline boxes (pieces of text, images, controls) are collected on the
 * current line until the next one doesn't fit; then the line is closed: its
 * height is the tallest box, everything sits on its bottom, and centred
 * lines are shifted. Text breaks after a space, or anywhere in a word too
 * long for a whole line. */
#include <stdlib.h>
#include <string.h>
#include "weblay.h"

#define INDENT 16
#define MAX_INDENT 6
#define CELL_GAP 14
#define CTRL_PAD 8
#define MAX_IMG_H 512 /* textures are at most 512 high */

static const int s_gap[4] = {0, 4, 9, 13};

typedef struct {
    const HtmlDoc *d;
    const WebLayCfg *c;
    WebLayout *l;
    int cap_spans, cap_focus;
    int oom;

    /* the open line */
    WebSpan line[256];
    int nline;
    int x, left, y;
    int line_flags, line_head, line_indent, line_has_bullet;
    int last_head;        /* head of the last closed line */
    int pending_gap;      /* gap owed before the next line, -1 at the very top */
} L;

static int line_h(const L *s, int font) { return s->c->line_h[font & 7]; }

static int text_w(const L *s, int font, const char *t, int n)
{
    const char *p = t, *e = t + n;
    int w = 0;
    while (p < e)
        w += s->c->adv(font, &p);
    return w;
}

static int font_of(const L *s, int flags, int head)
{
    if (head || (flags & HF_BOLD)) return s->c->font_bold;
    if (flags & HF_SMALL) return s->c->font_small;
    return s->c->font_body;
}

static int color_of(int flags, int head, int link)
{
    if (link >= 0) return WC_LINK;
    if (flags & HF_MUTED) return WC_MUTED;
    if (head) return WC_HEAD;
    return WC_TEXT;
}

static int left_of(int indent)
{
    if (indent > MAX_INDENT) indent = MAX_INDENT;
    return indent * INDENT;
}

static WebSpan *out_span(L *s)
{
    WebLayout *l = s->l;
    if (l->nspans >= s->cap_spans) {
        int nc = s->cap_spans ? s->cap_spans * 2 : 1024;
        WebSpan *n = realloc(l->spans, (size_t)nc * sizeof(WebSpan));
        if (!n) { s->oom = 1; return NULL; }
        l->spans = n;
        s->cap_spans = nc;
    }
    return &l->spans[l->nspans++];
}

static void focus_for(L *s, WebSpan *sp, int span_idx, int ctrl)
{
    WebLayout *l = s->l;
    if (sp->link < 0 && ctrl < 0) {
        sp->focus = -1;
        return;
    }
    if (ctrl < 0 && l->nfocus > 0) {
        WebFocus *f = &l->focus[l->nfocus - 1];
        if (f->link == sp->link && f->ctrl < 0) { /* the same link, continued */
            if (sp->y + sp->h > f->y1) f->y1 = sp->y + sp->h;
            sp->focus = l->nfocus - 1;
            return;
        }
    }
    if (l->nfocus >= s->cap_focus) {
        int nc = s->cap_focus ? s->cap_focus * 2 : 256;
        WebFocus *n = realloc(l->focus, (size_t)nc * sizeof(WebFocus));
        if (!n) { s->oom = 1; sp->focus = -1; return; }
        l->focus = n;
        s->cap_focus = nc;
    }
    WebFocus *f = &l->focus[l->nfocus];
    f->y0 = sp->y;
    f->y1 = sp->y + sp->h;
    f->first = span_idx;
    f->link = ctrl >= 0 ? -1 : sp->link;
    f->ctrl = ctrl;
    sp->focus = l->nfocus++;
}

/* Closes the open line (an empty one only if `force`, for <br><br>). */
static void flush(L *s, int force)
{
    if (s->nline == 0 && !force)
        return;
    if (s->pending_gap > 0)
        s->y += s->pending_gap;
    s->pending_gap = 0;
    int h = 0, right = 0;
    for (int i = 0; i < s->nline; i++) {
        int bh = s->line[i].kind == WS_IMG || s->line[i].kind == WS_CTRL ? s->line[i].ih : s->line[i].h;
        if (bh > h) h = bh;
        if (s->line[i].kind != WS_BULLET && s->line[i].x + s->line[i].w > right)
            right = s->line[i].x + s->line[i].w;
    }
    if (h == 0) h = line_h(s, s->c->font_body);
    int shift = 0;
    if (s->line_flags & HF_CENTER) {
        shift = (s->c->width - right) / 2;
        if (shift < 0) shift = 0;
    }
    if (s->line_flags & HF_PRE) {
        WebSpan *bg = out_span(s);
        if (bg) {
            memset(bg, 0, sizeof(*bg));
            bg->kind = WS_PREBG;
            bg->x = (short)(s->left > 4 ? s->left - 4 : 0);
            bg->w = (short)(s->c->width - bg->x);
            bg->y = s->y;
            bg->h = (short)h;
            bg->link = bg->focus = -1;
        }
    }
    if (s->line_flags & HF_QUOTE) {
        WebSpan *bar = out_span(s);
        if (bar) {
            memset(bar, 0, sizeof(*bar));
            bar->kind = WS_QUOTEBAR;
            bar->x = (short)(s->left - 10 > 0 ? s->left - 10 : 0);
            bar->w = 3;
            bar->y = s->y;
            bar->h = (short)h;
            bar->link = bar->focus = -1;
        }
    }
    for (int i = 0; i < s->nline; i++) {
        WebSpan *sp = out_span(s);
        if (!sp) break;
        *sp = s->line[i];
        sp->y = s->y;
        sp->h = (short)h;
        if (sp->kind != WS_BULLET) sp->x = (short)(sp->x + shift);
        focus_for(s, sp, s->l->nspans - 1, sp->kind == WS_CTRL ? sp->a : -1);
    }
    s->y += h + (h > 0 ? 1 : 0);
    s->last_head = s->line_head;
    s->nline = 0;
    s->line_flags = 0;
    s->line_head = 0;
    s->line_has_bullet = 0;
    s->x = s->left;
}

static WebSpan *line_add(L *s)
{
    if (s->nline >= (int)(sizeof(s->line) / sizeof(s->line[0])))
        flush(s, 0);
    WebSpan *sp = &s->line[s->nline++];
    memset(sp, 0, sizeof(*sp));
    sp->link = sp->focus = -1;
    return sp;
}

/* Before the first box of a line: the line takes this item's block style. */
static void line_style(L *s, const HtmlItem *it)
{
    if (s->nline == 0 || (s->nline == 1 && s->line_has_bullet)) {
        if (!s->line_has_bullet) {
            s->left = left_of(it->indent);
            s->x = s->left;
        }
        s->line_flags = it->flags;
        s->line_head = it->head;
        s->line_indent = it->indent;
    }
}

static void text_item(L *s, const HtmlItem *it, const char *t, int n, int kind)
{
    int font = font_of(s, it->flags, it->head);
    int color = color_of(it->flags, it->head, it->link);
    int fh = line_h(s, font);
    int right = s->c->width;
    const char *p = t, *e = t + n;
    while (p < e) {
        line_style(s, it);
        if (s->nline == 0)
            while (p < e && *p == ' ') p++; /* no spaces at the start of a line */
        if (p >= e) break;
        const char *seg = p, *brk = NULL;
        int x = s->x, brk_x = 0;
        while (p < e) {
            const char *q = p;
            int a = s->c->adv(font, &q);
            if (x + a > right && (p > seg || s->nline > 0)) break;
            x += a;
            if (*p == ' ') { brk = q; brk_x = x; }
            p = q;
        }
        if (p < e) { /* overflow */
            if (brk && brk > seg) {
                p = brk;
                x = brk_x;
            } else if (s->nline > 0 && !(s->nline == 1 && s->line_has_bullet) && p == seg) {
                flush(s, 0);
                continue;
            } else if (s->nline > 0 && !(s->nline == 1 && s->line_has_bullet) && !(it->flags & HF_PRE)) {
                /* the word started on this line doesn't fit: move it all to the next line if it would fit there */
                int ww = text_w(s, font, seg, (int)(p - seg));
                const char *we = p;
                while (we < e && *we != ' ') { const char *q = we; ww += s->c->adv(font, &q); we = q; }
                if (ww <= right - s->left) {
                    p = seg;
                    flush(s, 0);
                    continue;
                }
            }
        }
        if (p > seg) {
            WebSpan *sp = line_add(s);
            sp->kind = (uint8_t)kind;
            sp->font = (uint8_t)font;
            sp->color = (uint8_t)color;
            sp->flags = (uint8_t)((it->flags & HF_STRIKE ? WSF_STRIKE : 0) | (it->flags & HF_CODE && !(it->flags & HF_PRE) ? WSF_CODE : 0));
            sp->link = it->link;
            sp->x = (short)s->x;
            sp->w = (short)(x - s->x);
            sp->h = (short)fh;
            sp->a = (int)(seg - (kind == WS_ALT ? s->d->strs : s->d->text));
            sp->b = (int)(p - seg);
            s->x = x;
        }
        if (p < e)
            flush(s, 0);
    }
}

void weblay_img_size(const HtmlDoc *d, int img, const WebLayCfg *c, int avail, int *w, int *h)
{
    const HtmlImg *im = &d->imgs[img];
    int iw = 0, ih = 0;
    if (c->imgs && c->imgs[img].state == WI_READY) {
        iw = c->imgs[img].w;
        ih = c->imgs[img].h;
    } else if (im->w > 0 && im->h > 0) {
        iw = im->w;
        ih = im->h;
    } else if (im->w > 0) {
        iw = im->w;
        ih = im->w * 3 / 4;
    }
    if (iw > avail && iw > 0) {
        ih = (int)((long long)ih * avail / iw);
        iw = avail;
    }
    if (ih > MAX_IMG_H) {
        iw = (int)((long long)iw * MAX_IMG_H / ih);
        ih = MAX_IMG_H;
    }
    *w = iw;
    *h = ih;
}

void weblay_ctrl_size(const HtmlDoc *d, int ctrl, const WebLayCfg *c, int avail, int *w, int *h)
{
    const HtmlCtrl *k = &d->ctrls[ctrl];
    int lh = c->line_h[c->font_body & 7];
    *h = lh + CTRL_PAD;
    switch (k->type) {
    case HC_TEXT: case HC_PASSWORD: {
        int want = k->size > 0 ? k->size * 8 : 200;
        if (want < 120) want = 120;
        if (want > 260) want = 260;
        *w = want;
        break;
    }
    case HC_TEXTAREA:
        *w = avail;
        *h = lh * 3 + CTRL_PAD;
        break;
    case HC_CHECKBOX: case HC_RADIO:
        *w = 18;
        *h = lh;
        break;
    case HC_SELECT: {
        int mw = 40;
        for (int i = 0; i < k->opt_n && i < 64; i++) {
            const char *t = HTML_STR(d, d->opts[k->opt_first + i].label);
            const char *p = t;
            int tw = 0;
            while (*p) tw += c->adv(c->font_body, &p);
            if (tw > mw) mw = tw;
        }
        *w = mw + 32;
        break;
    }
    default: { /* buttons */
        const char *t = HTML_STR(d, k->label);
        const char *p = t;
        int tw = 0;
        while (*p) tw += c->adv(c->font_bold, &p);
        *w = tw + 24;
        break;
    }
    }
    if (*w > avail) *w = avail;
    if (*w < 12) *w = 12;
}

/* An inline box (image or control) of w x bh. */
static void box(L *s, const HtmlItem *it, int kind, int idx, int w, int bh, int flags, int link)
{
    line_style(s, it);
    if (s->x + w > s->c->width && s->nline > 0 && !(s->nline == 1 && s->line_has_bullet))
        flush(s, 0);
    line_style(s, it);
    WebSpan *sp = line_add(s);
    sp->kind = (uint8_t)kind;
    sp->x = (short)s->x;
    sp->w = (short)w;
    sp->h = (short)bh;
    sp->ih = (short)bh;
    sp->a = idx;
    sp->link = link;
    sp->flags = (uint8_t)flags;
    sp->font = (uint8_t)s->c->font_body;
    s->x += w + 2;
}

WebLayout *weblay_run(const HtmlDoc *d, const WebLayCfg *c)
{
    L *s = calloc(1, sizeof(L));
    WebLayout *l = calloc(1, sizeof(WebLayout));
    if (!s || !l) {
        free(s);
        free(l);
        return NULL;
    }
    l->anchor_y = calloc(d->nanchors > 0 ? d->nanchors : 1, sizeof(int));
    if (!l->anchor_y) {
        free(s);
        free(l);
        return NULL;
    }
    s->d = d;
    s->c = c;
    s->l = l;
    s->y = 4;
    s->pending_gap = -1;

    for (int i = 0; i < d->nitems && !s->oom; i++) {
        const HtmlItem *it = &d->items[i];
        switch (it->kind) {
        case HI_TEXT:
            text_item(s, it, d->text + it->a, it->b, WS_TEXT);
            break;
        case HI_BREAK:
            flush(s, s->nline == 0);
            if (s->pending_gap < 0) s->pending_gap = 0;
            break;
        case HI_PARA: {
            flush(s, 0);
            if (s->pending_gap < 0) break; /* nothing above yet */
            int g = s_gap[it->a & 3];
            if (s->last_head && s->last_head <= 2 && l->nspans > 0) { /* a line under big headings */
                WebSpan *r = out_span(s);
                if (r) {
                    memset(r, 0, sizeof(*r));
                    r->kind = WS_RULE;
                    r->x = (short)s->left;
                    r->w = (short)(c->width - s->left);
                    r->y = s->y + 1;
                    r->h = 1;
                    r->link = r->focus = -1;
                }
                s->y += 4;
                s->last_head = 0;
            }
            if (g > s->pending_gap) s->pending_gap = g;
            s->left = left_of(it->indent);
            s->x = s->left;
            break;
        }
        case HI_CELL:
            if (s->nline > 0) {
                s->x += CELL_GAP;
                if (s->x > c->width - 20) flush(s, 0);
            }
            break;
        case HI_BULLET: {
            flush(s, 0);
            int font = c->font_body;
            const char *t = d->text + it->a;
            int w = text_w(s, font, t, it->b);
            s->left = left_of(it->indent);
            if (s->left < w + 6) s->left = w + 6;
            s->x = s->left;
            WebSpan *sp = line_add(s);
            sp->kind = WS_BULLET;
            sp->font = (uint8_t)font;
            sp->color = WC_MUTED;
            sp->x = (short)(s->left - w - 5);
            sp->w = (short)w;
            sp->h = (short)line_h(s, font);
            sp->a = it->a;
            sp->b = it->b;
            s->line_has_bullet = 1;
            s->line_flags = it->flags;
            s->line_indent = it->indent;
            break;
        }
        case HI_IMG: {
            const HtmlImg *im = &d->imgs[it->a];
            int state = c->imgs ? c->imgs[it->a].state : WI_PENDING;
            int avail = c->width - left_of(it->indent);
            int w, h;
            weblay_img_size(d, it->a, c, avail, &w, &h);
            const char *alt = HTML_STR(d, im->alt);
            if (state == WI_READY && w > 0 && h > 0) {
                box(s, it, WS_IMG, it->a, w, h, 0, it->link);
            } else if (state == WI_PENDING && w > 0 && h > 0) {
                box(s, it, WS_IMG, it->a, w, h, WSF_PLACEHOLDER, it->link);
            } else if (alt[0]) {
                HtmlItem alt_it = *it;
                alt_it.flags |= HF_MUTED;
                if (state == WI_PENDING && it->link < 0) break; /* may still come: no text that jumps away */
                text_item(s, &alt_it, alt, (int)strlen(alt), WS_ALT);
            } else if (it->link >= 0) {
                box(s, it, WS_IMG, it->a, 28, 20, WSF_PLACEHOLDER, it->link);
            }
            break;
        }
        case HI_CTRL: {
            int w, h;
            weblay_ctrl_size(d, it->a, c, c->width - left_of(it->indent), &w, &h);
            box(s, it, WS_CTRL, it->a, w, h, 0, -1);
            break;
        }
        case HI_RULE: {
            flush(s, 0);
            if (s->pending_gap > 0) s->y += s->pending_gap;
            s->pending_gap = 0;
            WebSpan *r = out_span(s);
            if (r) {
                memset(r, 0, sizeof(*r));
                r->kind = WS_RULE;
                r->x = (short)s->left;
                r->w = (short)(c->width - s->left);
                r->y = s->y + 4;
                r->h = 1;
                r->link = r->focus = -1;
            }
            s->y += 9;
            break;
        }
        case HI_ANCHOR:
            if (it->a < d->nanchors)
                l->anchor_y[it->a] = s->y + (s->pending_gap > 0 ? s->pending_gap : 0);
            break;
        }
        if (it->kind != HI_PARA && it->kind != HI_ANCHOR && s->pending_gap < 0)
            s->pending_gap = 0;
    }
    flush(s, 0);
    l->height = s->y + 8;
    int oom = s->oom;
    free(s);
    if (oom) {
        weblay_free(l);
        return NULL;
    }
    return l;
}

void weblay_free(WebLayout *l)
{
    if (!l) return;
    free(l->spans);
    free(l->focus);
    free(l->anchor_y);
    free(l);
}

int weblay_span_at(const WebLayout *l, int y)
{
    int lo = 0, hi = l->nspans;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (l->spans[m].y + l->spans[m].h <= y) lo = m + 1;
        else hi = m;
    }
    return lo;
}
